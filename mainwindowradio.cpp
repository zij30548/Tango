#include "mainwindow.h"
#include "radioclient.h"
#include "radiodialog.h"
#include "radiostore.h"
#include "appbackendclient.h"
#include "playerwindowview.h"
#include "focussession.h"
#include "musictaskclient.h"
#include "conversationclient.h"
#include "lookupcontext.h"
#include "neteaseclient.h"
#include <QLabel>
#include <QCryptographicHash>
#include <QPushButton>
#include <QSqlDatabase>
#include <QTimer>
#include <QJsonArray>
#include <QSqlQuery>
#include <QSqlError>
#include <QRegularExpression>
#include <QUuid>
#include <QMediaPlayer>
#include <QMediaPlaylist>
#include <QMessageBox>
#include <cmath>

namespace {
qint64 catalogIdentifier(const QJsonValue &value) {
    const auto number = value.toDouble();
    return value.isDouble() && std::isfinite(number) && number >= 1 && number <= 9007199254740991. &&
        std::floor(number) == number ? qint64(number) : 0;
}
QString catalogIdentity(QString name) {
    name = name.normalized(QString::NormalizationForm_KC).toCaseFolded();
    name.remove(QRegularExpression(QStringLiteral("[\\s.·]")));
    return name;
}
bool catalogCredit(const QJsonObject &song, qint64 artistId) {
    for (const auto &credit : song.value("artists").toArray())
        if (catalogIdentifier(credit.toObject().value("id")) == artistId) return true;
    return false;
}
OnlineSong catalogSong(const QJsonObject &item) {
    OnlineSong song;
    song.id = catalogIdentifier(item.value("id"));
    song.title = item.value("title").toString().trimmed().left(200);
    song.artist = item.value("artist").toString().trimmed().left(200);
    song.album = item.value("album").toString().left(200);
    song.coverUrl = item.value("coverUrl").toString().left(2048);
    song.durationMs = qBound(0, item.value("durationMs").toInt(), 7200000);
    song.fee = item.value("fee").toInt();
    const auto version = item.value("version").toString();
    if (QStringList{"live", "studio"}.contains(version)) song.version = version;
    if (song.title.isEmpty() || song.artist.isEmpty()) song.id = 0;
    return song;
}
bool ensureAdoptionRequests(QSqlDatabase database, QString *error) {
    QSqlQuery query(database);
    if (query.exec("CREATE TABLE IF NOT EXISTS task_adoption_requests (profile TEXT NOT NULL, device_id TEXT NOT NULL, "
                   "task_id TEXT NOT NULL, task_revision INTEGER NOT NULL, preview_version INTEGER NOT NULL, "
                   "request_id TEXT NOT NULL UNIQUE, state TEXT NOT NULL, "
                   "PRIMARY KEY(profile,device_id,task_id,task_revision,preview_version))")) return true;
    if (error) *error = QStringLiteral("无法保存本机采用请求，未提交或播放。");
    return false;
}
QJsonObject adoptionRequest(QSqlDatabase database, const QString &profile, const QString &device,
                           const QJsonObject &task, QString *error) {
    if (!database.tables().contains(QStringLiteral("task_adoption_requests"))) return {};
    QSqlQuery query(database);
    query.prepare("SELECT request_id,state FROM task_adoption_requests WHERE profile=? AND device_id=? "
                  "AND task_id=? AND task_revision=? AND preview_version=?");
    query.addBindValue(profile); query.addBindValue(device); query.addBindValue(task.value("id").toString());
    query.addBindValue(task.value("revision").toInt()); query.addBindValue(task.value("preview").toObject().value("version").toInt());
    if (!query.exec()) { if (error) *error = QStringLiteral("无法核验原采用请求，未重复提交或播放。"); return {}; }
    if (!query.next()) return {};
    const auto id = query.value(0).toString(), state = query.value(1).toString();
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{8,100}$")).match(id).hasMatch() ||
        !QStringList{"unknown", "recorded", "rejected"}.contains(state)) {
        if (error) *error = QStringLiteral("原采用请求记录无效，未重复提交或播放。"); return {};
    }
    return {{"requestId",id},{"state",state}};
}
bool journalAdoption(QSqlDatabase database, const QString &profile, const QString &device,
                     const QJsonObject &task, const QString &requestId, QString *error) {
    if (!ensureAdoptionRequests(database,error)) return false;
    QSqlQuery query(database);
    query.prepare("INSERT INTO task_adoption_requests(profile,device_id,task_id,task_revision,preview_version,request_id,state) "
                  "VALUES(?,?,?,?,?,?,'unknown')");
    query.addBindValue(profile); query.addBindValue(device); query.addBindValue(task.value("id").toString());
    query.addBindValue(task.value("revision").toInt()); query.addBindValue(task.value("preview").toObject().value("version").toInt());
    query.addBindValue(requestId);
    if (query.exec()) return true;
    if (error) *error = QStringLiteral("无法记录当前采用请求，未提交；请先核验原操作或检查本机存储。");
    return false;
}
bool verifiedAppliedAdoption(const QJsonObject &task) {
    // 后台终态和本机实际应用回执必须指向同一任务、预览和采用ID；仅有本机队列不算后台确认。
    const auto preview=task.value("preview").toObject(), adoption=task.value("adoption").toObject();
    const auto id=adoption.value("id").toString();
    if(task.value("status")!="adopted" || id.isEmpty() ||
        adoption.value("previewVersion").toInt()!=preview.value("version").toInt() ||
        !adoption.value("appliedAt").isDouble() || adoption.value("appliedAt").toDouble()<=0)return false;
    const auto database=QSqlDatabase::database();
    if(!database.isOpen() || !database.tables().contains(QStringLiteral("task_adoptions")))return false;
    QSqlQuery query(database);query.prepare("SELECT state FROM task_adoptions WHERE id=? AND task_id=? AND preview_version=?");
    query.addBindValue(id);query.addBindValue(task.value("id").toString());query.addBindValue(preview.value("version").toInt());
    return query.exec() && query.next() && query.value(0).toString()==QStringLiteral("applied");
}
}

void MainWindow::setupRadio()
{
    radioClient = new RadioClient(this);
    musicTaskClient = new MusicTaskClient(this);
    conversationClient = new ConversationClient(this);
    radioUsageTimer = new QTimer(this);
    radioUsageTimer->setSingleShot(true);
    radioUsageTimer->setInterval(600);
    connect(radioUsageTimer, &QTimer::timeout, this, &MainWindow::refreshRadioUsage);
    connect(conversationClient, &ConversationClient::snapshotChanged,
            this, &MainWindow::applyConversationSnapshot);
    connect(conversationClient, &ConversationClient::clearStarted, this, [this] {
        radioConversationsClearing = true;
        discardRadioConversationExecution();
        if (radioDialog) {
            radioDialog->setConversationsClearing(true);
            radioDialog->setPlaybackStatus(tr("正在清除全部对话…"));
        }
    });
    connect(conversationClient, &ConversationClient::conversationsCleared, this, [this](int) {
        QString error;
        RadioStore(QSqlDatabase::database()).clearConversationRoutes(currentProfile, &error);
        discardRadioConversationExecution();
        visibleConversationId.clear(); radioHistory.clear(); radioSeen.clear();
        restoredConversationProfile = currentProfile;
        radioConversationsClearing = false;
        if (radioDialog) {
            radioDialog->setConversation({});
            radioDialog->setEarlierMessagesAvailable(false);
            radioDialog->setConversationsClearing(false);
            radioDialog->setPlaybackStatus(error.isEmpty() ? tr("全部对话已清除，可以开始新的聊天。") : error);
        }
        refreshRadioUsage();
    });
    connect(conversationClient, &ConversationClient::clearFailed, this, [this](const QString &message) {
        radioConversationsClearing = false;
        if (radioDialog) {
            radioDialog->setConversationsClearing(false);
            radioDialog->setPlaybackStatus(message);
        }
        refreshRadioUsage();
    });
    connect(conversationClient, &ConversationClient::messageAccepted, this,
            [this](const QString &id, const QString &instruction, const QString &hint) {
        if (!radioDialog || !AppBackendClient::instance()->authenticated()) return;
        if (pendingRadioInstruction == instruction) {
            radioDialog->submissionAccepted(instruction, pendingRadioSubmission);
            pendingRadioInstruction.clear(); pendingRadioSubmission = 0;
        }
        executeRadioInstruction(instruction, id, hint);
    });
    connect(conversationClient, &ConversationClient::routeReady, this, &MainWindow::applyRadioRoute);
    connect(conversationClient, &ConversationClient::routeRestored, this, &MainWindow::applyRestoredRadioRoute);
    connect(conversationClient, &ConversationClient::routeFailed, this,
            [this](const QString &turnId, const QString &message) {
        if (!radioDialog) return;
        if (radioLookupInputs.value(turnId) == radioLookupContext && !radioLookupContext.isEmpty())
            clearRadioLookupContext(true);
        else radioLookupInputs.remove(turnId);
        if (pendingRadioRouteRestores.remove(turnId)) {
            radioRouteRestoreOrder.removeAll(turnId);
            radioDialog->setPlaybackStatus(tr("旧操作未通过后台核验，已暂停恢复；不会重新调用模型。"));
            QTimer::singleShot(0, this, &MainWindow::restoreNextRadioRoute);
            return;
        }
        if (!musicTaskClient->snapshot().isEmpty()) radioDialog->setTaskSnapshot(musicTaskClient->snapshot());
        else radioDialog->setBusy(false);
        radioDialog->setRouteObservationAvailable(conversationClient->routeNeedsObservation(turnId) ? turnId : QString());
        radioDialog->setPlaybackStatus(message);
        appendRadioReply(message, turnId);
        scheduleRadioUsageRefresh();
    });
    connect(musicTaskClient, &MusicTaskClient::requestSettled, this,
            [this](const QString &requestId, bool accepted, const QString &error) {
        if (radioAdoptionRequests.contains(requestId)) {
            const auto originalTarget=radioAdoptionRequests.take(requestId);
            if (accepted) {
                QSqlQuery query;
                query.prepare("UPDATE task_adoption_requests SET state='recorded' WHERE profile=? AND device_id=? AND request_id=? AND state='unknown'");
                query.addBindValue(currentProfile); query.addBindValue(musicTaskClient->deviceId()); query.addBindValue(requestId);
                if (!query.exec() && radioDialog) radioDialog->setPlaybackStatus(tr("后台已确认原采用请求，但本机回执未保存；不会重新提交或自动播放。"));
            } else if (radioDialog) {
                const auto current=musicTaskClient->snapshot();
                // 原采用回调可能晚于applied确认；旧unknown不能覆盖同一已核实采用的终态，也不改原请求账本。
                const bool alreadyApplied=current.value("id").toString()==originalTarget.first &&
                    current.value("preview").toObject().value("version").toInt()==originalTarget.second && verifiedAppliedAdoption(current);
                radioDialog->setPlaybackStatus(alreadyApplied
                    ? (radioPlaybackStatus.isEmpty()?tr("这份歌单已采用；可以使用底部播放控制，或双击播放单首。"):radioPlaybackStatus)
                    : tr("原采用结果尚未确认；再次采用只会查询原请求，不会重复提交或切换队列。"));
            }
            return;
        }
        if (!routedRadioRequests.contains(requestId)) return;
        const auto target = routedRadioRequests.take(requestId);
        if (!routedRadioTurns.contains(target.first)) return;
        // 网络失败不能证明后台未提交；未知结果阻断依赖，恢复时只查询原请求。
        if (!setRadioRouteAction(target.first, target.second, accepted ? "completed" : "unknown")) return;
        if (!accepted) appendRadioReply(error.isEmpty() ? tr("这项操作未被后台确认，尚未继续后续步骤。") : error, target.first);
        else {
            for (const auto &value : routedRadioTurns.value(target.first).plan.value("actions").toArray()) {
                const auto action = value.toObject();
                if (action.value("id").toString() == target.second && action.value("capability").toString() == "task.control") {
                    const QHash<QString, QString> labels{{"pause", tr("暂停")}, {"cancel", tr("取消")},
                        {"resume", tr("继续")}, {"undo", tr("撤回修改")}};
                    appendRadioReply(tr("后台已接受“%1”操作；当前进度见聊天中的提示。")
                        .arg(labels.value(action.value("parameters").toObject().value("action").toString())), target.first);
                    break;
                }
            }
        }
        continueRadioRoute(target.first);
    });
    connect(musicTaskClient, &MusicTaskClient::requestRejected, this,
            [this](const QString &requestId, const QString &message) {
        if (radioAdoptionRequests.remove(requestId)) {
            QSqlQuery query;
            query.prepare("UPDATE task_adoption_requests SET state='rejected' WHERE profile=? AND device_id=? AND request_id=? AND state='unknown'");
            query.addBindValue(currentProfile); query.addBindValue(musicTaskClient->deviceId()); query.addBindValue(requestId);
            if (!query.exec()) {
                if (radioDialog) radioDialog->setPlaybackStatus(tr("后台已拒绝本次采用，但本机记录保存失败；未重发或播放。"));
                return;
            }
            if (radioDialog) radioDialog->setPlaybackStatus(message);
            return;
        }
        if (!routedRadioRequests.contains(requestId)) return;
        const auto target = routedRadioRequests.take(requestId);
        if (!routedRadioTurns.contains(target.first) || !setRadioRouteAction(target.first, target.second, "failed")) return;
        appendRadioReply(message, target.first);
        continueRadioRoute(target.first);
    });
    connect(musicTaskClient, &MusicTaskClient::busyChanged, this, [this](bool busy) {
        if (!busy) QTimer::singleShot(0, this, [this] {
            const auto order = routedRadioOrder;
            for (const auto &turn : order) continueRadioRoute(turn);
            observeRadioRequests();
        });
    });
    connect(conversationClient, &ConversationClient::submissionFailed, this,
            [this](const QString &, const QString &message) {
        if (radioDialog && !pendingRadioInstruction.isEmpty())
            radioDialog->submissionFailed(pendingRadioInstruction, pendingRadioSubmission);
        pendingRadioInstruction.clear(); pendingRadioSubmission = 0;
        ui->statusLabel->setText(message);
        if (radioDialog) radioDialog->setPlaybackStatus(message);
    });
    connect(conversationClient, &ConversationClient::error, this, [this](const QString &message) {
        // Journal delivery errors must not enqueue another journal event.
        if (conversationClient->snapshot().value("id").toString().isEmpty())
            restoredConversationProfile.clear();
        ui->statusLabel->setText(message);
        if (radioDialog) radioDialog->setPlaybackStatus(message);
    });
    connect(musicTaskClient, &MusicTaskClient::snapshotChanged, this, &MainWindow::applyMusicTaskSnapshot);
    connect(musicTaskClient, &MusicTaskClient::instructionAccepted, this, [this](const QString &instruction) {
        if (!conversationClient->available() && radioDialog && pendingRadioInstruction == instruction) {
            radioDialog->submissionAccepted(instruction, pendingRadioSubmission);
            pendingRadioInstruction.clear(); pendingRadioSubmission = 0;
        }
    });
    connect(musicTaskClient, &MusicTaskClient::error, this, [this](const QString &message) {
        pendingAdoptTask.clear(); pendingAdoptVersion = 0;
        if (radioDialog) {
            if (!conversationClient->available()) {
                if (!pendingRadioInstruction.isEmpty())
                    radioDialog->submissionFailed(pendingRadioInstruction, pendingRadioSubmission);
                pendingRadioInstruction.clear(); pendingRadioSubmission = 0;
                appendRadioReply(message);
            } else {
                // An older task's observation error cannot finish a newly
                // submitted journal message or borrow its conversation turn.
                const auto anchor = resolveTaskConversationTurn(musicTaskClient->snapshot(), false);
                if (!anchor.isEmpty()) appendRadioReply(message, anchor);
                else radioDialog->setPlaybackStatus(message);
            }
            if (musicTaskClient->snapshot().isEmpty()) radioDialog->setBusy(false);
        }
        ui->statusLabel->setText(message);
    });
    connect(ui->radioButton,&QPushButton::clicked,this,&MainWindow::openRadio);
    connect(radioClient,&RadioClient::stageChanged,this,[this](const QString &stage) {
        if (radioDialog) radioDialog->setBusy(true,stage);
    });
    connect(this,&MainWindow::onlinePlaybackFailed,this,[this](qint64 id,const QString &) {
        failRadioTrack(id);
    });
    connect(AppBackendClient::instance(),&AppBackendClient::authChanged,this,[this] {
        radioConversationsClearing = false;
        clearRadioLookupContext();
        ++radioLookupGeneration;
        delete radioLookupOperation.data(); radioLookupOperation = nullptr;
        pendingClearQueueDigest.clear(); pendingClearQueueConversation.clear();
        ++radioAccountEpoch;
        radioUsageTimer->stop(); radioUsageRefreshPending = false;
        delete radioUsageOperation.data(); radioUsageOperation = nullptr;
        restoredConversationProfile.clear(); visibleConversationId.clear(); syncedConversationTasks.clear();
        conversationTaskTurns.clear(); pendingConversationTask = {};
        pendingRadioInstruction.clear(); pendingRadioSubmission = 0;
        activeLegacyConversationTurn.clear(); preferenceConversationTurn.clear();
        routedRadioTurns.clear(); routedRadioOrder.clear(); routedRadioRequests.clear(); routePreferenceBases.clear();
        routePreferenceRevisions.clear(); pendingRadioRouteRestores.clear(); radioRouteRestoreOrder.clear(); observedRadioRequests.clear();
        observedRadioAdoptionRequests.clear(); radioAdoptionRequests.clear();
        pendingRouteMemoryTurn.clear(); pendingRouteMemoryAction.clear();
        legacyAssistantBusy = false; radioResultIsTask = false;
        restoredTaskProfile.clear(); pendingAdoptTask.clear(); pendingAdoptVersion = 0;
        const bool wasPlayingRadio=radioPlayback;
        endRadioPlayback();
        radioClient->reset();radioHistory.clear();radioSeen.clear();
        radioRecommendations.clear();radioTitle.clear();radioNote.clear();radioPlaybackStatus.clear();
        if (wasPlayingRadio) {
            ++mediaGeneration;playbackRequested=false;
            resolvingTrack=false;p_PlayerObject->stop();
        }
        if (radioDialog) {
            radioDialog->resetForAccount();
            radioDialog->setPreference(RadioStore(QSqlDatabase::database()).preference(currentProfile));
            updateRadioAvailability();
            if (AppBackendClient::instance()->authenticated())
                AppBackendClient::instance()->refreshAgentStatus(radioDialog,[this](const QString &){updateRadioAvailability();});
            refreshRadioUsage();
        }
    });
    connect(AppBackendClient::instance(),&AppBackendClient::agentStatusChanged,this,[this] {
        updateRadioAvailability();
        if (radioDialog) restoreRadioConversation();
    });
}

