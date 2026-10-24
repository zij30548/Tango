#include "conversationclient.h"
#include "appbackendclient.h"
#include "neteaseclient.h"
#include "lookupcontext.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonDocument>
#include <QSaveFile>
#include <QLockFile>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace {
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return pattern.match(id).hasMatch();
}
qint64 integer(const QJsonValue &value, qint64 minimum = 0) {
    const auto number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= minimum &&
        number <= 9007199254740991. && std::floor(number) == number ? qint64(number) : -1;
}
bool localPlaybackPermission(QString instruction, const QJsonObject &parameters) {
    // 本机播放再次核对整句直接请求；只接受声明过的信息/版本分句，未知条件不能授予副作用。
    instruction.remove(QRegularExpression(QStringLiteral("\\s+")));
    instruction.remove(QRegularExpression(QStringLiteral("[。！!？?]+$")));
    const auto clauses = instruction.split(QRegularExpression(QStringLiteral("[，,；;。\\n]+")), Qt::SkipEmptyParts);
    if (clauses.isEmpty()) return false;
    QString artist = parameters.value("artist").toString(), title = parameters.value("title").toString();
    artist.remove(QRegularExpression(QStringLiteral("\\s+"))); title.remove(QRegularExpression(QStringLiteral("\\s+")));
    const auto prefix = QStringLiteral("(?:(?:请你|请|你能|你可以|可以|能不能|能否|麻烦你|麻烦|帮我|帮忙|替我|为我|给我|现在|立即|就在这台电脑|在这台电脑|在本机)){0,5}");
    const auto query = (artist.isEmpty() ? QString() : QRegularExpression::escape(artist) + QStringLiteral("的?")) +
        QStringLiteral("[《“\"']?") + QRegularExpression::escape(title) + QStringLiteral("[》”\"']?");
    const auto version = QStringLiteral("(?:的?(?:录音室版|录音室版本|studio版|现场版|现场版本|live版))?");
    if (!QRegularExpression(QStringLiteral("^") + prefix + QStringLiteral("(?:播放|放一首|放一下|放|听一下|听一首)") +
            query + version + QStringLiteral("(?:好吗|可以吗|吗)?$"), QRegularExpression::CaseInsensitiveOption).match(clauses.first()).hasMatch()) return false;
    for (const auto &clause : clauses.mid(1)) {
        if (QRegularExpression(QStringLiteral("^(?:先|这次|现在)?(?:别|不要|不用|不必|无需)(?:自动)?播放(?:歌曲|它|音乐)?$"))
                .match(clause).hasMatch()) return false;
        const bool versionClause = QRegularExpression(QStringLiteral("^(?:请|用|要|选择)?(?:录音室版|录音室版本|studio版|现场版|现场版本|live版)$"),
            QRegularExpression::CaseInsensitiveOption).match(clause).hasMatch();
        const bool infoClause = QRegularExpression(QStringLiteral("^(?:只|先只|请|帮我)?(?:显示|展示|给我|告诉我|看)(?:一下)?(?:歌名|版本|时长|歌曲信息|信息)(?:(?:和|与|、|及)(?:歌名|版本|时长|信息))*(?:就好|即可|就行)?$"))
            .match(clause).hasMatch();
        if (!versionClause && !infoClause) return false;
    }
    return true;
}
bool validPlan(const QJsonObject &plan, const QString &messageId, const QString &instruction, const QJsonObject &lookupContext) {
    const QSet<QString> intents{"scene_research", "release_research", "explain_result", "preference_proposal",
        "preference_deletion", "task_control", "clarification", "help", "unsupported",
        "song_lookup", "artist_catalog", "playlist_search", "conversation_history"};
    if (integer(plan.value("version")) != 1 || plan.value("messageId").toString() != messageId ||
        !QStringList{"deterministic", "model"}.contains(plan.value("planner").toString()) ||
        !QStringList{"direct", "single_agent", "clarify", "reject"}.contains(plan.value("strategy").toString()) ||
        !plan.value("intents").isArray() || !plan.value("actions").isArray() ||
        !plan.value("missingSlots").isArray() || !plan.value("slots").isObject()) return false;
    QSet<QString> seenIntents;
    for (const auto &value : plan.value("intents").toArray()) {
        if (!value.isString() || !intents.contains(value.toString()) || seenIntents.contains(value.toString())) return false;
        seenIntents.insert(value.toString());
    }
    if (seenIntents.isEmpty()) return false;
    const auto routeSlots = plan.value("slots").toObject();
    if (!routeSlots.value("artist").isString() || routeSlots.value("artist").toString().size() > 100 ||
        integer(routeSlots.value("targetMinutes")) < 0 || integer(routeSlots.value("targetMinutes")) > 240) return false;
    if (routeSlots.contains("targetTrackCount") &&
        (integer(routeSlots.value("targetTrackCount")) < 0 || integer(routeSlots.value("targetTrackCount")) > 30)) return false;
    if (plan.value("missingSlots").toArray().size() > 4) return false;
    for (const auto &value : plan.value("missingSlots").toArray()) {
        const auto missing = value.toObject();
        if (!value.isObject() || !intents.contains(missing.value("intent").toString()) ||
            !missing.value("slot").isString() || missing.value("slot").toString().isEmpty() || missing.value("slot").toString().size() > 80 ||
            !missing.value("question").isString() || missing.value("question").toString().trimmed().isEmpty() || missing.value("question").toString().size() > 600) return false;
    }
    const auto actions = plan.value("actions").toArray();
    if (actions.isEmpty() || actions.size() > 4) return false;
    QHash<QString, QJsonObject> indexed;
    int musicCount = 0;
    for (const auto &value : actions) {
        const auto action = value.toObject();
        const auto id = action.value("id").toString(), source = action.value("source").toString();
        if (!value.isObject() || !validId(id) || indexed.contains(id) || !action.value("source").isString() ||
            source.trimmed().isEmpty() || source.size() > 1000 || !instruction.contains(source) ||
            !action.value("dependsOn").isArray() || action.value("dependsOn").toArray().size() > 3 ||
            !action.value("parameters").isObject()) return false;
        const auto capability = action.value("capability").toString();
        const auto parameters = action.value("parameters").toObject();
        if (capability == "music.task") {
            const auto mode = parameters.value("mode").toString();
            if (++musicCount > 1 || !QStringList{"new", "continue"}.contains(mode) ||
                (mode == "new" && parameters.size() != 1) ||
                (mode == "continue" && (parameters.size() != 3 || !validId(parameters.value("taskId").toString()) ||
                    integer(parameters.value("expectedRevision"), 1) < 1 || integer(parameters.value("expectedRevision"), 1) > 2147483647))) return false;
        } else if (capability == "music.artist_catalog" || capability == "music.playlist_search") {
            // 只读目录搜索绑定当前原文及单一目标，不能携带播放、URL或额外写入参数。
            const bool catalog = capability == "music.artist_catalog";
            const auto field = catalog ? QStringLiteral("artist") : QStringLiteral("keyword");
            const auto intent = catalog ? QStringLiteral("artist_catalog") : QStringLiteral("playlist_search");
            const auto query = parameters.value(field).toString();
            if (++musicCount > 1 || actions.size() != 1 || parameters.size() != 2 ||
                !seenIntents.contains(intent) || !parameters.value(field).isString() || query.trimmed().isEmpty() ||
                query.size() > (catalog ? 100 : 200) || !instruction.contains(query) ||
                integer(parameters.value("limit"), 1) < 1 || integer(parameters.value("limit"), 1) > (catalog ? 30 : 5) ||
                !action.value("dependsOn").toArray().isEmpty()) return false;
        } else if (capability == "music.lookup") {
            if (++musicCount > 1 || (parameters.size() != 3 && parameters.size() != 4) ||
                (parameters.size() == 4 && (!parameters.value("version").isString() ||
                    !QStringList{"studio", "live"}.contains(parameters.value("version").toString()))) ||
                !parameters.value("artist").isString() || parameters.value("artist").toString().size() > 100 ||
                !parameters.value("title").isString() || parameters.value("title").toString().trimmed().isEmpty() ||
                parameters.value("title").toString().size() > 200 || !parameters.value("play").isBool() ||
                (parameters.value("artist").toString() + QLatin1Char(' ') + parameters.value("title").toString()).trimmed().size() > 200 ||
                !instruction.contains(parameters.value("title").toString()) ||
                (!parameters.value("artist").toString().isEmpty() && !instruction.contains(parameters.value("artist").toString()))) return false;
            // 播放属于本机副作用，除后台契约外再核对当前原话，历史与工具文本不能授权。
            if (parameters.value("play").toBool() && !localPlaybackPermission(instruction, parameters)) return false;
            if (parameters.contains("version")) {
                const auto label = parameters.value("version").toString() == "studio"
                    ? QStringLiteral("录音室|studio") : QStringLiteral("现场|live");
                if (!QRegularExpression(label, QRegularExpression::CaseInsensitiveOption).match(instruction).hasMatch()) return false;
            }
        } else if (capability == "music.refine") {
            // 候选续接不能与其他能力混合，更不能借历史查询携带播放或写记忆权限。
            if (++musicCount > 1 || actions.size() != 1 || !seenIntents.contains("song_lookup")) return false;
            auto context = lookupContext;
            if (context.isEmpty() && parameters.value("operation").toString() == "artist")
                context.insert("rows", QJsonArray{QJsonObject{{"artist", parameters.value("artist")}}});
            if (!LookupContext::validParameters(parameters, instruction, context)) return false;
            if (!lookupContext.isEmpty())
                for (const auto &key : QStringList{"candidateSetId", "originMessageId", "originActionId"})
                    if (parameters.value(key) != lookupContext.value(key)) return false;
        } else if (capability == "memory.propose") {
            const auto operation = parameters.value("operation").toString();
            if (parameters.size() != 2 || !QStringList{"set", "clear"}.contains(operation) ||
                !parameters.value("preference").isString() || parameters.value("preference").toString().size() > 500 ||
                (operation == "set" && parameters.value("preference").toString().trimmed().isEmpty()) ||
                (operation == "clear" && !parameters.value("preference").toString().isEmpty())) return false;
        } else if (capability == "task.control") {
            if (parameters.size() != 3 || !QStringList{"pause", "cancel", "resume", "undo"}.contains(parameters.value("action").toString()) ||
                !validId(parameters.value("taskId").toString()) || integer(parameters.value("expectedRevision"), 1) < 1 ||
                integer(parameters.value("expectedRevision"), 1) > 2147483647) return false;
        } else if (capability == "assistant.reply") {
            if (!QStringList{"help", "explain", "clarify", "unsupported", "history"}.contains(parameters.value("kind").toString()) ||
                !parameters.value("text").isString() || parameters.value("text").toString().trimmed().isEmpty() || parameters.value("text").toString().size() > 1600 ||
                (parameters.size() != 2 && parameters.size() != 3)) return false;
            if (parameters.size() == 3 && (parameters.value("kind").toString() != "explain" ||
                integer(parameters.value("trackIndex")) < 0 || integer(parameters.value("trackIndex")) > 30)) return false;
        } else return false;
        indexed.insert(id, action);
    }
    QSet<QString> complete;
    for (int pass = 0; pass < indexed.size(); ++pass)
        for (auto it = indexed.cbegin(); it != indexed.cend(); ++it) {
            bool ready = true; QSet<QString> unique;
            for (const auto &dependency : it->value("dependsOn").toArray()) {
                if (!dependency.isString() || !indexed.contains(dependency.toString()) || dependency.toString() == it.key() || unique.contains(dependency.toString())) return false;
                unique.insert(dependency.toString());
                if (!complete.contains(dependency.toString())) ready = false;
            }
            if (ready) complete.insert(it.key());
        }
    return complete.size() == indexed.size();
}

}