void MainWindow::updateRadioAvailability()
{
    if (!radioDialog) return;
    auto *backend=AppBackendClient::instance();
    const bool signedIn=backend->authenticated();
    radioDialog->setAvailability(signedIn,backend->agentReady(),
        !signedIn ? tr("登录探歌后，聊场景、查新作、管理听歌口味。") : backend->agentReady()
        ? tr("说说想听什么，或问问喜欢的歌手最近出了什么。") : tr("AI 服务暂不可用；已保存口味仍可编辑。"));
    radioDialog->setTaskMode(signedIn && backend->taskRuntimeAvailable());
    radioDialog->setConversationMode(signedIn && backend->conversationAvailable());
}

void MainWindow::scheduleRadioUsageRefresh()
{
    if (!radioDialog || !AppBackendClient::instance()->authenticated()) return;
    if (radioUsageOperation) { radioUsageRefreshPending = true; return; }
    radioUsageTimer->start();
}

void MainWindow::refreshRadioUsage()
{
    if (!radioDialog || !AppBackendClient::instance()->authenticated()) return;
    radioUsageTimer->stop();
    if (radioUsageOperation) { radioUsageRefreshPending = true; return; }
    auto *context = new QObject(this);
    radioUsageOperation = context; radioUsageRefreshPending = false;
    const auto epoch = radioAccountEpoch;
    const auto profile = currentProfile;
    AppBackendClient::instance()->accountUsage(context,
        [this, context, epoch, profile](const QJsonObject &usage, const QString &error) {
            if (epoch != radioAccountEpoch || profile != currentProfile || radioUsageOperation != context) return;
            context->deleteLater(); radioUsageOperation = nullptr;
            if (radioDialog) radioDialog->setAccountUsage(error.isEmpty() ? usage : QJsonObject());
            // A failed/unsupported observation cannot change music execution.
            if (radioUsageRefreshPending) {
                radioUsageRefreshPending = false;
                scheduleRadioUsageRefresh();
            }
        });
}

void MainWindow::restoreRadioConversation()
{
    auto *backend = AppBackendClient::instance();
    if (!radioDialog || !backend->authenticated()) return;
    if (conversationClient->available()) {
        if (restoredConversationProfile != currentProfile) {
            restoredConversationProfile = currentProfile;
            restoredTaskProfile = currentProfile;
            conversationClient->restoreLatest();
        }
    } else if (backend->taskRuntimeAvailable() && restoredTaskProfile != currentProfile) {
        restoredTaskProfile = currentProfile;
        musicTaskClient->restoreLatest();
    }
}

QString MainWindow::latestConversationTurn() const
{
    if (!conversationClient || !conversationClient->available()) return {};
    const auto entries = conversationClient->snapshot().value("messages").toArray();
    for (int i = entries.size() - 1; i >= 0; --i) {
        const auto entry = entries.at(i).toObject();
        if (entry.value("role").toString() == QStringLiteral("user")) return entry.value("id").toString();
    }
    return {};
}

QString MainWindow::resolveTaskConversationTurn(const QJsonObject &snapshot, bool bindPending)
{
    if (!conversationClient || !conversationClient->available()) return {};
    const auto taskId = snapshot.value("id").toString();
    const int revision = snapshot.value("revision").toInt();
    if (taskId.isEmpty() || revision < 1) return {};
    const auto key = taskId + QLatin1Char(':') + QString::number(revision);
    if (conversationTaskTurns.contains(key)) return conversationTaskTurns.value(key);
    const auto journal = conversationClient->snapshot().value("messages").toArray();
    QString instruction;
    for (const auto &value : snapshot.value("messages").toArray()) {
        const auto entry = value.toObject();
        if (entry.value("role").toString() == "user" && entry.value("revision").toInt() == revision)
            instruction = entry.value("content").toString();
    }
    // Bind a server task only after its current user message matches the turn
    // recorded before submission. Repeated text still has a distinct turn ID.
    if (bindPending && pendingConversationTask.revision == revision &&
        (pendingConversationTask.taskId.isEmpty() || pendingConversationTask.taskId == taskId) &&
        pendingConversationTask.instruction == instruction) {
        for (const auto &value : journal) {
            const auto entry = value.toObject();
            if (entry.value("role").toString() == "user" &&
                entry.value("id").toString() == pendingConversationTask.turnId &&
                entry.value("content").toString() == instruction) {
                const auto anchor = pendingConversationTask.turnId;
                conversationTaskTurns.insert(key, anchor); pendingConversationTask = {};
                return anchor;
            }
        }
    }
    // Existing source links have already been checked by the backend.
    QSet<QString> linked;
    for (const auto &value : journal) {
        const auto entry = value.toObject();
        if (entry.value("source").toString() == "task" && entry.value("taskId").toString() == taskId &&
            entry.value("taskRevision").toInt() == revision && !entry.value("turnId").toString().isEmpty())
            linked.insert(entry.value("turnId").toString());
    }
    QString anchor;
    if (linked.size() == 1) anchor = *linked.cbegin();
    else if (linked.size() > 1) return {};
    if (anchor.isEmpty() && !instruction.isEmpty()) {
        QSet<QString> matches;
        for (const auto &value : journal) {
            const auto entry = value.toObject();
            if (entry.value("role").toString() == "user" && entry.value("content").toString() == instruction)
                matches.insert(entry.value("id").toString());
        }
        if (matches.size() == 1) anchor = *matches.cbegin();
        // Ambiguous text deliberately has no fallback to the newest message.
    }
    if (anchor.isEmpty() && instruction == tr("选择曲库歌手：%1。")
            .arg(snapshot.value("brief").toObject().value("artist").toString())) {
        QSet<QString> previous;
        for (const auto &value : journal) {
            const auto entry = value.toObject();
            const int linkedRevision = entry.value("taskRevision").toInt();
            if (entry.value("source").toString() == "task" && entry.value("taskId").toString() == taskId &&
                (linkedRevision == revision - 1 || linkedRevision == revision))
                previous.insert(entry.value("turnId").toString());
        }
        if (previous.size() == 1) anchor = *previous.cbegin();
        if (anchor.isEmpty() && previous.isEmpty()) {
            QString previousInstruction;
            for (const auto &value : snapshot.value("messages").toArray()) {
                const auto entry = value.toObject();
                if (entry.value("role").toString() == "user" && entry.value("revision").toInt() == revision - 1)
                    previousInstruction = entry.value("content").toString();
            }
            if (!previousInstruction.isEmpty()) {
                QSet<QString> candidates;
                for (const auto &value : journal) {
                    const auto entry = value.toObject();
                    if (entry.value("role").toString() == "user" && entry.value("content").toString() == previousInstruction)
                        candidates.insert(entry.value("id").toString());
                }
                // Only a candidate: the backend still proves the original
                // ledger and the consecutive, verified artist selection.
                if (candidates.size() == 1) anchor = *candidates.cbegin();
            }
        }
    }
    if (!anchor.isEmpty()) conversationTaskTurns.insert(key, anchor);
    return anchor;
}

void MainWindow::appendRadioReply(const QString &text, const QString &turnId)
{
    const QString anchor = turnId.isEmpty() ? latestConversationTurn() : turnId;
    if (conversationClient && conversationClient->available() && !anchor.isEmpty())
        conversationClient->appendLocalReply(anchor, text);
    else if (radioDialog) radioDialog->appendMessage(tr("探歌"), text);
}

void MainWindow::applyConversationSnapshot(const QJsonObject &snapshot)
{
    if (!radioDialog || !conversationClient->available()) return;
    const auto id = snapshot.value("id").toString();
    const bool changedConversation = id != visibleConversationId;
    if (changedConversation) {
        clearRadioLookupContext();
        ++radioLookupGeneration;
        delete radioLookupOperation.data(); radioLookupOperation = nullptr;
        pendingClearQueueDigest.clear(); pendingClearQueueConversation.clear();
        visibleConversationId = id; syncedConversationTasks.clear();
        conversationTaskTurns.clear(); pendingConversationTask = {};
        musicTaskClient->detach(); pendingAdoptTask.clear(); pendingAdoptVersion = 0;
        activeLegacyConversationTurn.clear(); preferenceConversationTurn.clear();
        routedRadioTurns.clear(); routedRadioOrder.clear(); routedRadioRequests.clear(); routePreferenceBases.clear();
        routePreferenceRevisions.clear(); pendingRadioRouteRestores.clear(); radioRouteRestoreOrder.clear(); observedRadioRequests.clear();
        pendingRouteMemoryTurn.clear(); pendingRouteMemoryAction.clear();
        radioResultIsTask = false;
        // 新会话不得把上一会话的单曲预览继续当作“第一首”播放。
        radioRecommendations.clear(); radioTitle.clear(); radioNote.clear();
        radioDialog->setTaskMode(false); radioDialog->setRecommendations({}, {}, {});
        radioDialog->clearPreferenceProposal(); radioDialog->setRouteObservationAvailable({});
    }
    radioDialog->setConversation(snapshot.value("messages").toArray());
    radioDialog->setEarlierMessagesAvailable(snapshot.value("hasEarlier").toBool());
    radioHistory.clear();
    for (const auto &value : snapshot.value("messages").toArray()) {
        const auto entry = value.toObject();
        if (entry.value("role").toString() == "user") radioHistory.append(entry.value("content").toString());
    }
    while (radioHistory.size() > 4) radioHistory.removeFirst();
    const auto taskLink = snapshot.value("latestTaskLink").toObject();
    const auto linkedTask = taskLink.value("taskId").toString();
    const auto linkedTurn = taskLink.value("turnId").toString();
    const double linkedRevision = taskLink.value("revision").toDouble(-1);
    if (!linkedTask.isEmpty() && linkedTask == snapshot.value("latestTaskId").toString() &&
        taskLink.value("revision").isDouble() && linkedRevision >= 1 && linkedRevision <= 2147483647. &&
        int(linkedRevision) == linkedRevision) {
        for (const auto &value : snapshot.value("messages").toArray()) {
            const auto entry = value.toObject();
            if (entry.value("role").toString() == "user" && entry.value("id").toString() == linkedTurn) {
                conversationTaskTurns.insert(linkedTask + QLatin1Char(':') + QString::number(int(linkedRevision)), linkedTurn);
                break;
            }
        }
    }
    // 恢复只核对原任务和行动日志；不会确认偏好、重发模型请求或启动播放器。
    const auto taskId = snapshot.value("latestTaskId").toString();
    if (changedConversation && !snapshot.value("clearPending").toBool() && !radioConversationsClearing &&
        !taskId.isEmpty() && AppBackendClient::instance()->taskRuntimeAvailable())
        musicTaskClient->restore(taskId);
    if (changedConversation && !id.isEmpty() && !snapshot.value("clearPending").toBool() && !radioConversationsClearing)
        restoreRadioRoutes();
}

void MainWindow::discardRadioConversationExecution()
{
    // 清除只废止聊天执行上下文；播放器、已保存口味和采用事实保持原状。
    ++radioAccountEpoch; ++radioLookupGeneration;
    // 额度请求同样依赖账号epoch，需一起取消，避免迟到回调留下永久非空的刷新句柄。
    radioUsageTimer->stop(); radioUsageRefreshPending = false;
    delete radioUsageOperation.data(); radioUsageOperation = nullptr;
    delete radioLookupOperation.data(); radioLookupOperation = nullptr;
    clearRadioLookupContext();
    radioClient->reset(); musicTaskClient->detach();
    pendingClearQueueDigest.clear(); pendingClearQueueConversation.clear();
    syncedConversationTasks.clear(); conversationTaskTurns.clear(); pendingConversationTask = {};
    pendingRadioInstruction.clear(); pendingRadioSubmission = 0;
    activeLegacyConversationTurn.clear(); preferenceConversationTurn.clear();
    routedRadioTurns.clear(); routedRadioOrder.clear(); routedRadioRequests.clear(); routePreferenceBases.clear();
    routePreferenceRevisions.clear(); pendingRadioRouteRestores.clear(); radioRouteRestoreOrder.clear();
    observedRadioRequests.clear(); observedRadioAdoptionRequests.clear(); radioAdoptionRequests.clear();
    pendingRouteMemoryTurn.clear(); pendingRouteMemoryAction.clear();
    legacyAssistantBusy = false; radioResultIsTask = false;
    restoredTaskProfile.clear(); pendingAdoptTask.clear(); pendingAdoptVersion = 0;
    radioRecommendations.clear(); radioTitle.clear(); radioNote.clear();
    if (radioDialog) {
        radioDialog->setTaskSnapshot({}); radioDialog->setTaskMode(false);
        radioDialog->setRecommendations({}, {}, {}); radioDialog->clearArtistChoices();
        radioDialog->clearPreferenceProposal(); radioDialog->setRouteObservationAvailable({});
    }
}

void MainWindow::clearRadioConversations()
{
    auto *backend = AppBackendClient::instance();
    if (radioConversationsClearing || !radioDialog || !backend->authenticated() || !conversationClient->available()) return;
    const auto profile = currentProfile;
    const auto account = radioAccountEpoch;
    const auto identity = backend->serviceIdentity() + QLatin1Char(':') + backend->userId();
    QMessageBox confirmation(QMessageBox::Question, tr("清除全部对话"),
        tr("清除当前探歌账号在所有设备上的助手对话和待确认操作？\n已保存口味、播放队列、调用额度和任务审计记录会保留。"),
        QMessageBox::Yes | QMessageBox::Cancel, radioDialog);
    confirmation.setObjectName(QStringLiteral("messageBox_ClearConversations"));
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.button(QMessageBox::Yes)->setText(tr("清除对话"));
    confirmation.button(QMessageBox::Cancel)->setText(tr("取消"));
    if (confirmation.exec() != QMessageBox::Yes) return;
    // 确认窗口期间若切换账号，原确认不能授权清除另一个账号。
    if (!backend->authenticated() || profile != currentProfile || account != radioAccountEpoch ||
        identity != backend->serviceIdentity() + QLatin1Char(':') + backend->userId()) return;
    conversationClient->clearAll();
}

void MainWindow::executeRadioInstruction(const QString &request, const QString &turnId, const QString &dispatchHint)
{
    if (!radioDialog || radioConversationsClearing || conversationClient->snapshot().value("clearPending").toBool()) return;
    auto *dialog = radioDialog.data();
    if (dispatchHint == QStringLiteral("router")) {
        if (!AppBackendClient::instance()->routingAvailable() || turnId.isEmpty()) {
            appendRadioReply(tr("后台路由能力不可用，本次未回退旧助手。"), turnId); return;
        }
        // 只有本轮消息完成持久化后才处理本机确认；恢复历史不会走到这里。
        if (!radioLookupContext.isEmpty() && radioLookupContextFor(turnId).isEmpty()) clearRadioLookupContext(true);
        if (turnId == latestConversationTurn() && executeRadioChatCommand(request, turnId)) {
            clearRadioLookupContext(true); return;
        }
        dialog->setRouteObservationAvailable({});
        dialog->setBusy(true, tr("正在理解需求并选择能力…"));
        QString error;
        const auto saved = RadioStore(QSqlDatabase::database()).preferenceSnapshot(currentProfile, &error);
        if (saved.isEmpty()) { dialog->setBusy(false); appendRadioReply(error, turnId); return; }
        const auto preference = saved.value("preference").toString();
        routePreferenceBases.insert(turnId, preference);
        routePreferenceRevisions.insert(turnId, qint64(saved.value("revision").toDouble()));
        const auto lookup = radioLookupContextFor(turnId);
        if (!radioLookupContext.isEmpty() && lookup.isEmpty()) clearRadioLookupContext();
        if (!lookup.isEmpty()) radioLookupInputs.insert(turnId, lookup);
        conversationClient->route(turnId, preference, lookup);
        return;
    }
    const bool taskAvailable = AppBackendClient::instance()->taskRuntimeAvailable();
    // Compatibility only for old backends without the durable journal. The new
    // path receives a persisted bridge hint; phase 2 replaces this with intents.
    static const QRegularExpression oldMemoryCommand(QStringLiteral("记住|记忆|(?:清空|删除|清除|忘掉|忘记).*(?:偏好|口味)|(?:长期|永久)保存|(?:以后|今后).*(?:优先|不要|少推|多推)"));
    const bool useTask = taskAvailable && (turnId.isEmpty()
        ? !oldMemoryCommand.match(request).hasMatch() : dispatchHint == QStringLiteral("task"));
    if (useTask) {
        dialog->setTaskMode(true); dialog->clearArtistChoices(); radioResultIsTask = true;
        if (!turnId.isEmpty()) {
            const auto current = musicTaskClient->snapshot();
            pendingConversationTask = {current.value("id").toString(), turnId, request,
                current.value("id").toString().isEmpty() ? 1 : current.value("revision").toInt() + 1};
        }
        musicTaskClient->submit(request, RadioStore(QSqlDatabase::database()).preference(currentProfile), radioSeen, turnId);
        return;
    }
    if (taskAvailable && QStringList{"planning", "researching", "waiting_tools", "composing"}
            .contains(musicTaskClient->snapshot().value("status").toString())) {
        appendRadioReply(tr("请先暂停当前查找，再编辑长期口味；当前歌单不会被修改。"), turnId);
        if (turnId.isEmpty()) dialog->submissionFailed(request, pendingRadioSubmission);
        pendingRadioInstruction.clear(); pendingRadioSubmission = 0;
        return;
    }
    if (turnId.isEmpty()) dialog->appendMessage(tr("你"), request);
    dialog->clearArtistChoices(); dialog->setBusy(true, tr("正在理解你的音乐请求…"));
    legacyAssistantBusy = true; activeLegacyConversationTurn = turnId;
    const QString profile = currentProfile; const auto epoch = radioAccountEpoch;
    const auto uiSubmission = pendingRadioSubmission;
    radioClient->assist(request, radioHistory, RadioStore(QSqlDatabase::database()).preference(profile),
        radioSeen, dialog, [this, profile, epoch, request, turnId, uiSubmission](const RadioResult &result, const QString &error) {
            if (profile != currentProfile || epoch != radioAccountEpoch) return;
            legacyAssistantBusy = false;
            scheduleRadioUsageRefresh();
            if (turnId.isEmpty() && radioDialog) {
                if (error.isEmpty()) radioDialog->submissionAccepted(request, uiSubmission);
                else radioDialog->submissionFailed(request, uiSubmission);
                pendingRadioInstruction.clear(); pendingRadioSubmission = 0;
            }
            applyAssistantResult(result, error, request, turnId);
        });
}

void MainWindow::openRadio()
{
    if (radioDialog) {
        updateRadioAvailability();
        showContentPage(radioDialog, ui->radioButton);
        restoreRadioConversation();
        refreshRadioUsage();
        return;
    }
    // Durable tasks continue while this page is hidden. Legacy requests retain
    // their original cancellation behavior; playback belongs to MainWindow.
    auto *dialog = new RadioDialog(this);
    radioDialog = dialog;
    connect(dialog, &RadioDialog::clearConversationsRequested, this, &MainWindow::clearRadioConversations);
    connect(dialog, &RadioDialog::playlistActivated, this, &MainWindow::openRadioPlaylist);
    dialog->setEmbedded(true);
    if (!radioRecommendations.isEmpty()) {
        dialog->setRecommendations(radioTitle, radioNote, radioRecommendations);
        for (const auto &message : radioHistory) dialog->appendMessage(tr("你"), message);
    }
    if (!radioPlaybackStatus.isEmpty()) dialog->setPlaybackStatus(radioPlaybackStatus);
    dialog->setPreference(RadioStore(QSqlDatabase::database()).preference(currentProfile));
    updateRadioAvailability();
    connect(dialog, &RadioDialog::generateRequested, dialog, [this, dialog](const QString &instruction) {
        const QString request = instruction.trimmed();
        if (request.isEmpty()) return;
        if (!pendingRadioInstruction.isEmpty()) {
            dialog->setPlaybackStatus(tr("上一条消息还在等待回执，输入内容已保留。")); return;
        }
        pendingRadioInstruction = request; pendingRadioSubmission = dialog->currentSubmissionId();
        if (legacyAssistantBusy || musicTaskClient->busy() || conversationClient->busy()) {
            dialog->submissionFailed(request, pendingRadioSubmission);
            pendingRadioInstruction.clear(); pendingRadioSubmission = 0;
            dialog->setPlaybackStatus(tr("上一个操作还在提交，请稍候；输入内容已保留。")); return;
        }
        if (conversationClient->available()) conversationClient->submit(request);
        else executeRadioInstruction(request);
    });
    connect(dialog, &RadioDialog::artistSelected, dialog, [this, dialog](const AssistantArtist &artist) {
        const auto pending = property("tangoCatalogChoices").toJsonObject();
        if (!pending.isEmpty()) {
            QJsonObject selected;
            for (const auto &value : pending.value("artists").toArray())
                if (catalogIdentifier(value.toObject().value("id")) == artist.id &&
                    value.toObject().value("name").toString() == artist.name) selected = value.toObject();
            const auto turn = pending.value("turn").toString(), action = pending.value("action").toString();
            const auto profile = currentProfile, conversation = visibleConversationId, selectionTurn = latestConversationTurn();
            const auto epoch = radioAccountEpoch;
            if (selected.isEmpty() || !routedRadioTurns.contains(turn) || pending.value("profile").toString() != profile ||
                pending.value("conversation").toString() != conversation) return;
            setProperty("tangoCatalogChoices", QJsonObject{});
            // 等当前已持久化的聊天选择结束清理旧候选，再续原只读查找；清除/换账号会废止此回调。
            QTimer::singleShot(0, this, [this, turn, action, selected, pending, profile, conversation, selectionTurn, epoch] {
                if (profile != currentProfile || epoch != radioAccountEpoch || conversation != visibleConversationId ||
                    selectionTurn != latestConversationTurn() || !routedRadioTurns.contains(turn)) return;
                routedRadioTurns[turn].confirmationTurn = selectionTurn;
                lookupRadioArtist(turn, action, pending.value("parameters").toObject(), selected);
            });
            return;
        }
        if (AppBackendClient::instance()->taskRuntimeAvailable() &&
            radioResultIsTask && !musicTaskClient->snapshot().isEmpty()) {
            musicTaskClient->action(QStringLiteral("select_artist"), {}, {{"artistId", artist.id}});
            return;
        }
        const QString profile = currentProfile;
        const auto epoch = radioAccountEpoch;
        const auto turn = activeLegacyConversationTurn.isEmpty() ? latestConversationTurn() : activeLegacyConversationTurn;
        activeLegacyConversationTurn = turn; legacyAssistantBusy = true;
        dialog->setBusy(true, tr("正在核验歌手发行记录…"));
        radioClient->releasesForArtist(artist, dialog,
            [this, profile, epoch, turn, artist](const RadioResult &result, const QString &error) {
                if (profile != currentProfile || epoch != radioAccountEpoch) return;
                legacyAssistantBusy = false;
                scheduleRadioUsageRefresh();
                applyAssistantResult(result, error, tr("查询歌手 %1 的近期发行").arg(artist.name), turn);
            });
    });
    connect(dialog, &RadioDialog::cancelRequested, dialog, [this, dialog] {
        if (legacyAssistantBusy) {
            radioClient->cancel(); legacyAssistantBusy = false; dialog->setBusy(false);
            if (!conversationClient->available() && !pendingRadioInstruction.isEmpty()) {
                dialog->submissionFailed(pendingRadioInstruction, pendingRadioSubmission);
                pendingRadioInstruction.clear(); pendingRadioSubmission = 0;
            }
            appendRadioReply(tr("已取消这次请求，已保存的口味和当前歌单保持原状态。"), activeLegacyConversationTurn);
            return;
        }
        if (AppBackendClient::instance()->taskRuntimeAvailable() && radioResultIsTask && !musicTaskClient->snapshot().isEmpty()) {
            musicTaskClient->action("cancel"); return;
        }
        radioClient->cancel();
        dialog->setBusy(false);
    });
    connect(dialog, &RadioDialog::playRequested, dialog, [this, dialog](const QVector<RadioRecommendation> &songs) {
        if (AppBackendClient::instance()->taskRuntimeAvailable() && radioResultIsTask && !musicTaskClient->snapshot().isEmpty()) {
            adoptMusicTask(); return;
        }
        if (startRadio(songs)) {
            radioPlaybackStatus = tr("电台已开始。切换页面后音乐会继续，也可以留在这里边听边调整。");
            dialog->setPlaybackStatus(radioPlaybackStatus);
        }
    });
    connect(dialog, &RadioDialog::trackActivated, dialog, [this](const OnlineSong &song) { playOnlineSong(song); });
    connect(dialog, &RadioDialog::rememberRequested, dialog, [this, dialog] {
        dialog->reviewPreference(RadioStore(QSqlDatabase::database()).preference(currentProfile));
    });
    connect(dialog, &RadioDialog::preferenceProposalDecided, this, &MainWindow::decideRadioPreference);
    connect(dialog, &RadioDialog::routeObservationRequested, conversationClient, &ConversationClient::observeRoute);
    connect(dialog, &RadioDialog::preferenceEdited, dialog, [this, dialog](const QString &preference) {
        QString error;
        if (RadioStore(QSqlDatabase::database()).save(currentProfile, preference, &error)) {
            dialog->setPreference(preference);
            appendRadioReply(preference.trimmed().isEmpty()
                ? tr("已清除当前账号在本机保存的口味。")
                : tr("已保存当前账号的口味，之后找歌会参考它；本次场景仍可覆盖偏好。"), preferenceConversationTurn);
        } else appendRadioReply(error, preferenceConversationTurn);
        preferenceConversationTurn.clear();
    });
    connect(dialog, &RadioDialog::clearPreferenceRequested, dialog, [this, dialog] {
        QString error;
        if (RadioStore(QSqlDatabase::database()).save(currentProfile, {}, &error)) {
            dialog->setPreference({});
            appendRadioReply(tr("已清除记住的口味。"), preferenceConversationTurn);
        } else appendRadioReply(error, preferenceConversationTurn);
        preferenceConversationTurn.clear();
    });
    connect(dialog, &RadioDialog::avoidArtistRequested, dialog, [dialog](const QString &artist) {
        emit dialog->generateRequested(QStringLiteral("这次避开歌手：%1").arg(artist));
    });
    connect(dialog, &RadioDialog::loginRequested, dialog, [this] { on_pushButton_SignIn_clicked(); });
    connect(dialog, &RadioDialog::earlierMessagesRequested, conversationClient, &ConversationClient::loadEarlier);
    connect(dialog, &RadioDialog::taskActionRequested, this, [this, dialog](const QString &action) {
        if (action == "new_conversation") {
            if (legacyAssistantBusy || musicTaskClient->busy() || conversationClient->busy()) return;
            if (QStringList{"planning", "researching", "waiting_tools", "composing"}
                    .contains(musicTaskClient->snapshot().value("status").toString())) {
                dialog->setPlaybackStatus(tr("请先暂停当前查找，再开始新对话。"));
                return;
            }
            // Keep the current executor until startNew accepts the switch.
            // startNew may reject an unresolved receipt or unsent reply.
            conversationClient->startNew(); return;
        }
        if (action == "restore" || (action == "resume" && musicTaskClient->snapshot().contains("clientConnection"))) {
            restoredTaskProfile = currentProfile;
            const auto linkedTask = conversationClient->snapshot().value("latestTaskId").toString();
            if (conversationClient->available() && !linkedTask.isEmpty()) musicTaskClient->restore(linkedTask);
            else if (conversationClient->available()) conversationClient->restoreLatest();
            else musicTaskClient->restoreLatest();
        }
        else if (action == "new") {
            musicTaskClient->detach(); restoredTaskProfile = currentProfile;
            pendingConversationTask = {};
            pendingAdoptTask.clear(); pendingAdoptVersion = 0;
            if (radioDialog) radioDialog->setTaskSnapshot({});
        } else if (action == "lock" || action == "lock_first_three" || action == "unlock") {
            QJsonArray locked;
            const auto snapshot = musicTaskClient->snapshot();
            const auto songs = snapshot.value("preview").toObject().value("tracks").toArray();
            if (action == "lock_first_three" || (action == "lock" && snapshot.value("lockedIds").toArray().isEmpty()))
                for (int i = 0; i < qMin(3, songs.size()); ++i) locked.append(songs.at(i).toObject().value("id"));
            musicTaskClient->action("lock", {}, {{"lockedIds", locked}});
        } else musicTaskClient->action(action);
    });
    showContentPage(dialog, ui->radioButton);
    if (conversationClient->available()) {
        if (!conversationClient->snapshot().isEmpty()) applyConversationSnapshot(conversationClient->snapshot());
        restoreRadioConversation();
    } else if (AppBackendClient::instance()->taskRuntimeAvailable()) {
        if (!musicTaskClient->snapshot().isEmpty()) applyMusicTaskSnapshot(musicTaskClient->snapshot());
        else { restoredTaskProfile = currentProfile; musicTaskClient->restoreLatest(); }
    }
    if (AppBackendClient::instance()->authenticated())
        AppBackendClient::instance()->refreshAgentStatus(dialog, [this](const QString &) { updateRadioAvailability(); });
    refreshRadioUsage();
}