ConversationClient::ConversationClient(QObject *parent, AppBackendClient *service)
    : QObject(parent), backend(service ? service : AppBackendClient::instance()) {
    QDir().mkpath(NeteaseClient::dataDirectory());
    const auto settingsPath = QDir(NeteaseClient::dataDirectory()).filePath(QStringLiteral("music-task-device.ini"));
    QSettings settings(settingsPath, QSettings::IniFormat);
    installationId = settings.value(QStringLiteral("musicTasks/deviceId")).toString();
    static const QRegularExpression devicePattern(QStringLiteral("^[A-Za-z0-9_-]{16,80}$"));
    if (!devicePattern.match(installationId).hasMatch()) {
        installationId = newId();
        settings.setValue(QStringLiteral("musicTasks/deviceId"), installationId);
        settings.sync();
        QFile::setPermissions(settingsPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    accountIdentity = backend->serviceIdentity() + QLatin1Char(':') + backend->userId();
    loadClearRequest();
    connect(backend, &AppBackendClient::authChanged, this, [this] {
        // Even signing into the same user again invalidates in-flight delivery.
        detach();
        accountIdentity = backend->serviceIdentity() + QLatin1Char(':') + backend->userId();
        loadClearRequest();
        emit snapshotChanged(snapshot());
    });
}
ConversationClient::~ConversationClient() { resetOperations(); }
QString ConversationClient::deviceId() const { return installationId; }
bool ConversationClient::busy() const { return requestBusy; }
bool ConversationClient::available() const { return backend->conversationAvailable(); }
bool ConversationClient::clearing() const { return clearingConversations; }
QJsonObject ConversationClient::snapshot() const {
    auto visible = state;
    if (!pendingId.isEmpty()) {
        visible.insert(QStringLiteral("pendingMessageId"), pendingId);
        visible.insert(QStringLiteral("pendingInstruction"), pendingInstruction);
    }
    visible.insert(QStringLiteral("journalPending"), !outbox.isEmpty());
    visible.insert(QStringLiteral("clearing"), clearingConversations);
    visible.insert(QStringLiteral("clearPending"), !clearRequestId.isEmpty() || !clearPersistenceError.isEmpty());
    return visible;
}
void ConversationClient::setBusy(bool value) {
    if (requestBusy == value) return;
    requestBusy = value; emit busyChanged(value);
}
void ConversationClient::resetOperations() {
    ++generation; ++operationEpoch;
    if (operation) delete operation.data();
    if (eventOperation) delete eventOperation.data();
    if (routeOperation) delete routeOperation.data();
    routeOperation = nullptr; routingMessage.clear(); routingLookupContext = {}; restoringRoute = false;
    operation = nullptr; eventOperation = nullptr;
    setBusy(false);
}
void ConversationClient::detach() {
    resetOperations(); state = {}; pendingId.clear(); pendingInstruction.clear(); createId.clear();
    clearingConversations = false; clearRequestId.clear(); clearPersistenceError.clear();
    accepted.clear(); routerAccepted.clear(); routesStarted.clear(); routesDelivered.clear(); routesFinished.clear(); completedEvents.clear(); outbox.clear(); eventOrder.clear(); eventFailures = 0;
    emit snapshotChanged(snapshot());
}
bool ConversationClient::current(quint64 epoch) const {
    return epoch == operationEpoch && backend->authenticated() &&
        accountIdentity == backend->serviceIdentity() + QLatin1Char(':') + backend->userId();
}
bool ConversationClient::acceptSnapshot(const QJsonObject &response, bool earlier) {
    const auto value = response.value(QStringLiteral("conversation"));
    if (value.isNull()) return true;
    if (!value.isObject()) return false;
    auto incoming = value.toObject();
    const auto id = incoming.value(QStringLiteral("id")).toString();
    const auto version = integer(incoming.value(QStringLiteral("version")));
    if (!validId(id) || incoming.value(QStringLiteral("deviceId")).toString() != installationId ||
        version < 0 || !incoming.value(QStringLiteral("messages")).isArray() ||
        incoming.value(QStringLiteral("messages")).toArray().size() > 500) return false;
    if (!state.isEmpty() && state.value(QStringLiteral("id")).toString() != id) return false;
    if (version < integer(state.value(QStringLiteral("version")))) return true;
    QMap<qint64, QJsonObject> sequence;
    QSet<QString> ids;
    const auto add = [&sequence, &ids](const QJsonArray &messages) {
        for (const auto &value : messages) {
            if (!value.isObject()) return false;
            const auto message = value.toObject();
            const auto id = message.value(QStringLiteral("id")).toString();
            const auto number = integer(message.value(QStringLiteral("sequence")), 1);
            const auto role = message.value(QStringLiteral("role")).toString();
            const auto content = message.value(QStringLiteral("content"));
            const auto source = message.value(QStringLiteral("source")).toString();
            if (!validId(id) || number < 1 || (role != QStringLiteral("user") &&
                role != QStringLiteral("assistant")) ||
                !content.isString() || content.toString().isEmpty() ||
                content.toString().size() > (role == QStringLiteral("user") ? 1000 : 1600) ||
                !validId(message.value(QStringLiteral("turnId")).toString()) ||
                (role == QStringLiteral("user") && source != QStringLiteral("user")) ||
                (role == QStringLiteral("assistant") && source != QStringLiteral("device_report") && source != QStringLiteral("task") && source != QStringLiteral("route"))) return false;
            if (ids.contains(id)) {
                bool identical = false;
                for (auto it = sequence.cbegin(); it != sequence.cend(); ++it)
                    if (it->value(QStringLiteral("id")).toString() == id) {
                        identical = it.key() == number && *it == message; break;
                    }
                if (!identical) return false;
                continue;
            }
            if (sequence.contains(number) && sequence.value(number).value(QStringLiteral("id")).toString() != id)
                return false;
            sequence.insert(number, message); ids.insert(id);
        }
        return true;
    };
    if (!add(state.value(QStringLiteral("messages")).toArray()) ||
        !add(incoming.value(QStringLiteral("messages")).toArray())) return false;
    while (sequence.size() > 500) sequence.erase(sequence.begin());
    QJsonArray messages;
    for (const auto &message : sequence) messages.append(message);
    incoming.insert(QStringLiteral("messages"), messages);
    if (!sequence.isEmpty()) {
        // Page from the beginning of the contiguous tail. If the server added
        // more than one response window while we were offline, older loaded
        // messages must not hide the intervening gap.
        auto cursor = sequence.lastKey();
        while (cursor > 1 && sequence.contains(cursor - 1)) --cursor;
        incoming.insert(QStringLiteral("earliestSequence"), double(cursor));
        incoming.insert(QStringLiteral("hasEarlier"), cursor > 1);
    }
    Q_UNUSED(earlier);
    state = incoming;
    emit snapshotChanged(snapshot());
    return true;
}
void ConversationClient::restoreLatest() {
    if (!available() || busy() || !pendingId.isEmpty() || !clearRequestId.isEmpty() || !clearPersistenceError.isEmpty()) return;
    setBusy(true); const auto epoch = ++operationEpoch;
    if (operation) delete operation.data();
    operation = new QObject(this);
    backend->latestConversation(installationId, operation,
        [this, epoch](const QJsonObject &response, const QString &failure) {
            if (!current(epoch)) return;
            setBusy(false);
            if (!failure.isEmpty()) { emit error(failure); return; }
            if (!acceptSnapshot(response)) emit error(tr("会话记录格式无效，已保留现有消息。"));
            // Deliberately no messageAccepted: history is not authorization.
            flushEvents();
        });
}
void ConversationClient::submit(const QString &text) {
    const auto instruction = text.trimmed();
    if (busy()) return;
    if (!clearRequestId.isEmpty() || !clearPersistenceError.isEmpty()) {
        emit submissionFailed(text, tr("上次清除的结果尚未核对。请再次点击清除全部对话，核对原操作后再发送。")); return;
    }
    if (!available()) { emit submissionFailed(text, tr("请先登录并连接支持会话的探歌后台。")); return; }
    if (instruction.isEmpty() || instruction.size() > 1000) {
        emit submissionFailed(text, tr("请输入 1–1000 字的需求。")); return;
    }
    if (!pendingId.isEmpty() && pendingInstruction != instruction) {
        emit submissionFailed(text, tr("上一条消息的提交结果尚未确认，请先重试原消息。")); return;
    }
    if (pendingId.isEmpty()) { pendingId = newId(); pendingInstruction = instruction; }
    setBusy(true); emit snapshotChanged(snapshot());
    const auto epoch = ++operationEpoch;
    if (operation) delete operation.data();
    operation = new QObject(this);
    if (state.value(QStringLiteral("id")).toString().isEmpty()) {
        if (createId.isEmpty()) createId = newId();
        backend->createConversation({{QStringLiteral("deviceId"), installationId},
            {QStringLiteral("requestId"), createId}}, operation,
            [this, epoch](const QJsonObject &response, const QString &failure) {
                if (!current(epoch)) return;
                if (!failure.isEmpty()) {
                    const auto status = response.value(QStringLiteral("httpStatus")).toInt();
                    if (status >= 400 && status < 500 && status != 408 && status != 429) {
                        const auto instruction = pendingInstruction;
                        pendingId.clear(); pendingInstruction.clear(); createId.clear(); setBusy(false);
                        emit snapshotChanged(snapshot()); emit submissionFailed(instruction, failure); return;
                    }
                    submissionError(failure); return;
                }
                if (!acceptSnapshot(response) || state.value(QStringLiteral("id")).toString().isEmpty()) {
                    submissionError(tr("后台未确认创建会话，原消息仍然保留。")); return;
                }
                createId.clear(); postPending(epoch);
            });
    } else postPending(epoch);
}
void ConversationClient::postPending(quint64 epoch) {
    const auto body = QJsonObject{{QStringLiteral("deviceId"), installationId},
        {QStringLiteral("messageId"), pendingId}, {QStringLiteral("content"), pendingInstruction}};
    backend->appendConversationMessage(state.value(QStringLiteral("id")).toString(), body, operation,
        [this, epoch](const QJsonObject &response, const QString &failure) {
            if (!current(epoch)) return;
            if (failure.isEmpty() && acceptReceipt(response)) return;
            const auto status = response.value(QStringLiteral("httpStatus")).toInt();
            if (!failure.isEmpty() && status >= 400 && status < 500 && status != 408 && status != 429) {
                const auto instruction = pendingInstruction;
                pendingId.clear(); pendingInstruction.clear(); setBusy(false);
                emit snapshotChanged(snapshot()); emit submissionFailed(instruction, failure); return;
            }
            // A timeout may follow a successful commit. Only read the receipt;
            // never silently start another business request or message ID.
            queryPending(epoch, failure.isEmpty() ? tr("消息回执格式无效。") : failure);
        });
}
void ConversationClient::queryPending(quint64 epoch, const QString &failure) {
    if (!current(epoch) || pendingId.isEmpty()) return;
    backend->conversationMessage(state.value(QStringLiteral("id")).toString(), pendingId, operation,
        [this, epoch, failure](const QJsonObject &response, const QString &lookupFailure) {
            if (!current(epoch)) return;
            if (lookupFailure.isEmpty() && acceptReceipt(response)) return;
            submissionError(failure + tr(" 原消息已保留；确认网络恢复后可重试，系统会沿用原消息编号。"));
        });
}
bool ConversationClient::acceptReceipt(const QJsonObject &response) {
    if (pendingId.isEmpty()) return false;
    const auto epoch = generation;
    const auto receipt = response.value(QStringLiteral("receipt")).toObject();
    const auto id = receipt.value(QStringLiteral("messageId")).toString();
    const auto hint = receipt.value(QStringLiteral("dispatchHint")).toString();
    if (id != pendingId || receipt.value(QStringLiteral("status")).toString() != QStringLiteral("accepted") ||
        receipt.value(QStringLiteral("turnId")).toString() != id ||
        (hint != QStringLiteral("task") && hint != QStringLiteral("legacy_assistant") && hint != QStringLiteral("router")) ||
        !acceptSnapshot(response) || epoch != generation) return false;
    bool found = false;
    for (const auto &value : state.value(QStringLiteral("messages")).toArray()) {
        const auto message = value.toObject();
        if (message.value(QStringLiteral("id")).toString() == id &&
            message.value(QStringLiteral("role")).toString() == QStringLiteral("user") &&
            message.value(QStringLiteral("content")).toString() == pendingInstruction &&
            message.value(QStringLiteral("turnId")).toString() == id) found = true;
    }
    if (!found) return false;
    const auto instruction = pendingInstruction;
    pendingId.clear(); pendingInstruction.clear(); setBusy(false);
    emit snapshotChanged(snapshot());
    if (!accepted.contains(id)) {
        accepted.insert(id);
        if (hint == QStringLiteral("router")) routerAccepted.insert(id);
        emit messageAccepted(id, instruction, hint);
    }
    flushEvents();
    return true;
}
void ConversationClient::submissionError(const QString &failure) {
    setBusy(false); emit snapshotChanged(snapshot()); emit submissionFailed(pendingInstruction, failure);
}
void ConversationClient::enqueueEvent(const QJsonObject &event) {
    if (!available() || state.isEmpty() || clearingConversations || !clearRequestId.isEmpty()) return;
    const auto id = event.value(QStringLiteral("eventId")).toString();
    if (completedEvents.contains(id)) return;
    if (!outbox.contains(id)) { outbox.insert(id, event); eventOrder.append(id); }
    emit snapshotChanged(snapshot()); flushEvents();
}
void ConversationClient::appendLocalReply(const QString &turnId, const QString &text) {
    if (!validId(turnId) || text.trimmed().isEmpty() || text.size() > 1600) return;
    const auto id = QStringLiteral("reply-") + QString::fromLatin1(QCryptographicHash::hash(
        (turnId + QLatin1Char('\n') + text).toUtf8(), QCryptographicHash::Sha256).toHex());
    enqueueEvent({{QStringLiteral("deviceId"), installationId}, {QStringLiteral("eventId"), id},
        {QStringLiteral("turnId"), turnId}, {QStringLiteral("kind"), QStringLiteral("local_reply")},
        {QStringLiteral("content"), text}});
}
void ConversationClient::syncTask(const QString &turnId, const QString &taskId, int revision, qint64 stateVersion) {
    if (!validId(turnId) || !validId(taskId) || revision < 1 || stateVersion < 0) return;
    // A running task may append replies without changing its turn revision.
    // Include stateVersion; the server also deduplicates source message IDs.
    const auto eventId = QStringLiteral("sync-") + QString::fromLatin1(QCryptographicHash::hash(
        (turnId + QLatin1Char(':') + taskId + QLatin1Char(':') + QString::number(revision) +
            QLatin1Char(':') + QString::number(stateVersion)).toUtf8(),
        QCryptographicHash::Sha256).toHex());
    enqueueEvent({{QStringLiteral("deviceId"), installationId}, {QStringLiteral("eventId"), eventId},
        {QStringLiteral("turnId"), turnId}, {QStringLiteral("kind"), QStringLiteral("task_sync")},
        {QStringLiteral("taskId"), taskId}, {QStringLiteral("taskRevision"), revision}});
}
void ConversationClient::flushEvents() {
    if (eventOperation || eventOrder.isEmpty() || !available() || state.isEmpty() || clearingConversations || !clearRequestId.isEmpty()) return;
    const auto id = eventOrder.first();
    const auto epoch = generation;
    eventOperation = new QObject(this);
    backend->appendConversationEvent(state.value(QStringLiteral("id")).toString(), outbox.value(id), eventOperation,
        [this, epoch, id](const QJsonObject &response, const QString &failure) {
            if (epoch != generation) return;
            if (eventOperation) eventOperation->deleteLater(); eventOperation = nullptr;
            const auto receipt = response.value(QStringLiteral("receipt")).toObject();
            if (failure.isEmpty() && receipt.value(QStringLiteral("eventId")).toString() == id &&
                receipt.value(QStringLiteral("status")).toString() == QStringLiteral("recorded") && acceptSnapshot(response)) {
                outbox.remove(id); eventOrder.removeAll(id); completedEvents.insert(id); eventFailures = 0;
                if (completedEvents.size() > 1000) completedEvents.clear();
                emit snapshotChanged(snapshot()); flushEvents();
            } else {
                ++eventFailures;
                const auto status = response.value(QStringLiteral("httpStatus")).toInt();
                if (status >= 400 && status < 500 && status != 408 && status != 429) {
                    outbox.remove(id); eventOrder.removeAll(id); eventFailures = 0;
                    emit error(tr("后台拒绝同步此回复：%1 请刷新或开启新会话，已完成的业务不会重复执行。").arg(failure));
                    emit snapshotChanged(snapshot()); flushEvents(); return;
                }
                emit error(tr("对话回复尚未同步，系统保留了原事件编号。"));
                // Event writes are idempotent and have no model/playback side
                // effects, so one automatic same-ID resend is safe.
                if (eventFailures < 2) QTimer::singleShot(250, this, [this, epoch] {
                    if (epoch == generation) flushEvents();
                });
            }
        });
}
void ConversationClient::loadEarlier() {
    if (!available() || busy() || state.isEmpty() || !clearRequestId.isEmpty() || !state.value(QStringLiteral("hasEarlier")).toBool()) return;
    const auto before = integer(state.value(QStringLiteral("earliestSequence")), 1);
    if (before < 1) return;
    setBusy(true); const auto epoch = ++operationEpoch;
    if (operation) delete operation.data(); operation = new QObject(this);
    backend->conversationBefore(state.value(QStringLiteral("id")).toString(), before, operation,
        [this, epoch](const QJsonObject &response, const QString &failure) {
            if (!current(epoch)) return;
            setBusy(false);
            if (!failure.isEmpty()) emit error(failure);
            else if (!acceptSnapshot(response, true)) emit error(tr("更早消息格式无效。"));
        });
}
void ConversationClient::startNew() {
    if (busy() || !pendingId.isEmpty() || !outbox.isEmpty() || !clearRequestId.isEmpty() || !clearPersistenceError.isEmpty()) {
        emit error(tr("请先确认待提交消息及回复同步，再开启新会话。")); return;
    }
    detach();
}

QString ConversationClient::clearRequestPath() const {
    // 只保存清除操作编号；账号与后台分别命名，不写Bearer或聊天正文。
    const auto key = QCryptographicHash::hash(accountIdentity.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(NeteaseClient::dataDirectory()).filePath(QStringLiteral("conversation-clear-%1.json").arg(QString::fromLatin1(key)));
}
bool ConversationClient::loadClearRequest() {
    clearRequestId.clear(); clearPersistenceError.clear();
    if (!backend->authenticated()) return true;
    const QFileInfo info(clearRequestPath());
    if (!info.exists() && !info.isSymLink()) return true;
    QFile file(info.absoluteFilePath());
    if (info.isSymLink() || !info.isFile() || info.size() > 4096 || !file.open(QIODevice::ReadOnly)) {
        clearPersistenceError = tr("无法读取本机清除回执，未提交新的清除操作。"); return false;
    }
    QJsonParseError error;
    const auto value = QJsonDocument::fromJson(file.readAll(), &error);
    const auto id = value.object().value(QStringLiteral("requestId")).toString();
    if (error.error != QJsonParseError::NoError || !value.isObject() || value.object().size() != 1 || !validId(id)) {
        clearPersistenceError = tr("本机清除回执格式无效，未提交新的清除操作。"); return false;
    }
    clearRequestId = id; return true;
}
bool ConversationClient::saveClearRequest() {
    QSaveFile file(clearRequestPath());
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto body = QJsonDocument(QJsonObject{{QStringLiteral("requestId"), clearRequestId}}).toJson(QJsonDocument::Compact);
    return file.write(body) == body.size() && file.commit();
}
void ConversationClient::clearAll() {
    if (clearingConversations) return;
    if (!available() || !backend->authenticated()) { emit clearFailed(tr("请先连接当前探歌账号，再清除对话。")); return; }
    // 多窗口共享本机目录时串行读写回执；只在文件操作期间持锁，不等待网络。
    QLockFile receiptLock(clearRequestPath() + QStringLiteral(".lock"));
    if (!receiptLock.tryLock(0)) { emit clearFailed(tr("另一窗口正在整理清除回执，请稍后再次核对。")); return; }
    const auto existingPending = clearRequestId;
    if (!loadClearRequest()) { emit clearFailed(clearPersistenceError); return; }
    if (clearRequestId.isEmpty()) {
        // 别的窗口可能已核对并删除落盘文件；本窗口仍未知时只能重放原ID，不能清除后来新建的聊天。
        clearRequestId = existingPending.isEmpty() ? newId() : existingPending;
        if (!saveClearRequest()) {
            clearRequestId = existingPending;
            emit clearFailed(tr("无法保存清除操作编号，未发送新请求；请检查本机存储后再次核对。")); return;
        }
    }
    receiptLock.unlock();
    // 先废止所有旧网络回调；清除成功前仍保留原历史，绝不把本机隐藏当作后台删除。
    resetOperations(); clearingConversations = true;
    const auto epoch = ++operationEpoch;
    const auto requestId = clearRequestId;
    operation = new QObject(this); setBusy(true); emit clearStarted();
    backend->clearConversations({{QStringLiteral("requestId"), requestId}}, operation,
        [this, epoch, requestId](const QJsonObject &response, const QString &failure) {
            if (!current(epoch) || !clearingConversations || clearRequestId != requestId) return;
            const auto count = integer(response.value(QStringLiteral("conversationCount")));
            const auto cancelled = integer(response.value(QStringLiteral("cancelledTaskCount")));
            const auto clearedAt = QDateTime::fromString(response.value(QStringLiteral("clearedAt")).toString(), Qt::ISODate);
            if (!failure.isEmpty() || response.value(QStringLiteral("cleared")) != QJsonValue(true) ||
                response.value(QStringLiteral("requestId")).toString() != requestId || count < 0 || count > 2147483647 ||
                cancelled < 0 || !clearedAt.isValid()) {
                clearingConversations = false; setBusy(false); emit snapshotChanged(snapshot());
                emit clearFailed(tr("清除结果尚未确认，原消息仍保留；请再次点击清除以核对同一次操作。%1")
                    .arg(failure.isEmpty() ? QString() : QLatin1Char(' ') + failure)); return;
            }
            QLockFile receiptLock(clearRequestPath() + QStringLiteral(".lock"));
            if (!receiptLock.tryLock(0)) {
                clearingConversations = false; setBusy(false);
                emit clearFailed(tr("后台已确认清除，但另一窗口正在整理本机回执；请再次点击清除核对。")); return;
            }
            if (!loadClearRequest() || (!clearRequestId.isEmpty() && clearRequestId != requestId)) {
                receiptLock.unlock(); clearingConversations = false; setBusy(false); emit snapshotChanged(snapshot());
                emit clearFailed(tr("原清除已确认，但本机另有待核对的清除操作；已保留其编号，请再次点击清除核对。")); return;
            }
            if (QFile::exists(clearRequestPath()) && !QFile::remove(clearRequestPath())) {
                receiptLock.unlock();
                clearingConversations = false; setBusy(false);
                emit clearFailed(tr("后台对话已清除，但本机回执未能整理；请再次点击清除核对，暂未开始新对话。")); return;
            }
            receiptLock.unlock();
            detach(); emit conversationsCleared(int(count));
        });
}

void ConversationClient::route(const QString &messageId, const QString &preference, const QJsonObject &lookupContext) {
    if (clearingConversations || !clearRequestId.isEmpty()) return;
    if (!backend->routingAvailable() || !routerAccepted.contains(messageId) || preference.size() > 500 ||
        (!lookupContext.isEmpty() && !LookupContext::valid(lookupContext))) {
        emit routeFailed(messageId, tr("此消息未取得统一路由授权，未执行任何能力。")); return;
    }
    if (routesDelivered.contains(messageId) || routesFinished.contains(messageId)) return;
    if (!routingMessage.isEmpty()) { emit routeFailed(messageId, tr("上一条需求仍在理解，请稍候。")); return; }
    if (routesStarted.contains(messageId)) { observeRoute(messageId); return; }
    restoringRoute = false; routingLookupContext = lookupContext;
    routesStarted.insert(messageId); routingMessage = messageId; routeObservations = 0;
    setBusy(true); const auto epoch = generation;
    if (routeOperation) delete routeOperation.data(); routeOperation = new QObject(this);
    QJsonObject body{{"deviceId", installationId}, {"preference", preference}};
    if (!lookupContext.isEmpty()) body.insert("lookupContext", lookupContext);
    backend->routeConversationMessage(state.value("id").toString(), messageId, body, routeOperation,
        [this, messageId, epoch](const QJsonObject &response, const QString &failure) {
            receiveRoute(messageId, epoch, response, failure, true);
        });
}
void ConversationClient::observeRoute(const QString &messageId) {
    if (clearingConversations || !clearRequestId.isEmpty() || !backend->routingAvailable() || !routeNeedsObservation(messageId) ||
        !routingMessage.isEmpty()) return;
    restoringRoute = false;
    routingMessage = messageId; routeObservations = 0; setBusy(true);
    if (routeOperation) delete routeOperation.data(); routeOperation = new QObject(this);
    queryRoute(messageId, generation);
}
bool ConversationClient::routeNeedsObservation(const QString &messageId) const {
    return routesStarted.contains(messageId) && !routesDelivered.contains(messageId) && !routesFinished.contains(messageId);
}
void ConversationClient::restoreRoute(const QString &messageId) {
    if (clearingConversations || !clearRequestId.isEmpty()) return;
    routingLookupContext = {};
    if (!validId(messageId) || !routingMessage.isEmpty()) return;
    if (!available() || !backend->routingAvailable()) {
        emit routeFailed(messageId, tr("后台原路由读取能力不可用，旧操作已暂停恢复。")); return;
    }
    bool ownedMessage = false;
    for (const auto &value : state.value("messages").toArray()) {
        const auto message = value.toObject();
        if (message.value("id").toString() == messageId && message.value("role").toString() == "user") ownedMessage = true;
    }
    if (!ownedMessage) { emit routeFailed(messageId, tr("原消息不在当前会话窗口，未恢复旧操作。")); return; }
    restoringRoute = true; routingMessage = messageId; routeObservations = 0; setBusy(true);
    if (routeOperation) delete routeOperation.data(); routeOperation = new QObject(this);
    queryRoute(messageId, generation);
}
void ConversationClient::queryRoute(const QString &messageId, quint64 epoch) {
    if (epoch != generation || routingMessage != messageId || !backend->authenticated()) return;
    ++routeObservations;
    backend->conversationMessageRoute(state.value("id").toString(), messageId, routeOperation,
        [this, messageId, epoch](const QJsonObject &response, const QString &failure) {
            receiveRoute(messageId, epoch, response, failure);
        });
}
void ConversationClient::receiveRoute(const QString &messageId, quint64 epoch, const QJsonObject &response,
                                      const QString &failure, bool originalSubmission) {
    if (epoch != generation || routingMessage != messageId || !backend->authenticated()) return;
    const auto finishFailure = [this, &messageId](const QString &message) {
        routingMessage.clear(); setBusy(false); emit routeFailed(messageId, message);
    };
    if (!failure.isEmpty()) {
        const auto status = response.value("httpStatus").toInt();
        const auto code = response.value("errorCode").toString();
        // 只认原POST的两个明确提交前候选错误；补查404、幂等冲突和传输未知不能冒充未执行。
        if (originalSubmission && !routingLookupContext.isEmpty() && response.value("completeHttp").toBool() &&
            ((status == 400 && code == "INVALID_LOOKUP_CONTEXT") || (status == 409 && code == "LOOKUP_CONTEXT_STALE"))) {
            routesFinished.insert(messageId);
            finishFailure(tr("后台已拒绝本次候选筛选：这批候选已失效或无法核验。请明确歌名重新搜索；没有执行筛选、播放或自动重试。"));
            return;
        }
        // POST超时可能发生在持久提交之后；未知结果只GET原路由，不能新增模型尝试。
        if (routeObservations < 2) {
            QTimer::singleShot(500, this, [this, messageId, epoch] { queryRoute(messageId, epoch); }); return;
        }
        finishFailure(tr("路由状态暂未确认；可稍后观察原消息，不会自动重试模型。")); return;
    }
    const auto route = response.value("route").toObject();
    if (!validId(route.value("id").toString()) || route.value("messageId").toString() != messageId) {
        finishFailure(tr("路由回执无效，未执行任何能力。")); return;
    }
    const auto status = route.value("status").toString();
    if (status == "running") {
        if (!acceptSnapshot(response)) { finishFailure(tr("路由会话格式无效。")); return; }
        if (routeObservations < 15) {
            QTimer::singleShot(1000, this, [this, messageId, epoch] { queryRoute(messageId, epoch); }); return;
        }
        finishFailure(tr("需求仍在处理中；可稍后观察原消息，不会再次调用模型。")); return;
    }
    if (status != "completed") {
        if (status == "failed") {
            if (!acceptSnapshot(response)) { finishFailure(tr("路由会话格式无效，尚未确认原请求终态。")); return; }
            routesFinished.insert(messageId);
            const auto code = route.value("errorCode").toString();
            const bool safeCode = QRegularExpression(QStringLiteral("^[A-Z][A-Z0-9_]{0,79}$")).match(code).hasMatch();
            finishFailure(tr("后台已拒绝本次需求%1，未执行相关能力。请调整表达后重新提出；不会自动重试。")
                .arg(safeCode ? QStringLiteral("（%1）").arg(code) : QString())); return;
        }
        if (status == "uncertain") acceptSnapshot(response);
        finishFailure(tr("原路由结果尚未确认；可稍后观察原请求，不会重发或执行其他能力。")); return;
    }
    QString instruction;
    for (const auto &value : state.value("messages").toArray()) {
        const auto message = value.toObject();
        if (message.value("id").toString() == messageId && message.value("role").toString() == "user") instruction = message.value("content").toString();
    }
    const auto plan = route.value("result").toObject();
    if (instruction.isEmpty() || !route.value("result").isObject() || !validPlan(plan, messageId, instruction, routingLookupContext) ||
        !acceptSnapshot(response)) {
        routesFinished.insert(messageId);
        finishFailure(tr("路由方案未通过类型或授权范围校验，未执行任何能力。")); return;
    }
    routesFinished.insert(messageId);
    const bool restored = restoringRoute;
    restoringRoute = false; routingMessage.clear(); setBusy(false);
    if (restored) { emit routeRestored(messageId, plan); return; }
    if (!routesDelivered.contains(messageId)) { routesDelivered.insert(messageId); emit routeReady(messageId, plan); }
}