void MainWindow::adoptMusicTask()
{
    const auto snapshot = musicTaskClient->snapshot();
    const auto preview = snapshot.value("preview").toObject();
    if (snapshot.value("id").toString().isEmpty() || preview.value("tracks").toArray().isEmpty()) return;
    if (snapshot.value("status").toString() == "adopted") {
        if (radioDialog) radioDialog->setPlaybackStatus(tr("这份歌单已采用；可以使用底部播放控制，或双击播放单首。"));
        return;
    }
    QString error;
    const auto original = adoptionRequest(QSqlDatabase::database(),currentProfile,musicTaskClient->deviceId(),snapshot,&error);
    if (!error.isEmpty()) { if(radioDialog)radioDialog->setPlaybackStatus(error); return; }
    if (!original.isEmpty()) {
        const auto requestId=original.value("requestId").toString();
        if (original.value("state").toString()=="rejected") {
            if (radioDialog) radioDialog->setPlaybackStatus(tr("原采用请求已被后台明确拒绝；请刷新任务并核对新版本，不会重复提交或播放。"));
            return;
        }
        if (original.value("state").toString()=="unknown") {
            pendingAdoptTask.clear(); pendingAdoptVersion=0;
            radioAdoptionRequests.insert(requestId,qMakePair(snapshot.value("id").toString(),preview.value("version").toInt()));
            observedRadioAdoptionRequests.insert(requestId);
            if (radioDialog) radioDialog->setPlaybackStatus(tr("正在只读核验原采用请求；查到结果后也不会自动切换队列，请核对后再次明确采用。"));
            musicTaskClient->observeAdoptionRequest(requestId,snapshot.value("id").toString(),snapshot.value("revision").toInt()); return;
        }
        if (snapshot.value("status").toString()!=QStringLiteral("awaiting_apply") || snapshot.value("adoption").toObject().value("id").toString().isEmpty()) {
            if (radioDialog)radioDialog->setPlaybackStatus(tr("原采用已记录，但当前预览状态不一致；请先重新找回任务，不会重复提交。"));
            return;
        }
        // 已核验的旧采用只有这次新的明确确认才允许应用；查询或恢复本身不能播放。
        pendingAdoptTask=snapshot.value("id").toString(); pendingAdoptVersion=preview.value("version").toInt();
        applyMusicTaskSnapshot(snapshot); return;
    }
    const auto requestId=QStringLiteral("adopt-")+QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!journalAdoption(QSqlDatabase::database(),currentProfile,musicTaskClient->deviceId(),snapshot,requestId,&error)) {
        if (radioDialog)radioDialog->setPlaybackStatus(error); return;
    }
    radioAdoptionRequests.insert(requestId,qMakePair(snapshot.value("id").toString(),preview.value("version").toInt()));
    pendingAdoptTask = snapshot.value("id").toString();
    pendingAdoptVersion = preview.value("version").toInt();
    musicTaskClient->adopt(requestId);
}

void MainWindow::applyMusicTaskSnapshot(const QJsonObject &snapshot)
{
    if (radioConversationsClearing || conversationClient->snapshot().value("clearPending").toBool()) return;
    if (!AppBackendClient::instance()->authenticated() || !AppBackendClient::instance()->taskRuntimeAvailable()) return;
    if (!snapshot.value("id").toString().isEmpty()) scheduleRadioUsageRefresh();
    if (snapshot.value("status").toString() == "preview") {
        for (const auto &value : snapshot.value("preview").toObject().value("tracks").toArray()) {
            const qint64 id = qint64(value.toObject().value("id").toDouble());
            if (id > 0 && !radioSeen.contains(id)) radioSeen.append(id);
        }
        while (radioSeen.size() > 60) radioSeen.removeFirst();
    }
    if (radioDialog) {
        radioResultIsTask = !snapshot.value("id").toString().isEmpty();
        radioDialog->setTaskSnapshot(snapshot);
        radioRecommendations = radioDialog->recommendations();
        // awaiting_apply的同步回调可能先启动applied、再收到旧采用失败；最终权威确认到达后修正文案。
        // 保留当前真实播放错误，不用采用成功覆盖权限或音频失败；恢复只刷新事实，不触发队列操作。
        if(verifiedAppliedAdoption(snapshot))radioDialog->setPlaybackStatus(radioPlaybackStatus.isEmpty()
            ?tr("这份歌单已采用；可以使用底部播放控制，或双击播放单首。"):radioPlaybackStatus);
    }
    if (!snapshot.value("id").toString().isEmpty() && pendingAdoptTask.isEmpty()) {
        QString error;
        const auto original=adoptionRequest(QSqlDatabase::database(),currentProfile,musicTaskClient->deviceId(),snapshot,&error);
        const auto requestId=original.value("requestId").toString();
        if (!error.isEmpty() && radioDialog)radioDialog->setPlaybackStatus(error);
        if (original.value("state").toString()=="unknown" && !observedRadioAdoptionRequests.contains(requestId)) {
            observedRadioAdoptionRequests.insert(requestId);
            const auto account=radioAccountEpoch;
            const auto taskId=snapshot.value("id").toString();
            const auto revision=snapshot.value("revision").toInt();
            const auto previewVersion=snapshot.value("preview").toObject().value("version").toInt();
            QTimer::singleShot(0,this,[this,account,taskId,revision,previewVersion,requestId] {
                const auto current=musicTaskClient->snapshot();
                if(account!=radioAccountEpoch || musicTaskClient->busy() || current.value("id").toString()!=taskId ||
                    current.value("revision").toInt()!=revision || current.value("preview").toObject().value("version").toInt()!=previewVersion)return;
                radioAdoptionRequests.insert(requestId,qMakePair(taskId,previewVersion));
                musicTaskClient->observeAdoptionRequest(requestId,taskId,revision);
            });
        }
    }
    if (conversationClient->available() && !visibleConversationId.isEmpty() &&
            !snapshot.value("id").toString().isEmpty()) {
        const int revision = snapshot.value("revision").toInt();
        const auto anchor = resolveTaskConversationTurn(snapshot);
        const auto version = qint64(snapshot.value("stateVersion").toDouble());
        const auto key = snapshot.value("id").toString() + QLatin1Char(':') +
            QString::number(revision) + QLatin1Char(':') + QString::number(version);
        if (!anchor.isEmpty() && !syncedConversationTasks.contains(key)) {
            syncedConversationTasks.insert(key);
            conversationClient->syncTask(anchor, snapshot.value("id").toString(), revision, version);
        }
    }
    if (pendingAdoptTask.isEmpty() || snapshot.value("id").toString() != pendingAdoptTask) return;
    const auto preview = snapshot.value("preview").toObject();
    if (preview.value("version").toInt() != pendingAdoptVersion) {
        pendingAdoptTask.clear(); pendingAdoptVersion = 0;
        if (radioDialog) radioDialog->setPlaybackStatus(tr("歌单已更新，请核对新版本后采用。"));
        return;
    }
    if (snapshot.value("status").toString() != "awaiting_apply") return;
    const auto adoption = snapshot.value("adoption").toObject();
    const QString id = adoption.value("id").toString();
    if (id.isEmpty() || id.size() > 256) return;
    pendingAdoptTask.clear(); pendingAdoptVersion = 0;
    auto database = QSqlDatabase::database();
    QSqlQuery query(database);
    if (!query.exec("CREATE TABLE IF NOT EXISTS task_adoptions (id TEXT PRIMARY KEY, "
                    "task_id TEXT NOT NULL, preview_version INTEGER NOT NULL, state TEXT NOT NULL)")) {
        if (radioDialog) radioDialog->setPlaybackStatus(tr("无法保存采用记录，尚未切换播放。请检查本机数据目录。"));
        return;
    }
    query.prepare("SELECT state FROM task_adoptions WHERE id=?");
    query.addBindValue(id);
    if (!query.exec()) return;
    if (query.next()) {
        const QString state = query.value(0).toString();
        query.finish();
        if (state == "applied") musicTaskClient->action("applied", {}, {{"adoptionId", id}});
        else if (radioDialog) radioDialog->setPlaybackStatus(tr("上次采用的结果尚未确认，为避免重复切换，未自动执行。请检查当前队列，也可双击播放单首。"));
        return;
    }
    query.finish();
    query.prepare("INSERT INTO task_adoptions(id,task_id,preview_version,state) VALUES(?,?,?,'issued')");
    query.addBindValue(id); query.addBindValue(snapshot.value("id").toString());
    query.addBindValue(preview.value("version").toInt());
    if (!query.exec()) return;
    // Only an explicit current-page adopt intent reaches this point. Restoring
    // a task or retrying an acknowledgement never starts the player.
    if (!radioDialog || !startRadio(radioDialog->recommendations())) {
        query.prepare("DELETE FROM task_adoptions WHERE id=? AND state='issued'");
        query.addBindValue(id); query.exec();
        if (radioDialog) radioDialog->setPlaybackStatus(tr("歌单无效，未采用。请重新生成预览。"));
        return;
    }
    query.prepare("UPDATE task_adoptions SET state='applied' WHERE id=?");
    query.addBindValue(id);
    if (!query.exec()) {
        if (radioDialog) radioDialog->setPlaybackStatus(tr("队列已采用，本机回执保存失败；不会自动重复执行。"));
        return;
    }
    radioPlaybackStatus = tr("歌单已采用；切换页面后继续播放，任务和预览可在重新登录后找回。途中权限变化的歌曲会被跳过。");
    if (radioDialog) radioDialog->setPlaybackStatus(radioPlaybackStatus);
    musicTaskClient->action("applied", {}, {{"adoptionId", id}});
}

void MainWindow::applyAssistantResult(const RadioResult &result,const QString &error,const QString &request,
                                    const QString &conversationTurn)
{
    if(!radioDialog)return;
    auto *dialog=radioDialog.data();dialog->setBusy(false);
    if(!error.isEmpty()){appendRadioReply(error,conversationTurn);return;}
    if(!request.isEmpty()) {radioHistory.append(request);while(radioHistory.size()>4)radioHistory.removeFirst();}
    if(result.kind==QStringLiteral("artists")) {
        dialog->setArtistChoices(result.artists);appendRadioReply(result.note,conversationTurn);return;
    }
    dialog->clearArtistChoices();
    if(result.kind==QStringLiteral("remember")) {
        preferenceConversationTurn = conversationTurn;
        appendRadioReply(tr("我整理了长期口味摘要，请在编辑框中核对并保存；当前场景不会自动记入。"),conversationTurn);
        dialog->reviewPreference(result.preference);return;
    }
    if(result.kind==QStringLiteral("forget")) {
        preferenceConversationTurn = conversationTurn;
        appendRadioReply(tr("即将清除保存的口味。请在口味编辑框中留空并保存；取消会保留原内容。"),conversationTurn);
        dialog->reviewPreference({});return;
    }
    if(result.kind==QStringLiteral("clarify")) {appendRadioReply(result.note,conversationTurn);return;}
    // Text-only replies leave the previous playable selection untouched.
    if(!result.tracks.isEmpty() || result.kind==QStringLiteral("releases")) {
        radioResultIsTask = false;
        if(result.kind==QStringLiteral("radio")) {
            for(const auto &track:result.tracks)if(!radioSeen.contains(track.song.id))radioSeen.append(track.song.id);
            while(radioSeen.size()>60)radioSeen.removeFirst();
        }
        radioRecommendations=result.tracks;radioTitle=result.title;radioNote=result.note;
        dialog->setRecommendations(result.title,result.note,result.tracks);
    }
    if(result.kind==QStringLiteral("releases"))appendRadioReply(result.note,conversationTurn);
    else appendRadioReply(tr("为你准备了“%1”，共 %2 首。可以开始收听，也可以继续告诉我想怎么调整。")
                              .arg(result.title).arg(result.tracks.size()),conversationTurn);
}

bool MainWindow::startRadio(const QVector<RadioRecommendation> &songs)
{
    if(songs.isEmpty() || songs.size()>40)return false;
    QSet<qint64> ids;
    for(const auto &track:songs) {
        if(track.song.id<=0 || track.song.id>9007199254740991LL || track.song.title.isEmpty() || ids.contains(track.song.id))return false;
        ids.insert(track.song.id);
    }
    radioPlayback=false;
    if(focusSession)focusSession->cancel(tr("开始收听 AI 私人电台"));
    ++mediaGeneration;playbackRequested=false;resolvingTrack=false;preparedTrack=-1;
    p_PlayerObject->stop();p_PlayerObject->setMedia(QMediaContent());
    p_PlayerList->clear();tracks.clear();trackIndexes.clear();trackDurations.clear();lastHistoryUrl.clear();ui->queueModel->clear();
    for(const auto &recommendation:songs) {
        const auto &song=recommendation.song;
        const int index=appendTrack(QUrl(QStringLiteral("https://music.163.com/song?id=%1").arg(song.id)),song.title,song.artist,song.id);
        tracks[index].album=song.album;tracks[index].coverUrl=song.coverUrl;
        ui->queueModel->setArtwork(index,song.album,song.coverUrl);
    }
    radioFailed.clear();radioPaused=false;radioPlayback=true;
    ui->shuffleButton->setChecked(false);ui->repeatButton->setChecked(true);
    repeatMode=0;updatePlaybackMode();
    ui->shuffleButton->setEnabled(false);ui->repeatButton->setEnabled(false);
    playTrack(0,true);return true;
}

void MainWindow::endRadioPlayback(const QString &reason)
{
    if(!radioPlayback)return;
    radioPlayback=false;radioPaused=false;radioFailed.clear();
    ui->shuffleButton->setEnabled(true);ui->repeatButton->setEnabled(true);
    radioPlaybackStatus=reason;
    if(radioDialog)radioDialog->setPlaybackStatus(reason);
}

void MainWindow::failRadioTrack(qint64 id)
{
    if(!radioPlayback || p_PlayerList->currentIndex()<0 ||
       tracks.value(p_PlayerList->currentIndex()).id!=id)return;
    const int generation=mediaGeneration;
    QTimer::singleShot(0,this,[this,generation,id] {
        if(radioPlayback && generation==mediaGeneration && tracks.value(p_PlayerList->currentIndex()).id==id)
            advanceRadio(1,true);
    });
}

void MainWindow::advanceRadio(int step,bool failed)
{
    if(!radioPlayback || tracks.isEmpty())return;
    const int index=p_PlayerList->currentIndex();
    if(failed && index>=0)radioFailed.insert(tracks.at(index).id);
    for(int count=1;count<=tracks.size();count++) {
        const int next=(index+step*count+tracks.size()*2)%tracks.size();
        if(!radioFailed.contains(tracks.at(next).id)) {playTrack(next,true,!radioPaused);return;}
    }
    endRadioPlayback();playbackRequested=false;resolvingTrack=false;++mediaGeneration;
    p_PlayerObject->stop();
    const QString message=tr("这批歌曲暂时无法完整播放。请检查网易云授权，或让 AI 换一批音乐。无权限与仅试听歌曲已跳过。");
    radioPlaybackStatus=message;
    ui->statusLabel->setText(message);if(radioDialog)radioDialog->setPlaybackStatus(message);
}

bool MainWindow::setRadioRouteAction(const QString &turnId, const QString &actionId, const QString &state)
{
    if (!routedRadioTurns.contains(turnId)) return false;
    auto &turn = routedRadioTurns[turnId];
    const auto previous = turn.actionStates.value(actionId);
    if (previous == state) return true;
    QString error;
    if (!RadioStore(QSqlDatabase::database()).setRouteAction(currentProfile, conversationClient->deviceId(),
            visibleConversationId, turnId, actionId, previous, state, &error)) {
        turn.restored = true;
        if (radioDialog) radioDialog->setPlaybackStatus(error);
        return false;
    }
    turn.actionStates[actionId] = state;
    return true;
}

void MainWindow::restoreRadioRoutes()
{
    QString error;
    const auto records = RadioStore(QSqlDatabase::database()).routes(currentProfile, conversationClient->deviceId(),
                                                                   visibleConversationId, &error);
    if (!error.isEmpty()) { radioDialog->setPlaybackStatus(error); return; }
    for (const auto &value : records) {
        const auto record = value.toObject();
        const auto turnId = record.value("turnId").toString();
        bool visible = false;
        for (const auto &message : conversationClient->snapshot().value("messages").toArray())
            if (message.toObject().value("role").toString() == "user" && message.toObject().value("id").toString() == turnId) visible = true;
        if (!visible) continue;
        pendingRadioRouteRestores.insert(turnId, record); radioRouteRestoreOrder.append(turnId);
    }
    restoreNextRadioRoute();
}

void MainWindow::restoreNextRadioRoute()
{
    if (!radioDialog || !AppBackendClient::instance()->authenticated() || radioRouteRestoreOrder.isEmpty()) return;
    if (conversationClient->busy()) {
        QTimer::singleShot(100, this, &MainWindow::restoreNextRadioRoute); return;
    }
    conversationClient->restoreRoute(radioRouteRestoreOrder.first());
}

void MainWindow::applyRestoredRadioRoute(const QString &turnId, const QJsonObject &plan)
{
    if (!radioDialog || !AppBackendClient::instance()->authenticated() || !pendingRadioRouteRestores.contains(turnId)) return;
    const auto record = pendingRadioRouteRestores.take(turnId);
    radioRouteRestoreOrder.removeAll(turnId);
    QTimer::singleShot(0, this, &MainWindow::restoreNextRadioRoute);
    if (record.value("plan").toObject() != plan) {
        radioDialog->setPlaybackStatus(tr("本机记录与后台原路由不一致，未恢复旧操作。")); return;
    }
    RoutedRadioTurn turn; turn.plan = plan; turn.restored = true;
    turn.preferenceBase = record.value("preferenceBase").toString();
    turn.baseRevision = qint64(record.value("baseRevision").toDouble());
    turn.confirmationTurn = record.value("confirmationTurn").toString();
    for (const auto &message : conversationClient->snapshot().value("messages").toArray())
        if (message.toObject().value("id").toString() == turnId) turn.instruction = message.toObject().value("content").toString();
    const auto states = record.value("actions").toObject();
    for (auto it = states.begin(); it != states.end(); ++it) turn.actionStates.insert(it.key(), it.value().toString());
    routedRadioTurns.insert(turnId, turn); routedRadioOrder.append(turnId);
    for (const auto &value : record.value("receipts").toObject()) {
        const auto text = value.toObject().value("text").toString();
        if (!text.isEmpty()) appendRadioReply(text, turnId);
    }
    bool waiting = false;
    for (const auto &value : plan.value("actions").toArray()) {
        const auto action = value.toObject(); const auto id = action.value("id").toString();
        const auto status = turn.actionStates.value(id);
        if (QStringList{"music.lookup", "music.refine", "music.artist_catalog", "music.playlist_search"}
                .contains(action.value("capability").toString()) &&
            QStringList{"pending", "dispatched", "unknown"}.contains(status)) {
            // 只读检索候选不跨进程恢复；核对原路由后结束旧动作，不自动重查或永久占用待恢复名额。
            if (!setRadioRouteAction(turnId, id, "skipped")) continue;
            appendRadioReply(action.value("capability").toString() == "music.refine"
                ? tr("已核对上次的候选筛选记录。原候选未跨重启保存，请明确歌名重新搜索；没有重做筛选或播放。")
                : tr("已核对上次的候选查找记录。原候选未跨重启保存，请明确歌曲、歌手或歌单重新搜索；没有重查或播放。"), turnId);
            continue;
        }
        if (action.value("capability").toString() == "memory.propose" &&
            (status == "awaiting_confirmation" || status == "pending") && pendingRouteMemoryTurn.isEmpty()) {
            bool dependenciesReady = true;
            for (const auto &dependency : action.value("dependsOn").toArray())
                dependenciesReady &= turn.actionStates.value(dependency.toString()) == "completed";
            if (!dependenciesReady) { waiting = true; continue; }
            if (turnId != latestConversationTurn() && turn.confirmationTurn != latestConversationTurn()) {
                setRadioRouteAction(turnId, id, "failed"); continue;
            }
            if (status == "pending" && !setRadioRouteAction(turnId, id, "awaiting_confirmation")) continue;
            pendingRouteMemoryTurn = turnId; pendingRouteMemoryAction = id;
            const auto parameters = action.value("parameters").toObject();
            radioDialog->setPreferenceProposal(turnId + QLatin1Char(':') + id,
                parameters.value("operation").toString(), parameters.value("preference").toString());
            appendRadioReply(tr("已找回未确认的口味提议。请核对后输入“%1”或“取消口味”；恢复本身不会保存或执行后续操作。")
                .arg(parameters.value("operation").toString() == "clear" ? tr("确认清除口味") : tr("确认保存口味")), turnId);
        } else if (status == "pending") waiting = true;
    }
    if (waiting && pendingRouteMemoryTurn != turnId)
        appendRadioReply(tr("已找回尚未派发的步骤。核对当前需求后输入“继续未完成步骤”；已发出但结果未知的操作只会查询，不会重发。"), turnId);
    if (musicTaskClient->snapshot().isEmpty()) radioDialog->setBusy(false);
    observeRadioRequests();
}

void MainWindow::observeRadioRequests()
{
    if (!radioDialog || !AppBackendClient::instance()->authenticated() || musicTaskClient->busy()) return;
    for (const auto &turnId : routedRadioOrder) {
        const auto turn = routedRadioTurns.value(turnId);
        for (const auto &value : turn.plan.value("actions").toArray()) {
            const auto action = value.toObject(); const auto id = action.value("id").toString();
            if (turn.actionStates.value(id) != "unknown") continue;
            const auto capability = action.value("capability").toString();
            if (capability != "music.task" && capability != "task.control") continue;
            const auto requestId = capability == "music.task" ? turnId : QStringLiteral("route-") +
                QString::fromLatin1(QCryptographicHash::hash((turnId + QLatin1Char('\n') + id).toUtf8(), QCryptographicHash::Sha256).toHex());
            if (observedRadioRequests.contains(requestId)) continue;
            observedRadioRequests.insert(requestId);
            routedRadioRequests.insert(requestId, qMakePair(turnId, id));
            // 旧操作只核对提交事实，不能抢占较新的可见任务或取消其本机工具。
            const bool selectTask = musicTaskClient->snapshot().value("id").toString().isEmpty() &&
                (turnId == latestConversationTurn() || turn.confirmationTurn == latestConversationTurn());
            if (selectTask && capability == "music.task") {
                const auto parameters = action.value("parameters").toObject();
                pendingConversationTask = {parameters.value("taskId").toString(), turnId, turn.instruction,
                    parameters.value("mode").toString() == "continue" ? parameters.value("expectedRevision").toInt() + 1 : 1};
            }
            const auto parameters = action.value("parameters").toObject();
            QJsonObject expectedFacts{{"action", "create"}};
            if (capability == "task.control" || parameters.value("mode").toString() == "continue") {
                expectedFacts = {{"action", capability == "task.control" ? parameters.value("action") : QJsonValue("turn")},
                    {"taskId", parameters.value("taskId")}, {"expectedRevision", parameters.value("expectedRevision")}};
            }
            musicTaskClient->observeRequest(requestId, expectedFacts, selectTask);
            if (selectTask) return;
        }
    }
}

void MainWindow::applyRadioRoute(const QString &turnId, const QJsonObject &plan)
{
    if (!radioDialog || !AppBackendClient::instance()->authenticated() ||
        !AppBackendClient::instance()->routingAvailable() || routedRadioTurns.contains(turnId)) return;
    QString instruction;
    for (const auto &value : conversationClient->snapshot().value("messages").toArray()) {
        const auto entry = value.toObject();
        if (entry.value("id").toString() == turnId && entry.value("role").toString() == "user") instruction = entry.value("content").toString();
    }
    if (instruction.isEmpty() || plan.value("messageId").toString() != turnId) return;
    if (turnId != latestConversationTurn()) {
        appendRadioReply(tr("已有更新的消息，较早的路由未改动当前候选或执行操作。"), turnId); return;
    }
    const auto lookupInput = radioLookupInputs.take(turnId);
    const auto planActions = plan.value("actions").toArray();
    const auto onlyAction = planActions.size() == 1 ? planActions.first().toObject() : QJsonObject{};
    const bool refining = onlyAction.value("capability").toString() == "music.refine";
    const bool clarification = onlyAction.value("capability").toString() == "assistant.reply" &&
        onlyAction.value("parameters").toObject().value("kind").toString() == "clarify";
    if (clarification && plan.value("intents").toArray() == QJsonArray{"song_lookup"} && LookupContext::related(instruction) &&
        !lookupInput.isEmpty() && lookupInput == radioLookupContext &&
        !radioLookupContextFor(turnId).isEmpty()) radioLookupContextLastTurn = turnId;
    else if (!refining) clearRadioLookupContext(true);
    RoutedRadioTurn turn; turn.instruction = instruction; turn.plan = plan;
    turn.preferenceBase = routePreferenceBases.take(turnId);
    turn.baseRevision = routePreferenceRevisions.take(turnId);
    QString error;
    if (!RadioStore(QSqlDatabase::database()).journalRoute(currentProfile, conversationClient->deviceId(), visibleConversationId,
            turnId, plan, turn.preferenceBase, turn.baseRevision, &error)) {
        radioDialog->setBusy(false); appendRadioReply(error, turnId); return;
    }
    for (const auto &value : plan.value("actions").toArray()) turn.actionStates.insert(value.toObject().value("id").toString(), "pending");
    // 先持久化再派发；同步信号或重复回执不能重复执行或重新打开确认。
    routedRadioTurns.insert(turnId, turn); routedRadioOrder.append(turnId);
    radioDialog->setRouteObservationAvailable({});
    if (!musicTaskClient->snapshot().isEmpty()) radioDialog->setTaskSnapshot(musicTaskClient->snapshot());
    else radioDialog->setBusy(false);
    continueRadioRoute(turnId);
    scheduleRadioUsageRefresh();
}

void MainWindow::continueRadioRoute(const QString &turnId)
{
    if (!radioDialog || !AppBackendClient::instance()->authenticated() || !routedRadioTurns.contains(turnId) ||
        routedRadioTurns.value(turnId).restored) return;
    const auto actions = routedRadioTurns.value(turnId).plan.value("actions").toArray();
    for (int pass = 0; pass < actions.size(); ++pass) {
        for (const auto &value : actions) {
            const auto action = value.toObject(); const auto id = action.value("id").toString();
            if (routedRadioTurns.value(turnId).actionStates.value(id) != "pending") continue;
            bool ready = true, rejected = false;
            for (const auto &dependency : action.value("dependsOn").toArray()) {
                const auto status = routedRadioTurns.value(turnId).actionStates.value(dependency.toString());
                if (status == "cancelled" || status == "failed" || status == "skipped") rejected = true;
                if (status != "completed") ready = false;
            }
            if (rejected) {
                if (!setRadioRouteAction(turnId, id, "skipped")) return;
                appendRadioReply(tr("一项后续操作依赖未确认或失败的步骤，因此没有执行；独立要求会继续处理。"), turnId);
                continue;
            }
            if (!ready) continue;
            const auto capability = action.value("capability").toString();
            const auto parameters = action.value("parameters").toObject();
            if (capability == "assistant.reply") {
                // The backend already recorded this text with source=route.
                if (!setRadioRouteAction(turnId, id, "completed")) return; continue;
            }
            if (capability == "memory.propose") {
                if (!pendingRouteMemoryTurn.isEmpty()) continue;
                if (!setRadioRouteAction(turnId, id, "awaiting_confirmation")) return;
                pendingRouteMemoryTurn = turnId; pendingRouteMemoryAction = id;
                radioDialog->setPreferenceProposal(turnId + QLatin1Char(':') + id,
                    parameters.value("operation").toString(), parameters.value("preference").toString());
                continue;
            }
            if (capability == "music.lookup" || capability == "music.artist_catalog" || capability == "music.playlist_search") {
                if (turnId != latestConversationTurn()) {
                    if (!setRadioRouteAction(turnId, id, "skipped")) return;
                    continue;
                }
                if (!setRadioRouteAction(turnId, id, "dispatched")) return;
                if (capability == "music.artist_catalog") lookupRadioArtist(turnId, id, parameters);
                else if (capability == "music.playlist_search") lookupRadioPlaylists(turnId, id, parameters);
                else lookupRadioSong(turnId, id, parameters);
                continue;
            }
            if (capability == "music.refine") {
                if (turnId != latestConversationTurn()) {
                    if (!setRadioRouteAction(turnId, id, "skipped")) return;
                    continue;
                }
                if (!setRadioRouteAction(turnId, id, "dispatched")) return;
                refineRadioSong(turnId, id, parameters);
                continue;
            }
            if (musicTaskClient->busy()) continue;
            if (capability == "music.task") {
                const auto mode = parameters.value("mode").toString();
                const auto currentTask = musicTaskClient->snapshot();
                const auto latest = latestConversationTurn();
                const auto turn = routedRadioTurns.value(turnId);
                if ((turnId != latest && turn.confirmationTurn != latest) || (mode == "continue" &&
                    (parameters.value("taskId").toString() != currentTask.value("id").toString() ||
                     parameters.value("expectedRevision").toInt() != currentTask.value("revision").toInt()))) {
                    if (!setRadioRouteAction(turnId, id, "skipped")) return;
                    appendRadioReply(tr("已有更新的对话要求或任务版本，这条较早的找歌操作未继续执行；请在当前上下文重新提出。"), turnId);
                    continue;
                }
                if (!AppBackendClient::instance()->taskRuntimeAvailable() ||
                    (mode == "continue" && musicTaskClient->snapshot().value("id").toString().isEmpty())) {
                    if (!setRadioRouteAction(turnId, id, "failed")) return;
                    appendRadioReply(tr("当前没有可继续修改的音乐任务，或后台未开放此能力；请说明想新找哪些歌曲。"), turnId); continue;
                }
                if (!setRadioRouteAction(turnId, id, "dispatched")) return;
                routedRadioRequests.insert(turnId, qMakePair(turnId, id));
                if (mode == "new" && !musicTaskClient->snapshot().isEmpty()) musicTaskClient->detach();
                const auto current = musicTaskClient->snapshot();
                radioDialog->setTaskMode(true); radioDialog->clearArtistChoices(); radioResultIsTask = true;
                const auto instruction = routedRadioTurns.value(turnId).instruction;
                pendingConversationTask = {current.value("id").toString(), turnId, instruction,
                    current.value("id").toString().isEmpty() ? 1 : current.value("revision").toInt() + 1};
                musicTaskClient->submit(instruction, RadioStore(QSqlDatabase::database()).preference(currentProfile), radioSeen, turnId);
            } else if (capability == "task.control") {
                const auto current = musicTaskClient->snapshot();
                const auto turn = routedRadioTurns.value(turnId);
                if (turnId != latestConversationTurn() && turn.confirmationTurn != latestConversationTurn()) {
                    if (!setRadioRouteAction(turnId, id, "skipped")) return;
                    appendRadioReply(tr("已有更新的对话要求，这条较早的任务控制未执行；请基于当前任务重新提出。"), turnId); continue;
                }
                if (current.value("id").toString() != parameters.value("taskId").toString() ||
                    current.value("revision").toInt() != parameters.value("expectedRevision").toInt()) {
                    if (!setRadioRouteAction(turnId, id, "failed")) return;
                    appendRadioReply(tr("任务已变化，这条控制没有执行；请基于当前任务重新提出要求。"), turnId); continue;
                }
                if (!setRadioRouteAction(turnId, id, "dispatched")) return;
                const auto requestId = QStringLiteral("route-") + QString::fromLatin1(QCryptographicHash::hash(
                    (turnId + QLatin1Char('\n') + id).toUtf8(), QCryptographicHash::Sha256).toHex());
                routedRadioRequests.insert(requestId, qMakePair(turnId, id));
                musicTaskClient->action(parameters.value("action").toString(), {}, {}, requestId);
            }
        }
    }
}

void MainWindow::decideRadioPreference(const QString &actionId, bool confirmed, const QString &preference)
{
    const auto turnId = pendingRouteMemoryTurn, id = pendingRouteMemoryAction;
    if (!radioDialog || !AppBackendClient::instance()->authenticated() || turnId.isEmpty() ||
        actionId != turnId + QLatin1Char(':') + id || !routedRadioTurns.contains(turnId) ||
        routedRadioTurns.value(turnId).actionStates.value(id) != "awaiting_confirmation") return;
    auto store = RadioStore(QSqlDatabase::database());
    QString error;
    const auto confirmation = routedRadioTurns.value(turnId).confirmationTurn.isEmpty()
        ? turnId : routedRadioTurns.value(turnId).confirmationTurn;
    QJsonObject receipt;
    // 确认锚点先保存；偏好CAS、完成标记和可重送回复在同一SQLite事务中提交。
    if (!store.setRouteConfirmationTurn(currentProfile, conversationClient->deviceId(), visibleConversationId,
            turnId, confirmation, &error) ||
        !store.decidePreference(currentProfile, conversationClient->deviceId(), visibleConversationId,
            turnId, id, confirmed, preference, &receipt, &error)) {
        const auto saved = store.preferenceSnapshot(currentProfile);
        if (!saved.isEmpty() && qint64(saved.value("revision").toDouble()) != routedRadioTurns.value(turnId).baseRevision) {
            setRadioRouteAction(turnId, id, "failed");
            radioDialog->clearPreferenceProposal(actionId);
            pendingRouteMemoryTurn.clear(); pendingRouteMemoryAction.clear();
            appendRadioReply(tr("长期口味在这条提议之后已被更新，本次未用旧摘要覆盖它。请基于当前口味重新提出或编辑。"), turnId);
            routedRadioTurns[turnId].restored = false; continueRadioRoute(turnId);
        } else {
            for (const auto &value : routedRadioTurns.value(turnId).plan.value("actions").toArray()) {
                const auto action = value.toObject();
                if (action.value("id").toString() == id)
                    radioDialog->setPreferenceProposal(actionId, action.value("parameters").toObject().value("operation").toString(), preference);
            }
            radioDialog->setPlaybackStatus(error);
            appendRadioReply(error.isEmpty() ? tr("确认尚未保存，请检查本机存储后重试或取消。") : error, turnId);
        }
        return;
    }
    radioDialog->setPreference(store.preference(currentProfile));
    routedRadioTurns[turnId].actionStates[id] = confirmed ? "completed" : "cancelled";
    routedRadioTurns[turnId].restored = false;
    appendRadioReply(receipt.value("text").toString(), turnId);
    radioDialog->clearPreferenceProposal(actionId);
    pendingRouteMemoryTurn.clear(); pendingRouteMemoryAction.clear();
    const auto order = routedRadioOrder;
    for (const auto &turn : order) continueRadioRoute(turn);
}

QByteArray MainWindow::radioQueueDigest() const
{
    QByteArray contents;
    for (const auto &track : tracks) {
        contents += track.url.toEncoded() + '\n' + QByteArray::number(track.id) + '\n';
    }
    return QCryptographicHash::hash(contents, QCryptographicHash::Sha256);
}

bool MainWindow::executeRadioChatCommand(const QString &request, const QString &turnId)
{
    const auto text = request.trimmed();
    const auto conversation = conversationClient->snapshot().value("id").toString();
    static const QRegularExpression readPreference(QStringLiteral(
        "^(?:查看(?:一下)?(?:我的)?(?:长期)?(?:音乐)?(?:口味|偏好|喜好|记忆)|看看我(?:的)?(?:口味|偏好)|你记住了(?:我(?:的)?)?(?:哪些口味|什么|哪些偏好)|你还记得我喜欢什么(?:吗)?)[。！!？?]?$"));
    if (readPreference.match(text).hasMatch()) {
        pendingClearQueueDigest.clear(); pendingClearQueueConversation.clear();
        // 只读当前账号在本机确认过的值，模型提议与聊天推测不能冒充已保存记忆。
        const auto summary = RadioStore(QSqlDatabase::database()).preference(currentProfile);
        appendRadioReply(summary.isEmpty()
            ? tr("当前账号在本机还没有确认保存的长期口味。可以说“记住我喜欢粤语”，核对提议后再确认保存。")
            : tr("当前账号在本机已确认保存的长期口味：%1。临时场景和未确认提议不在其中。").arg(summary), turnId);
        return true;
    }
    if (text == QStringLiteral("确认清空待播放队列")) {
        const bool valid = !pendingClearQueueDigest.isEmpty() && pendingClearQueueConversation == conversation &&
            pendingClearQueueDigest == radioQueueDigest();
        pendingClearQueueDigest.clear(); pendingClearQueueConversation.clear();
        if (!valid) {
            appendRadioReply(tr("没有待确认的队列清空操作，或队列已变化。请重新输入“清空待播放队列”核对范围。"), turnId);
            return true;
        }
        if (focusSession) focusSession->cancel(tr("已确认清空待播放队列"));
        endRadioPlayback();
        ++mediaGeneration; playbackRequested = false; resolvingTrack = false; preparedTrack = -1;
        p_PlayerObject->stop(); p_PlayerObject->setMedia(QMediaContent()); p_PlayerList->clear();
        tracks.clear(); trackIndexes.clear(); trackDurations.clear(); lastHistoryUrl.clear(); ui->queueModel->clear();
        ui->setNowPlaying(tr("还没有播放音乐"), {}, {});
        appendRadioReply(tr("已清空当前待播放队列。收藏、播放历史和磁盘文件均保留。"), turnId);
        return true;
    }
    // 确认只对紧接着的当前会话指令有效；跨话题、切账号或重启不能沿用。
    pendingClearQueueDigest.clear(); pendingClearQueueConversation.clear();
    if (QRegularExpression(QStringLiteral("^(?:请|帮我|请帮我)?(?:删除|删掉|清空|移除)(?:全部|所有)(?:歌曲|音乐)[。！!？?]?$"))
            .match(text).hasMatch()) {
        appendRadioReply(tr("你想清空待播放队列，还是删除收藏或历史？如需清空队列，请输入“清空待播放队列”，我会先列明范围再请你确认。"), turnId);
        return true;
    }
    if (QStringList{QStringLiteral("清空待播放队列"), QStringLiteral("帮我清空待播放队列"), QStringLiteral("清空播放队列")}.contains(text)) {
        if (tracks.isEmpty()) appendRadioReply(tr("当前待播放队列已经是空的。"), turnId);
        else {
            pendingClearQueueDigest = radioQueueDigest(); pendingClearQueueConversation = conversation;
            appendRadioReply(tr("将停止播放并移除待播放队列中的 %1 首歌；收藏、历史和磁盘文件保留。确认请输入“确认清空待播放队列”，取消请输入“取消清空”。").arg(tracks.size()), turnId);
        }
        return true;
    }
    if (text == QStringLiteral("取消清空")) {
        appendRadioReply(tr("已取消清空，队列保留。"), turnId); return true;
    }
    if (text == QStringLiteral("继续未完成步骤") || text == QStringLiteral("核对未完成步骤")) {
        QString previousUser;
        for (const auto &value : conversationClient->snapshot().value("messages").toArray()) {
            const auto message = value.toObject();
            if (message.value("role").toString() != "user") continue;
            if (message.value("id").toString() == turnId) break;
            previousUser = message.value("id").toString();
        }
        bool released = false;
        for (const auto &original : routedRadioOrder) {
            auto &turn = routedRadioTurns[original];
            if (!turn.restored || (original != previousUser && turn.confirmationTurn != previousUser)) continue;
            bool awaiting = false, pending = false;
            for (const auto &state : turn.actionStates) { awaiting |= state == "awaiting_confirmation"; pending |= state == "pending"; }
            if (awaiting || !pending || text == QStringLiteral("核对未完成步骤")) continue;
            QString error;
            if (!RadioStore(QSqlDatabase::database()).setRouteConfirmationTurn(currentProfile, conversationClient->deviceId(),
                    visibleConversationId, original, turnId, &error)) { radioDialog->setPlaybackStatus(error); return true; }
            turn.confirmationTurn = turnId; turn.restored = false; released = true;
            continueRadioRoute(original);
        }
        observedRadioRequests.clear(); observeRadioRequests();
        if (!released) appendRadioReply(tr("已核对原请求；未知结果不会重发。若有待确认口味，请先确认或取消；已有新话题的旧步骤请重新提出。"), turnId);
        return true;
    }
    const auto memoryTurn = pendingRouteMemoryTurn;
    const bool memoryConfirmation = text == QStringLiteral("确认保存口味") || text == QStringLiteral("确认清除口味");
    if (memoryConfirmation && !memoryTurn.isEmpty() && routedRadioTurns.contains(memoryTurn)) {
        QString previousUser;
        const auto journal = conversationClient->snapshot().value("messages").toArray();
        for (const auto &value : journal) {
            const auto message = value.toObject();
            if (message.value("role").toString() != "user") continue;
            if (message.value("id").toString() == turnId) break;
            previousUser = message.value("id").toString();
        }
        // 紧接原提议的明确确认可续接依赖动作；跨话题确认仍不能唤醒旧的找歌请求。
        if (previousUser == memoryTurn) routedRadioTurns[memoryTurn].confirmationTurn = turnId;
    }
    if (radioDialog->executeChatAction(text)) return true;
    if (memoryConfirmation && routedRadioTurns.contains(memoryTurn)) routedRadioTurns[memoryTurn].confirmationTurn.clear();
    if (memoryConfirmation || text == QStringLiteral("取消口味") || text == QStringLiteral("取消记忆")) {
        appendRadioReply(tr("当前没有可确认或取消的口味提议。正在恢复时请等待核验；已过期、跨会话或失效的提议请重新提出。"), turnId);
        return true;
    }
    if (QStringList{QStringLiteral("保留前三首"), QStringLiteral("锁定前三首"),
                    QStringLiteral("解除歌曲锁定"), QStringLiteral("解除锁定")}.contains(text)) {
        appendRadioReply(tr("当前没有可锁定或解除锁定的任务预览。请先生成歌单，等待完成或先处理当前澄清。"), turnId);
        return true;
    }
    if (text == QStringLiteral("采用歌单") || text == QStringLiteral("开始收听") ||
        QRegularExpression(QStringLiteral("^播放第\\s*([0-9]{1,2}|[一二三四五六七八九十])\\s*首$")).match(text).hasMatch()) {
        const auto candidates = radioDialog->recommendations();
        appendRadioReply(candidates.isEmpty()
            ? tr("当前还没有可以采用或播放的歌曲。请先搜索一首歌或描述想听的场景。")
            : tr("当前操作暂不可用。请核对歌曲序号，并等待当前任务完成或先处理歌手澄清。"), turnId);
        return true;
    }
    return false;
}

void MainWindow::lookupRadioArtist(const QString &turnId, const QString &actionId, const QJsonObject &parameters,
                                 const QJsonObject &resolved)
{
    clearRadioLookupContext();
    radioLookupOperation = new QObject(this);
    const auto generation = ++radioLookupGeneration, epoch = radioAccountEpoch;
    const auto profile = currentProfile, conversation = visibleConversationId;
    const auto queryName = parameters.value("artist").toString();
    const int limit = qBound(1, parameters.value("limit").toInt(12), 30);
    const auto current = [this, generation, epoch, profile, conversation, turnId] {
        if (generation != radioLookupGeneration || epoch != radioAccountEpoch || profile != currentProfile ||
            conversation != visibleConversationId || !radioDialog || !AppBackendClient::instance()->authenticated() ||
            !routedRadioTurns.contains(turnId)) return false;
        const auto turn = routedRadioTurns.value(turnId);
        return latestConversationTurn() == turnId || (!turn.confirmationTurn.isEmpty() && latestConversationTurn() == turn.confirmationTurn);
    };
    const auto fail = [this, current, turnId, actionId](const QString &text) {
        if (!current()) return;
        radioDialog->setPlaybackStatus({});
        if (setRadioRouteAction(turnId, actionId, "failed")) appendRadioReply(text, turnId);
    };
    const auto fetch = [this, current, fail, turnId, actionId, limit](const QJsonObject &artist) {
        if (!current()) return;
        const auto id = catalogIdentifier(artist.value("id"));
        const auto name = artist.value("name").toString();
        if (!id || name.isEmpty()) { fail(tr("未取得可核验的歌手身份，请补充姓名。")); return; }
        if (radioDialog) radioDialog->clearArtistChoices();
        QUrlQuery query; query.addQueryItem("artistId", QString::number(id));
        query.addQueryItem("limit", QString::number(limit)); query.addQueryItem("offset", "0");
        netease()->get("/artist/tracks", query, radioLookupOperation,
            [this, current, fail, turnId, actionId, id, name, limit](const QJsonObject &response, const QString &error) {
                if (!current()) return;
                if (!error.isEmpty()) { fail(tr("歌手作品查询未完成：%1").arg(error.left(250))); return; }
                if (catalogIdentifier(response.value("artistId")) != id || !response.value("songs").isArray()) {
                    fail(tr("曲库未返回与所选歌手一致的作品信息，未展示未经核验的歌曲。")); return;
                }
                radioDialog->setPlaybackStatus({});
                QVector<RadioRecommendation> songs; QSet<qint64> seen;
                for (const auto &row : response.value("songs").toArray()) {
                    const auto item = row.toObject(); const auto song = catalogSong(item);
                    if (!song.id || !catalogCredit(item, id) || seen.contains(song.id)) continue;
                    seen.insert(song.id); songs.append(RadioRecommendation{song, tr("曲库艺人ID与所选歌手一致"), tr("艺人作品查询：%1").arg(name), {}});
                    if (songs.size() >= limit) break;
                }
                musicTaskClient->detach(); pendingAdoptTask.clear(); pendingAdoptVersion = 0;
                radioResultIsTask = false; radioDialog->setTaskSnapshot({}); radioDialog->setTaskMode(false);
                radioTitle = tr("%1 · 歌曲").arg(name); radioNote = tr("按曲库艺人身份查询；点击歌曲播放单首。");
                radioRecommendations = songs;
                radioDialog->setRecommendations(radioTitle, radioNote, songs, limit);
                radioRecommendations = radioDialog->recommendations();
                if (!setRadioRouteAction(turnId, actionId, "completed")) return;
                appendRadioReply(songs.isEmpty() ? tr("已找到歌手 %1，但曲库没有返回可核验的作品。可以指定歌名继续查找。").arg(name)
                    : tr("已找到 %1 的歌曲，当前展示 %2 首。已按艺人ID筛选，未自动播放；可以直接点击歌曲。")
                        .arg(name).arg(radioRecommendations.size()), turnId);
                continueRadioRoute(turnId);
            });
    };
    if (!resolved.isEmpty()) { fetch(resolved); return; }
    radioDialog->setPlaybackStatus(tr("正在确认歌手身份并查找作品…"));
    QUrlQuery query; query.addQueryItem("keywords", queryName); query.addQueryItem("limit", "5");
    netease()->get("/assistant/artists", query, radioLookupOperation,
        [this, current, fail, fetch, queryName, parameters, turnId, actionId, profile, conversation]
        (const QJsonObject &response, const QString &error) {
            if (!current()) return;
            if (!error.isEmpty()) { fail(tr("歌手身份查询未完成：%1").arg(error.left(250))); return; }
            QJsonArray candidates, matches; QSet<qint64> seen; QVector<AssistantArtist> choices;
            for (const auto &value : response.value("artists").toArray()) {
                const auto item = value.toObject(); const auto id = catalogIdentifier(item.value("id"));
                const auto name = item.value("name").toString().trimmed();
                if (!id || name.isEmpty() || name.size() > 120 || seen.contains(id) || candidates.size() >= 5) continue;
                seen.insert(id); candidates.append(item);
                AssistantArtist artist; artist.id = id; artist.name = name; artist.coverUrl = item.value("coverUrl").toString();
                bool exact = catalogIdentity(name) == catalogIdentity(queryName);
                for (const auto &alias : item.value("aliases").toArray()) {
                    if (alias.isString() && alias.toString().size() <= 100) {
                        artist.aliases.append(alias.toString()); exact |= catalogIdentity(alias.toString()) == catalogIdentity(queryName);
                    }
                }
                choices.append(artist); if (exact) matches.append(item);
            }
            if (matches.size() == 1) { fetch(matches.first().toObject()); return; }
            if (choices.isEmpty()) { fail(tr("没有找到这位歌手，请补充完整姓名。")); return; }
            radioDialog->setPlaybackStatus({});
            musicTaskClient->detach(); radioResultIsTask = false; radioDialog->setTaskSnapshot({}); radioDialog->setTaskMode(false);
            setProperty("tangoCatalogChoices", QJsonObject{{"turn", turnId}, {"action", actionId}, {"parameters", parameters},
                {"artists", candidates}, {"profile", profile}, {"conversation", conversation}});
            radioDialog->setArtistChoices(choices);
            appendRadioReply(tr("找到多个可能的歌手，请在这些候选中确认身份；不会直接采用搜索排名第一项。"), turnId);
        });
}

void MainWindow::lookupRadioPlaylists(const QString &turnId, const QString &actionId, const QJsonObject &parameters)
{
    clearRadioLookupContext(); radioLookupOperation = new QObject(this);
    const auto generation = ++radioLookupGeneration, epoch = radioAccountEpoch;
    const auto profile = currentProfile, conversation = visibleConversationId;
    QUrlQuery query; query.addQueryItem("keywords", parameters.value("keyword").toString());
    query.addQueryItem("limit", QString::number(qBound(1, parameters.value("limit").toInt(5), 5))); query.addQueryItem("offset", "0");
    netease()->get("/radio/search", query, radioLookupOperation,
        [this, generation, epoch, profile, conversation, turnId, actionId](const QJsonObject &response, const QString &error) {
            if (generation != radioLookupGeneration || epoch != radioAccountEpoch || profile != currentProfile ||
                conversation != visibleConversationId || !radioDialog || latestConversationTurn() != turnId ||
                !routedRadioTurns.contains(turnId) || !AppBackendClient::instance()->authenticated()) return;
            if (!error.isEmpty()) {
                if (setRadioRouteAction(turnId, actionId, "failed")) appendRadioReply(tr("歌单搜索未完成：%1").arg(error.left(250)), turnId);
                return;
            }
            QJsonArray playlists;
            for (const auto &value : response.value("playlists").toArray()) {
                const auto item = value.toObject();
                if (catalogIdentifier(item.value("id")) && !item.value("name").toString().trimmed().isEmpty()) playlists.append(item);
                if (playlists.size() >= 5) break;
            }
            musicTaskClient->detach(); radioResultIsTask = false;
            radioDialog->setTaskSnapshot({}); radioDialog->setTaskMode(false); radioRecommendations.clear();
            radioDialog->setRecommendations({}, {}, {});
            setProperty("tangoPlaylistContext", QJsonObject{{"playlists", playlists}, {"turn", turnId},
                {"profile", profile}, {"conversation", conversation}, {"generation", double(generation)}});
            radioDialog->setPlaylistCandidates(playlists);
            if (!setRadioRouteAction(turnId, actionId, "completed")) return;
            appendRadioReply(playlists.isEmpty() ? tr("曲库没有返回匹配歌单，可以换个关键词。")
                : tr("已找到 %1 份现有歌单，点击卡片查看曲目；尚未播放或替换队列。").arg(playlists.size()), turnId);
            continueRadioRoute(turnId);
        });
}

void MainWindow::openRadioPlaylist(qint64 id, const QString &name)
{
    const auto context = property("tangoPlaylistContext").toJsonObject();
    const auto turn = context.value("turn").toString();
    if (!radioDialog || !AppBackendClient::instance()->authenticated() || context.isEmpty() || radioConversationsClearing ||
        context.value("profile").toString() != currentProfile || context.value("conversation").toString() != visibleConversationId ||
        context.value("generation").toDouble(-1) != double(radioLookupGeneration) || turn != latestConversationTurn()) return;
    bool listed = false;
    for (const auto &value : context.value("playlists").toArray())
        listed |= catalogIdentifier(value.toObject().value("id")) == id && value.toObject().value("name").toString() == name;
    if (!listed) return;
    delete radioLookupOperation.data(); radioLookupOperation = new QObject(this);
    const auto generation = ++radioLookupGeneration, epoch = radioAccountEpoch;
    const auto profile = currentProfile, conversation = visibleConversationId;
    setProperty("tangoPlaylistContext", QJsonObject{});
    QUrlQuery query; query.addQueryItem("id", QString::number(id)); query.addQueryItem("limit", "20");
    netease()->get("/radio/playlist", query, radioLookupOperation,
        [this, generation, epoch, profile, conversation, turn, id, name](const QJsonObject &response, const QString &error) {
            if (generation != radioLookupGeneration || epoch != radioAccountEpoch || profile != currentProfile ||
                conversation != visibleConversationId || !radioDialog || turn != latestConversationTurn() ||
                !AppBackendClient::instance()->authenticated()) return;
            if (!error.isEmpty() || catalogIdentifier(response.value("playlist").toObject().value("id")) != id) {
                appendRadioReply(tr("歌单曲目暂未读取成功，请重新搜索后选择；尚未播放。"), turn); return;
            }
            QVector<RadioRecommendation> songs; QSet<qint64> seen;
            for (const auto &value : response.value("songs").toArray()) {
                const auto song = catalogSong(value.toObject());
                if (song.id && !seen.contains(song.id)) { seen.insert(song.id); songs.append(RadioRecommendation{song, tr("来自歌单 %1").arg(name), name, {}}); }
                if (songs.size() >= 20) break;
            }
            radioRecommendations = songs; radioTitle = name; radioNote = tr("现有歌单曲目预览；点击歌曲播放单首。");
            radioDialog->setRecommendations(name, radioNote, songs, 20); radioRecommendations = radioDialog->recommendations();
            appendRadioReply(tr("已展开《%1》，当前展示 %2 首歌曲；未自动播放或保存口味。")
                .arg(name).arg(radioRecommendations.size()), turn);
        });
}

void MainWindow::lookupRadioSong(const QString &turnId, const QString &actionId, const QJsonObject &parameters)
{
    clearRadioLookupContext();
    radioDialog->clearArtistChoices();
    delete radioLookupOperation.data();
    radioLookupOperation = new QObject(this);
    const auto generation = ++radioLookupGeneration, accountEpoch = radioAccountEpoch;
    const auto profile = currentProfile;
    const auto conversation = conversationClient->snapshot().value("id").toString();
    const auto artist = parameters.value("artist").toString(), title = parameters.value("title").toString();
    const auto requestedVersion = parameters.value("version").toString();
    const bool shouldPlay = parameters.value("play").toBool();
    radioDialog->setPlaybackStatus(tr("正在曲库查找 %1 %2…").arg(artist, title));
    const auto search = [this, generation, accountEpoch, profile, conversation, turnId, actionId, artist, title, requestedVersion, shouldPlay](qint64 resolvedArtistId) {
    if (generation != radioLookupGeneration || accountEpoch != radioAccountEpoch || profile != currentProfile ||
        conversation != visibleConversationId || !radioDialog || !AppBackendClient::instance()->authenticated()) return;
    QUrlQuery query; query.addQueryItem("keywords", (artist + QLatin1Char(' ') + title).trimmed());
    const bool mayPlay = shouldPlay && (artist.isEmpty() || resolvedArtistId > 0);
    query.addQueryItem("limit", "20"); query.addQueryItem("offset", "0");
    netease()->get("/search", query, radioLookupOperation,
        [this, generation, accountEpoch, profile, conversation, turnId, actionId, artist, title, requestedVersion, shouldPlay, resolvedArtistId, mayPlay]
        (const QJsonObject &response, const QString &failure) {
        if (generation != radioLookupGeneration || accountEpoch != radioAccountEpoch || profile != currentProfile ||
            !radioDialog || !AppBackendClient::instance()->authenticated() ||
            conversation != conversationClient->snapshot().value("id").toString() || !routedRadioTurns.contains(turnId)) return;
        if (latestConversationTurn() != turnId) {
            radioDialog->setPlaybackStatus({});
            if (!setRadioRouteAction(turnId, actionId, "skipped")) return;
            appendRadioReply(tr("已有更新的消息，本次较早的查找未切换选曲或启动播放，请重新提出。"), turnId); return;
        }
        if (!failure.isEmpty()) {
            radioDialog->setPlaybackStatus({});
            if (!setRadioRouteAction(turnId, actionId, "failed")) return;
            appendRadioReply(tr("曲库搜索暂未完成：%1。可以稍后重新搜索。").arg(failure.left(300)), turnId); return;
        }
        const auto normalize = [](QString text) {
            text = text.normalized(QString::NormalizationForm_KC).toCaseFolded();
            text.remove(QRegularExpression(QStringLiteral("[\\s\\p{P}\\p{S}]+")));
            return text;
        };
        const auto versionOf = [](const OnlineSong &song) {
            const auto metadata = song.title + QLatin1Char(' ') + song.album;
            const bool live = song.version == QStringLiteral("live") ||
                QRegularExpression(QStringLiteral("\\blive\\b|现场|演唱会|音乐会"), QRegularExpression::CaseInsensitiveOption).match(metadata).hasMatch();
            const bool studio = song.version == QStringLiteral("studio") ||
                QRegularExpression(QStringLiteral("\\bstudio\\b|录音室"), QRegularExpression::CaseInsensitiveOption).match(metadata).hasMatch();
            if (live != studio) return live ? QStringLiteral("live") : QStringLiteral("studio");
            // 没有版本证据的曲库元数据保持未知，不能把‘没写Live’当作录音室版。
            return QString();
        };
        const auto baseTitle = [](QString value) {
            value.remove(QRegularExpression(QStringLiteral("\\s*[（(\\[【](?:[^）)\\]】]*(?:\\blive\\b|\\bstudio\\b|现场|录音室)[^）)\\]】]*)[）)\\]】]\\s*$"),
                QRegularExpression::CaseInsensitiveOption));
            return value;
        };
        QVector<RadioRecommendation> found, exact;
        QSet<qint64> ids, unsafeContextIds;
        for (const auto &value : response.value("songs").toArray()) {
            const auto item = value.toObject();
            // 已取得真实身份时按艺人ID核验，展示姓名和别名不能冒充其他艺人的署名。
            if (resolvedArtistId > 0 && !catalogCredit(item, resolvedArtistId)) continue;
            const auto id = item.value("id").toDouble();
            OnlineSong song;
            if (!item.value("id").isDouble() || !std::isfinite(id) || id < 1 || id > 9007199254740991. || std::floor(id) != id) continue;
            song.id = qint64(id); song.title = item.value("title").toString().left(200);
            song.artist = item.value("artist").toString().left(200); song.album = item.value("album").toString().left(200);
            if (item.value("title").toString().size() > 200 || item.value("artist").toString().size() > 200 ||
                item.value("album").toString().size() > 200) unsafeContextIds.insert(song.id);
            song.coverUrl = item.value("coverUrl").toString().left(2048);
            const auto explicitVersion = item.value("version").toString().trimmed().toCaseFolded();
            if (QStringList{"studio", "录音室版", "录音室版本"}.contains(explicitVersion)) song.version = QStringLiteral("studio");
            else if (QStringList{"live", "现场版", "现场版本"}.contains(explicitVersion)) song.version = QStringLiteral("live");
            song.durationMs = qBound(0, item.value("durationMs").toInt(), 86400000); song.fee = item.value("fee").toInt();
            song.version = versionOf(song);
            if (song.title.isEmpty() || ids.contains(song.id)) continue;
            ids.insert(song.id);
            RadioRecommendation recommendation; recommendation.song = song; recommendation.reason = tr("网易云曲库搜索结果");
            if (found.size() < 10) found.append(recommendation);
            bool artistMatches = artist.isEmpty() || (resolvedArtistId > 0 && catalogCredit(item, resolvedArtistId));
            for (const auto &performer : song.artist.split(QRegularExpression(QStringLiteral("[/、,，;；]")), Qt::SkipEmptyParts))
                artistMatches = artistMatches || normalize(performer) == normalize(artist);
            const bool versionMatches = requestedVersion.isEmpty() || song.version == requestedVersion;
            if (normalize(baseTitle(song.title)) == normalize(baseTitle(title)) && artistMatches && versionMatches && exact.size() < 20)
                exact.append(recommendation);
        }
        radioDialog->setPlaybackStatus({});
        if (found.isEmpty()) {
            if (!setRadioRouteAction(turnId, actionId, "completed")) return;
            appendRadioReply(tr("这次曲库没有返回匹配的歌曲。请核对歌名、歌手，或换一个关键词。"), turnId); return;
        }
        // 单曲搜索与旧歌单分开；保留后台任务供以后恢复，避免旧轮询把新选曲覆盖。
        musicTaskClient->detach(); pendingAdoptTask.clear(); pendingAdoptVersion = 0;
        radioResultIsTask = false; radioDialog->setTaskSnapshot({}); radioDialog->setTaskMode(false);
        radioRecommendations = exact.isEmpty() ? found : exact;
        radioTitle = tr("%1 %2").arg(artist, title).trimmed(); radioNote = tr("来自曲库的歌曲信息；完整播放按当前账号权益核验。");
        radioDialog->setRecommendations(radioTitle, radioNote, radioRecommendations);
        bool contextSafe = true;
        const auto displayed = radioDialog->recommendations();
        for (const auto &recommendation : displayed) contextSafe = contextSafe && !unsafeContextIds.contains(recommendation.song.id);
        if (contextSafe) contextSafe = saveRadioLookupContext(turnId, actionId, displayed, true);
        if (!contextSafe) appendRadioReply(tr("本批候选信息过长，未启用跨轮指代。请明确歌名和歌手重新查询。"), turnId);
        if (!setRadioRouteAction(turnId, actionId, "completed")) return;
        if (mayPlay && exact.size() == 1) {
            playOnlineSong(exact.first().song);
            appendRadioReply(tr("已找到 %1《%2》，正在请求播放；完整播放或试听以音乐服务返回的账号权益为准。")
                .arg(exact.first().song.artist, exact.first().song.title), turnId);
        } else {
            appendRadioReply((shouldPlay && !mayPlay ? tr("曲库未提供唯一可核验的歌手身份，尚未自动播放。请先核对候选。") : !requestedVersion.isEmpty() && exact.isEmpty()
                ? tr("找到了候选，但曲库元数据不足以确认你指定的版本，尚未自动播放。")
                : shouldPlay ? tr("找到多个版本或尚未精确匹配，请核对后选择。") : tr("已找到 %1 首候选。").arg(radioRecommendations.size())) +
                tr("输入“播放第1首”可播放对应歌曲。"), turnId);
        }
        continueRadioRoute(turnId);
    });
    };
    if (artist.isEmpty()) { search(0); return; }
    QUrlQuery identity; identity.addQueryItem("keywords", artist); identity.addQueryItem("limit", "5");
    netease()->get("/assistant/artists", identity, radioLookupOperation,
        [this, generation, accountEpoch, profile, conversation, turnId, actionId, artist, search]
        (const QJsonObject &response, const QString &error) {
            if (generation != radioLookupGeneration || accountEpoch != radioAccountEpoch || profile != currentProfile ||
                conversation != visibleConversationId || !radioDialog || !AppBackendClient::instance()->authenticated() ||
                !routedRadioTurns.contains(turnId) || latestConversationTurn() != turnId) return;
            if (!error.isEmpty() || !response.value("artists").isArray()) {
                radioDialog->setPlaybackStatus({});
                if (setRadioRouteAction(turnId, actionId, "failed"))
                    appendRadioReply(tr("歌手身份查询暂未完成，未降级为未经身份核验的播放。请稍后重新查询。"), turnId);
                return;
            }
            QSet<qint64> exact;
            for (const auto &value : response.value("artists").toArray()) {
                const auto item = value.toObject(); const auto id = catalogIdentifier(item.value("id"));
                bool matches = catalogIdentity(item.value("name").toString()) == catalogIdentity(artist);
                for (const auto &alias : item.value("aliases").toArray()) matches |= catalogIdentity(alias.toString()) == catalogIdentity(artist);
                if (id && matches) exact.insert(id);
            }
            if (exact.size() > 1) {
                radioDialog->setPlaybackStatus({});
                if (setRadioRouteAction(turnId, actionId, "failed")) appendRadioReply(tr("存在同名歌手，请先明确歌手身份后再查这首歌；未播放。"), turnId);
                return;
            }
            // 没有唯一身份时仅显示按署名搜索的候选；上层禁止自动播放，不把名字匹配说成ID核验。
            search(exact.isEmpty() ? 0 : *exact.cbegin());
        });
}

void MainWindow::clearRadioLookupContext(bool clearPreview)
{
    if (!property("tangoCatalogChoices").toJsonObject().isEmpty() && radioDialog)
        radioDialog->clearArtistChoices();
    setProperty("tangoCatalogChoices", QJsonObject{});
    setProperty("tangoPlaylistContext", QJsonObject{});
    ++radioLookupGeneration;
    delete radioLookupOperation.data(); radioLookupOperation = nullptr;
    radioLookupContext = {}; radioLookupPool.clear(); radioLookupInputs.clear();
    radioLookupContextProfile.clear(); radioLookupContextDevice.clear(); radioLookupContextConversation.clear();
    radioLookupContextLastTurn.clear(); radioLookupContextDeadline = QDeadlineTimer(0, Qt::PreciseTimer);
    setProperty("tangoLookupContext", QJsonObject{});
    if (clearPreview && radioDialog && !radioResultIsTask) {
        radioRecommendations.clear(); radioTitle.clear(); radioNote.clear();
        radioDialog->setRecommendations({}, {}, {});
    }
}

QJsonObject MainWindow::radioLookupContextFor(const QString &turnId) const
{
    if (!conversationClient || !AppBackendClient::instance()->authenticated() || !LookupContext::valid(radioLookupContext) ||
        radioLookupContextDeadline.hasExpired() ||
        radioLookupContextProfile != currentProfile || radioLookupContextDevice != conversationClient->deviceId() ||
        radioLookupContextConversation != conversationClient->snapshot().value("id").toString()) return {};
    QString previous; bool found = false;
    for (const auto &value : conversationClient->snapshot().value("messages").toArray()) {
        const auto message = value.toObject();
        if (message.value("role").toString() != "user") continue;
        if (message.value("id").toString() == turnId) { found = true; break; }
        previous = message.value("id").toString();
    }
    return found && previous == radioLookupContextLastTurn ? radioLookupContext : QJsonObject{};
}

bool MainWindow::saveRadioLookupContext(const QString &turnId, const QString &actionId,
                                      const QVector<RadioRecommendation> &pool, bool initial)
{
    QJsonArray rows, visible;
    for (const auto &recommendation : pool) {
        const auto &song = recommendation.song;
        rows.append(QJsonObject{{"id", double(song.id)}, {"title", song.title}, {"artist", song.artist}, {"album", song.album},
            {"version", QStringList{"studio", "live"}.contains(song.version) ? song.version : QStringLiteral("unknown")},
            {"durationMs", song.durationMs}});
    }
    for (const auto &recommendation : radioDialog->recommendations()) visible.append(double(recommendation.song.id));
    QJsonObject context{{"candidateSetId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"originMessageId", turnId}, {"originActionId", actionId}, {"rows", rows}, {"visibleIds", visible}};
    if (!LookupContext::valid(context)) { clearRadioLookupContext(); return false; }
    radioLookupContext = context; radioLookupPool = pool;
    radioLookupContextProfile = currentProfile; radioLookupContextDevice = conversationClient->deviceId();
    radioLookupContextConversation = conversationClient->snapshot().value("id").toString(); radioLookupContextLastTurn = turnId;
    // 成功筛选只更新来源，不延长首次曲库查找的十分钟有效期。
    if (initial) radioLookupContextDeadline = QDeadlineTimer(10 * 60 * 1000, Qt::PreciseTimer);
    setProperty("tangoLookupContext", context);
    return true;
}

void MainWindow::refineRadioSong(const QString &turnId, const QString &actionId, const QJsonObject &parameters)
{
    const auto context = radioLookupContextFor(turnId);
    const auto instruction = routedRadioTurns.value(turnId).instruction;
    bool matching = !context.isEmpty() && LookupContext::validParameters(parameters, instruction, context);
    for (const auto &key : QStringList{"candidateSetId", "originMessageId", "originActionId"})
        matching = matching && parameters.value(key) == context.value(key);
    bool known = false;
    const auto ids = matching ? LookupContext::filteredIds(context, parameters, &known) : QJsonArray{};
    if (!matching || !known) {
        if (!setRadioRouteAction(turnId, actionId, "failed")) return;
        appendRadioReply(tr("当前没有可核验的这批候选，或筛选条件仍有歧义。请明确歌名和歌手重新查找；本次没有播放或修改口味。"), turnId);
        return;
    }
    QVector<RadioRecommendation> selected;
    for (const auto &value : ids)
        for (const auto &recommendation : radioLookupPool)
            if (double(recommendation.song.id) == value.toDouble()) { selected.append(recommendation); break; }
    radioRecommendations = selected;
    radioNote = tr("从原来这批候选按本轮条件重新筛选；仅展示信息，没有自动播放或保存口味。");
    radioDialog->setRecommendations(radioTitle, radioNote, selected);
    if (!saveRadioLookupContext(turnId, actionId, radioLookupPool, false)) {
        if (!setRadioRouteAction(turnId, actionId, "failed")) return;
        appendRadioReply(tr("本批候选无法安全续接，请明确歌名和歌手重新查询。"), turnId); return;
    }
    if (!setRadioRouteAction(turnId, actionId, "completed")) return;
    appendRadioReply(selected.isEmpty()
        ? tr("这批候选中没有可确认符合条件的歌曲。版本未知不会当作录音室版；可以换一个条件，或重新搜索。")
        : tr("已从原来这批候选按本轮条件筛出 %1 首。仅展示信息，没有自动播放；其他条件未静默沿用。").arg(selected.size()), turnId);
    continueRadioRoute(turnId);
}
