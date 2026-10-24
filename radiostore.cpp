#include "radiostore.h"
#include "lookupcontext.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QJsonDocument>
#include <QJsonParseError>
#include <cmath>
bool RadioStore::initialize()
{
    if (!database.isValid() || !database.isOpen()) return false;
    QSqlQuery query(database);
    return query.exec("CREATE TABLE IF NOT EXISTS radio_preferences (profile TEXT PRIMARY KEY, "
                      "preference TEXT NOT NULL CHECK(length(preference)<=500))") &&
        query.exec("CREATE TABLE IF NOT EXISTS radio_preference_items (profile TEXT NOT NULL, "
                   "id TEXT NOT NULL, value TEXT NOT NULL, scope TEXT NOT NULL, source TEXT NOT NULL, "
                   "confirmed_at TEXT NOT NULL, revision INTEGER NOT NULL, position INTEGER NOT NULL, "
                   "PRIMARY KEY(profile,id))") &&
        query.exec("CREATE TABLE IF NOT EXISTS radio_preference_versions (profile TEXT PRIMARY KEY, "
                   "revision INTEGER NOT NULL)") &&
        query.exec("CREATE TABLE IF NOT EXISTS radio_route_journal (profile TEXT NOT NULL, "
                   "device_id TEXT NOT NULL, conversation_id TEXT NOT NULL, turn_id TEXT NOT NULL, "
                   "plan_json TEXT NOT NULL, plan_digest TEXT NOT NULL, preference_base TEXT NOT NULL, "
                   "base_revision INTEGER NOT NULL, actions_json TEXT NOT NULL, confirmation_turn TEXT NOT NULL, "
                   "receipts_json TEXT NOT NULL, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, "
                   "expires_at INTEGER NOT NULL, journal_version INTEGER NOT NULL DEFAULT 1, terminal INTEGER NOT NULL DEFAULT 0, "
                   "PRIMARY KEY(profile,device_id,conversation_id,turn_id))");
}
QString RadioStore::preference(const QString &profile)
{
    if (!initialize()) return {};
    QSqlQuery query(database);
    query.prepare("SELECT preference FROM radio_preferences WHERE profile=?");
    query.addBindValue(profile);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}
namespace {
// 调用方持有事务；摘要、条目和版本必须与确认回执同时提交。
bool writePreference(QSqlDatabase database, const QString &profile, const QString &preference, QString *error)
{
    QSqlQuery query(database);
    const auto fail = [&] {
        if (error) *error = QStringLiteral("电台口味保存失败，请稍后重试。");
        return false;
    };
    query.prepare(preference.trimmed().isEmpty()
                      ? "DELETE FROM radio_preferences WHERE profile=?"
                      : "INSERT OR REPLACE INTO radio_preferences(profile,preference) VALUES(?,?)");
    query.addBindValue(profile);
    if (!preference.trimmed().isEmpty()) query.addBindValue(preference.trimmed());
    if (!query.exec()) return fail();
    query.prepare("INSERT INTO radio_preference_versions(profile,revision) VALUES(?,1) "
                  "ON CONFLICT(profile) DO UPDATE SET revision=revision+1");
    query.addBindValue(profile);
    if (!query.exec()) return fail();
    query.prepare("SELECT revision FROM radio_preference_versions WHERE profile=?");
    query.addBindValue(profile);
    if (!query.exec() || !query.next()) return fail();
    const int revision = query.value(0).toInt();
    query.finish();
    QSet<QString> ids;
    int position = 0;
    for (const auto &part : preference.split(QRegularExpression(QStringLiteral("[；;\\n]+")), Qt::SkipEmptyParts)) {
        const QString value = part.trimmed();
        if (value.isEmpty()) continue;
        const QString id = QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
        if (ids.contains(id)) continue;
        ids.insert(id);
        query.prepare("INSERT INTO radio_preference_items(profile,id,value,scope,source,confirmed_at,revision,position) "
                      "VALUES(?,?,?,'global','user_confirmed_editor',?,?,?) ON CONFLICT(profile,id) "
                      "DO UPDATE SET revision=excluded.revision,position=excluded.position");
        query.addBindValue(profile); query.addBindValue(id); query.addBindValue(value);
        query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        query.addBindValue(revision); query.addBindValue(position++);
        if (!query.exec()) return fail();
    }
    query.prepare("DELETE FROM radio_preference_items WHERE profile=? AND revision<>?");
    query.addBindValue(profile); query.addBindValue(revision);
    if (!query.exec()) return fail();
    return true;
}
}

bool RadioStore::clearConversationRoutes(const QString &profile, QString *error)
{
    // 清除账号下所有本机待确认路由；偏好、播放队列和采用事实账本不在删除范围。
    if (initialize() && !profile.isEmpty()) {
        QSqlQuery query(database);
        query.prepare("DELETE FROM radio_route_journal WHERE profile=?");
        query.addBindValue(profile);
        if (query.exec()) return true;
    }
    if (error) *error = QStringLiteral("后台对话已清除，但本机待确认记录清理失败；请再次清除核对。");
    return false;
}

bool RadioStore::save(const QString &profile, const QString &preference, QString *error)
{
    if (error) error->clear();
    if (profile.isEmpty() || preference.size() > 500 || !initialize()) {
        if (error) *error = QStringLiteral("暂时无法保存电台口味，请检查本机数据空间。");
        return false;
    }
    if (!database.transaction()) {
        if (error) *error = QStringLiteral("口味正在保存，请稍后重试。");
        return false;
    }
    if (!writePreference(database, profile, preference, error) || !database.commit()) {
        database.rollback();
        if (error && error->isEmpty()) *error = QStringLiteral("电台口味保存失败，请稍后重试。");
        return false;
    }
    return true;
}

QJsonArray RadioStore::items(const QString &profile)
{
    QJsonArray result;
    if (!initialize()) return result;
    QSqlQuery query(database);
    query.prepare("SELECT id,value,scope,source,confirmed_at,revision FROM radio_preference_items "
                  "WHERE profile=? ORDER BY position,id");
    query.addBindValue(profile);
    if (!query.exec()) return result;
    while (query.next()) result.append(QJsonObject{{"id", query.value(0).toString()},
        {"value", query.value(1).toString()}, {"scope", query.value(2).toString()},
        {"source", query.value(3).toString()}, {"confirmedAt", query.value(4).toString()},
        {"revision", query.value(5).toInt()}});
    return result;
}

namespace {
constexpr qint64 journalLifetimeMs = 7LL * 24 * 60 * 60 * 1000;
const QString journalFailure = QStringLiteral("本机待确认记录无效或无法保存，未继续执行；请检查数据空间后重试。");
const QSet<QString> journalStates{"pending", "awaiting_confirmation", "dispatched", "unknown", "completed", "cancelled", "failed", "skipped"};
bool failJournal(QString *error, const QString &message = journalFailure)
{
    if (error) *error = message;
    return false;
}
bool journalId(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return pattern.match(value).hasMatch();
}
bool journalScope(const QString &profile, const QString &device, const QString &conversation, const QString &turn = {})
{
    return !profile.isEmpty() && profile.size() <= 512 && journalId(device) && journalId(conversation) &&
        (turn.isEmpty() || journalId(turn));
}
QString json(const QJsonObject &value) { return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)); }
QString digest(const QString &value) { return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex()); }
bool privatePayload(const QString &value)
{
    // 日志不承接凭据或播放地址；合法音乐提议也要先通过这条本机保存边界。
    static const QRegularExpression secret(QStringLiteral("https?://|wss?://|lsv2_|sk-[A-Za-z0-9]{16,}|Bearer\\s+|(?:api[_-]?key|cookie|authorization)\\s*[:=]"), QRegularExpression::CaseInsensitiveOption);
    return secret.match(value).hasMatch();
}
bool positiveRevision(const QJsonValue &value)
{
    const auto number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= 1 && number <= 2147483647. && std::floor(number) == number;
}
bool journalPlan(const QJsonObject &plan, const QString &turn)
{
    if (plan.size() != 8 || plan.value("messageId").toString() != turn || plan.value("version").toDouble(-1) != 1 ||
        !QStringList{"deterministic", "model"}.contains(plan.value("planner").toString()) ||
        !QStringList{"direct", "single_agent", "clarify", "reject"}.contains(plan.value("strategy").toString()) ||
        !plan.value("intents").isArray() || plan.value("intents").toArray().isEmpty() || plan.value("intents").toArray().size() > 12 ||
        !plan.value("slots").isObject() || !plan.value("missingSlots").isArray() || plan.value("missingSlots").toArray().size() > 4 ||
        !plan.value("actions").isArray() || json(plan).toUtf8().size() > 32768 || privatePayload(json(plan))) return false;
    const QSet<QString> keys{"version", "messageId", "planner", "strategy", "intents", "actions", "missingSlots", "slots"};
    for (auto it = plan.begin(); it != plan.end(); ++it) if (!keys.contains(it.key())) return false;
    const auto actions = plan.value("actions").toArray();
    if (actions.isEmpty() || actions.size() > 4) return false;
    QHash<QString, QJsonObject> indexed;
    for (const auto &entry : actions) {
        const auto action = entry.toObject();
        const auto id = action.value("id").toString();
        const auto capability = action.value("capability").toString();
        const auto parameters = action.value("parameters").toObject();
        if (!entry.isObject() || action.size() != 5 || !journalId(id) || indexed.contains(id) ||
            !action.value("source").isString() || action.value("source").toString().trimmed().isEmpty() ||
            action.value("source").toString().size() > 1000 || !action.value("parameters").isObject() ||
            !action.value("dependsOn").isArray() || action.value("dependsOn").toArray().size() > 3) return false;
        if (capability == "memory.propose") {
            const auto operation = parameters.value("operation").toString();
            if (parameters.size() != 2 || !parameters.value("preference").isString() ||
                parameters.value("preference").toString().size() > 500 ||
                (operation != "set" && operation != "clear") ||
                (operation == "set" && parameters.value("preference").toString().trimmed().isEmpty()) ||
                (operation == "clear" && !parameters.value("preference").toString().isEmpty())) return false;
        } else if (capability == "music.task") {
            const auto mode = parameters.value("mode").toString();
            if ((mode != "new" && mode != "continue") ||
                (mode == "new" && parameters.size() != 1) ||
                (mode == "continue" && (parameters.size() != 3 || !journalId(parameters.value("taskId").toString()) ||
                    !positiveRevision(parameters.value("expectedRevision"))))) return false;
        } else if (capability == "task.control") {
            if (parameters.size() != 3 || !QStringList{"pause", "cancel", "resume", "undo"}.contains(parameters.value("action").toString()) ||
                !journalId(parameters.value("taskId").toString()) || !positiveRevision(parameters.value("expectedRevision"))) return false;
        } else if (capability == "music.artist_catalog" || capability == "music.playlist_search") {
            const bool catalog = capability == "music.artist_catalog";
            const auto field = catalog ? QStringLiteral("artist") : QStringLiteral("keyword");
            const auto value = parameters.value(field).toString();
            if (actions.size() != 1 || parameters.size() != 2 || !parameters.value(field).isString() ||
                value.trimmed().isEmpty() || value.size() > (catalog ? 100 : 200) ||
                !action.value("source").toString().contains(value) ||
                !parameters.value("limit").isDouble() || parameters.value("limit").toDouble() < 1 ||
                parameters.value("limit").toDouble() > (catalog ? 30 : 5) ||
                parameters.value("limit").toDouble() != parameters.value("limit").toInt() ||
                !action.value("dependsOn").toArray().isEmpty() ||
                plan.value("intents").toArray() != QJsonArray{catalog ? "artist_catalog" : "playlist_search"}) return false;
        } else if (capability == "music.lookup") {
            if ((parameters.size() != 3 && parameters.size() != 4) ||
                (parameters.size() == 4 && (!parameters.value("version").isString() ||
                    !QStringList{"studio", "live"}.contains(parameters.value("version").toString()))) ||
                !parameters.value("artist").isString() || parameters.value("artist").toString().size() > 100 ||
                !parameters.value("title").isString() || parameters.value("title").toString().trimmed().isEmpty() ||
                parameters.value("title").toString().size() > 200 || !parameters.value("play").isBool()) return false;
        } else if (capability == "music.refine") {
            QJsonObject context;
            if (parameters.value("operation").toString() == "artist")
                context.insert("rows", QJsonArray{QJsonObject{{"artist", parameters.value("artist")}}});
            if (actions.size() != 1 || plan.value("intents").toArray() != QJsonArray{"song_lookup"} ||
                !action.value("dependsOn").toArray().isEmpty() ||
                !LookupContext::validParameters(parameters, action.value("source").toString(), context)) return false;
        } else if (capability == "assistant.reply") {
            if ((parameters.size() != 2 && parameters.size() != 3) ||
                !QStringList{"help", "explain", "clarify", "unsupported", "history"}.contains(parameters.value("kind").toString()) ||
                !parameters.value("text").isString() || parameters.value("text").toString().trimmed().isEmpty() ||
                parameters.value("text").toString().size() > 1600) return false;
        } else return false;
        indexed.insert(id, action);
    }
    QSet<QString> complete;
    for (int pass = 0; pass < indexed.size(); ++pass) {
        for (auto it = indexed.cbegin(); it != indexed.cend(); ++it) {
            bool ready = true; QSet<QString> seen;
            for (const auto &dependency : it.value().value("dependsOn").toArray()) {
                const auto id = dependency.toString();
                if (!dependency.isString() || id == it.key() || !indexed.contains(id) || seen.contains(id)) return false;
                seen.insert(id); if (!complete.contains(id)) ready = false;
            }
            if (ready) complete.insert(it.key());
        }
    }
    return complete.size() == indexed.size();
}
bool terminalActions(const QJsonObject &states)
{
    for (const auto &entry : states) if (QStringList{"pending", "awaiting_confirmation", "dispatched", "unknown"}.contains(entry.toString())) return false;
    return true;
}
struct JournalRow {
    QJsonObject plan, actions, receipts;
    QString turn, preferenceBase, confirmationTurn;
    qint64 baseRevision = -1, createdAt = 0, expiresAt = 0, version = 0;
};
bool parseObject(const QString &text, QJsonObject *value)
{
    if (text.toUtf8().size() > 32768) return false;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    *value = doc.object(); return true;
}
void bindScope(QSqlQuery &query, const QString &profile, const QString &device, const QString &conversation, const QString &turn = {})
{
    query.addBindValue(profile); query.addBindValue(device); query.addBindValue(conversation);
    if (!turn.isEmpty()) query.addBindValue(turn);
}
bool readJournal(QSqlQuery &query, JournalRow *row)
{
    row->turn = query.value("turn_id").toString();
    row->preferenceBase = query.value("preference_base").toString();
    row->baseRevision = query.value("base_revision").toLongLong();
    row->confirmationTurn = query.value("confirmation_turn").toString();
    row->createdAt = query.value("created_at").toLongLong();
    row->expiresAt = query.value("expires_at").toLongLong();
    row->version = query.value("journal_version").toLongLong();
    if (!journalId(row->turn) || row->preferenceBase.size() > 500 || privatePayload(row->preferenceBase) ||
        row->baseRevision < 0 || row->version < 1 || row->createdAt <= 0 || row->expiresAt <= row->createdAt ||
        row->expiresAt - row->createdAt > journalLifetimeMs || row->expiresAt <= QDateTime::currentMSecsSinceEpoch() ||
        (!row->confirmationTurn.isEmpty() && !journalId(row->confirmationTurn)) ||
        !parseObject(query.value("plan_json").toString(), &row->plan) || !journalPlan(row->plan, row->turn) ||
        digest(json(row->plan)) != query.value("plan_digest").toString() ||
        !parseObject(query.value("actions_json").toString(), &row->actions) ||
        !parseObject(query.value("receipts_json").toString(), &row->receipts) ||
        row->actions.size() != row->plan.value("actions").toArray().size()) return false;
    QSet<QString> ids;
    for (const auto &entry : row->plan.value("actions").toArray()) ids.insert(entry.toObject().value("id").toString());
    for (auto it = row->actions.begin(); it != row->actions.end(); ++it)
        if (!ids.contains(it.key()) || !it.value().isString() || !journalStates.contains(it.value().toString())) return false;
    for (auto it = row->receipts.begin(); it != row->receipts.end(); ++it) {
        const auto receipt = it.value().toObject();
        if (!ids.contains(it.key()) || !it.value().isObject() || receipt.value("text").toString().isEmpty() ||
            receipt.size() != 5 || !receipt.value("confirmed").isBool() || privatePayload(json(receipt)) ||
            !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(receipt.value("preferenceDigest").toString()).hasMatch() ||
            !receipt.value("preferenceRevision").isDouble() || receipt.value("preferenceRevision").toDouble(-1) < 0 ||
            !receipt.value("committedAt").isDouble() || receipt.value("committedAt").toDouble(-1) < row->createdAt ||
            receipt.value("committedAt").toDouble(-1) >= row->expiresAt ||
            row->actions.value(it.key()).toString() != (receipt.value("confirmed").toBool() ? "completed" : "cancelled")) return false;
        const auto text = receipt.value("text").toString();
        if ((!receipt.value("confirmed").toBool() && text != QStringLiteral("已取消这次记忆提议，未改变长期口味。")) ||
            (receipt.value("confirmed").toBool() && text != QStringLiteral("已确认清除当前账号的本机长期口味。") &&
                text != QStringLiteral("已确认保存当前账号的本机长期口味。"))) return false;
    }
    for (const auto &entry : row->plan.value("actions").toArray()) {
        const auto action = entry.toObject(); const auto id = action.value("id").toString();
        if (row->receipts.contains(id) && action.value("capability").toString() != "memory.propose") return false;
        if (action.value("capability").toString() == "memory.propose" &&
            QStringList{"completed", "cancelled"}.contains(row->actions.value(id).toString()) && !row->receipts.contains(id)) return false;
    }
    return true;
}
bool loadJournal(QSqlDatabase database, const QString &profile, const QString &device, const QString &conversation,
                 const QString &turn, JournalRow *row, QString *error)
{
    QSqlQuery query(database);
    query.prepare("SELECT * FROM radio_route_journal WHERE profile=? AND device_id=? AND conversation_id=? AND turn_id=?");
    bindScope(query, profile, device, conversation, turn);
    if (!query.exec() || !query.next() || !readJournal(query, row)) return failJournal(error);
    return true;
}
bool updateJournal(QSqlDatabase database, const QString &profile, const QString &device, const QString &conversation,
                   const JournalRow &row, QString *error)
{
    QSqlQuery query(database);
    query.prepare("UPDATE radio_route_journal SET actions_json=?, confirmation_turn=?, receipts_json=?, updated_at=?, "
                  "journal_version=journal_version+1,terminal=? WHERE profile=? AND device_id=? AND conversation_id=? AND turn_id=? AND journal_version=?");
    query.addBindValue(json(row.actions)); query.addBindValue(row.confirmationTurn); query.addBindValue(json(row.receipts));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch()); query.addBindValue(terminalActions(row.actions) ? 1 : 0);
    bindScope(query, profile, device, conversation, row.turn); query.addBindValue(row.version);
    if (!query.exec() || query.numRowsAffected() != 1) return failJournal(error);
    return true;
}
bool readPreference(QSqlDatabase database, const QString &profile, QJsonObject *snapshot)
{
    QSqlQuery query(database);
    query.prepare("SELECT (SELECT preference FROM radio_preferences WHERE profile=?), "
                  "COALESCE((SELECT revision FROM radio_preference_versions WHERE profile=?),0)");
    query.addBindValue(profile); query.addBindValue(profile);
    if (!query.exec() || !query.next() || query.value(1).toLongLong() < 0) return false;
    *snapshot = {{"preference", query.value(0).toString()}, {"revision", double(query.value(1).toLongLong())}};
    return true;
}
}

QJsonObject RadioStore::preferenceSnapshot(const QString &profile, QString *error)
{
    if (error) error->clear();
    QJsonObject result;
    if (profile.isEmpty() || profile.size() > 512 || !initialize() || !readPreference(database, profile, &result)) {
        failJournal(error); return {};
    }
    return result;
}

bool RadioStore::journalRoute(const QString &profile, const QString &device, const QString &conversation,
                             const QString &turn, const QJsonObject &plan, const QString &preferenceBase,
                             qint64 baseRevision, QString *error)
{
    if (error) error->clear();
    if (!journalScope(profile, device, conversation, turn) || !journalId(turn) || !journalPlan(plan, turn) ||
        preferenceBase.size() > 500 || privatePayload(preferenceBase) || baseRevision < 0 || !initialize() || !database.transaction()) return failJournal(error);
    const auto rollback = [&] { database.rollback(); return failJournal(error); };
    QSqlQuery query(database);
    query.prepare("SELECT * FROM radio_route_journal WHERE profile=? AND device_id=? AND conversation_id=? AND turn_id=?");
    bindScope(query, profile, device, conversation, turn);
    if (!query.exec()) return rollback();
    if (query.next()) {
        JournalRow existing;
        if (!readJournal(query, &existing) || existing.plan != plan || existing.preferenceBase != preferenceBase || existing.baseRevision != baseRevision) return rollback();
        query.finish();
        if (!database.commit()) return rollback();
        return true;
    }
    query.finish();
    QJsonObject snapshot;
    if (!readPreference(database, profile, &snapshot) || snapshot.value("preference").toString() != preferenceBase ||
        qint64(snapshot.value("revision").toDouble(-1)) != baseRevision) return rollback();
    query.prepare("SELECT COUNT(*) FROM radio_route_journal WHERE profile=? AND device_id=? AND conversation_id=? AND terminal=0 AND expires_at>?");
    bindScope(query, profile, device, conversation); query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec() || !query.next() || query.value(0).toInt() >= 32) return rollback();
    query.finish();
    QJsonObject states;
    for (const auto &entry : plan.value("actions").toArray()) states.insert(entry.toObject().value("id").toString(), "pending");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    query.prepare("INSERT INTO radio_route_journal(profile,device_id,conversation_id,turn_id,plan_json,plan_digest,preference_base,base_revision,"
                  "actions_json,confirmation_turn,receipts_json,created_at,updated_at,expires_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    bindScope(query, profile, device, conversation, turn);
    query.addBindValue(json(plan)); query.addBindValue(digest(json(plan))); query.addBindValue(preferenceBase);
    query.addBindValue(baseRevision); query.addBindValue(json(states)); query.addBindValue(QStringLiteral(""));
    query.addBindValue(QStringLiteral("{}")); query.addBindValue(now); query.addBindValue(now); query.addBindValue(now + journalLifetimeMs);
    if (!query.exec() || !database.commit()) return rollback();
    return true;
}

QJsonArray RadioStore::routes(const QString &profile, const QString &device, const QString &conversation, QString *error)
{
    if (error) error->clear();
    if (!journalScope(profile, device, conversation) || !initialize() || !database.transaction()) { failJournal(error); return {}; }
    const auto rollback = [&]() -> QJsonArray { database.rollback(); failJournal(error); return {}; };
    QSqlQuery query(database);
    query.prepare("SELECT * FROM radio_route_journal WHERE profile=? AND device_id=? AND conversation_id=? AND expires_at>? "
                  "ORDER BY terminal ASC,created_at DESC,turn_id ASC LIMIT 32");
    bindScope(query, profile, device, conversation); query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) return rollback();
    QList<JournalRow> rows;
    while (query.next()) {
        JournalRow row; if (!readJournal(query, &row)) return rollback(); rows.append(row);
    }
    query.finish();
    QJsonArray result;
    for (auto row : rows) {
        bool changed = false;
        // 断点落在发出与收到回执之间时结果未知；只能查询原请求，不允许据本机日志重发。
        for (auto it = row.actions.begin(); it != row.actions.end(); ++it)
            if (it.value().toString() == "dispatched") { it.value() = "unknown"; changed = true; }
        if (changed && !updateJournal(database, profile, device, conversation, row, error)) return rollback();
        result.append(QJsonObject{{"turnId", row.turn}, {"plan", row.plan}, {"preferenceBase", row.preferenceBase},
            {"baseRevision", double(row.baseRevision)}, {"actions", row.actions}, {"confirmationTurn", row.confirmationTurn},
            {"receipts", row.receipts}, {"expiresAt", double(row.expiresAt)}});
    }
    if (!database.commit()) return rollback();
    return result;
}

bool RadioStore::setRouteAction(const QString &profile, const QString &device, const QString &conversation,
                               const QString &turn, const QString &action, const QString &expected,
                               const QString &next, QString *error)
{
    if (error) error->clear();
    const QHash<QString, QSet<QString>> transitions{
        {"pending", {"awaiting_confirmation", "dispatched", "completed", "failed", "skipped", "cancelled"}},
        {"awaiting_confirmation", {"failed", "skipped"}},
        {"dispatched", {"completed", "failed", "unknown"}},
        {"unknown", {"completed", "failed", "skipped", "cancelled"}}};
    if (!journalScope(profile, device, conversation, turn) || !journalId(turn) || !journalId(action) ||
        !transitions.value(expected).contains(next) || !initialize() || !database.transaction()) return failJournal(error);
    const auto rollback = [&] { database.rollback(); return failJournal(error); };
    JournalRow row;
    if (!loadJournal(database, profile, device, conversation, turn, &row, error) || row.actions.value(action).toString() != expected) return rollback();
    for (const auto &entry : row.plan.value("actions").toArray()) {
        const auto planned = entry.toObject();
        if (planned.value("id").toString() != action) continue;
        if (expected == "pending" && QStringList{"awaiting_confirmation", "dispatched", "completed"}.contains(next)) {
            for (const auto &dependency : planned.value("dependsOn").toArray())
                if (row.actions.value(dependency.toString()).toString() != "completed") return rollback();
        }
        if (planned.value("capability").toString() == "memory.propose" &&
            (next == "completed" || next == "dispatched" || next == "cancelled")) return rollback();
    }
    row.actions.insert(action, next);
    if (!updateJournal(database, profile, device, conversation, row, error) || !database.commit()) return rollback();
    return true;
}

bool RadioStore::setRouteConfirmationTurn(const QString &profile, const QString &device, const QString &conversation,
                                         const QString &turn, const QString &confirmation, QString *error)
{
    if (error) error->clear();
    if (!journalScope(profile, device, conversation, turn) || !journalId(turn) || !journalId(confirmation) ||
        !initialize() || !database.transaction()) return failJournal(error);
    const auto rollback = [&] { database.rollback(); return failJournal(error); };
    JournalRow row;
    if (!loadJournal(database, profile, device, conversation, turn, &row, error)) return rollback();
    row.confirmationTurn = confirmation;
    if (!updateJournal(database, profile, device, conversation, row, error) || !database.commit()) return rollback();
    return true;
}

bool RadioStore::decidePreference(const QString &profile, const QString &device, const QString &conversation,
                                 const QString &turn, const QString &action, bool confirmed, const QString &edited,
                                 QJsonObject *receipt, QString *error)
{
    if (error) error->clear();
    if (receipt) *receipt = {};
    if (!journalScope(profile, device, conversation, turn) || !journalId(turn) || !journalId(action) ||
        edited.size() > 500 || privatePayload(edited) || !initialize() || !database.transaction()) return failJournal(error);
    const auto rollback = [&] { database.rollback(); return failJournal(error); };
    JournalRow row;
    if (!loadJournal(database, profile, device, conversation, turn, &row, error)) return rollback();
    QJsonObject parameters;
    for (const auto &entry : row.plan.value("actions").toArray()) {
        const auto planned = entry.toObject();
        if (planned.value("id").toString() == action && planned.value("capability").toString() == "memory.propose")
            parameters = planned.value("parameters").toObject();
    }
    if (parameters.isEmpty()) return rollback();
    const auto summary = parameters.value("operation").toString() == "clear" ? QString() : edited.trimmed();
    const auto previous = row.receipts.value(action).toObject();
    if (!previous.isEmpty()) {
        if (previous.value("confirmed").toBool() != confirmed ||
            (confirmed && previous.value("preferenceDigest").toString() != digest(summary)) ||
            row.actions.value(action).toString() != (confirmed ? "completed" : "cancelled")) return rollback();
        if (!database.commit()) return rollback();
        if (receipt) *receipt = previous;
        return true;
    }
    if (row.actions.value(action).toString() != "awaiting_confirmation" || row.confirmationTurn.isEmpty() ||
        (confirmed && parameters.value("operation").toString() != "clear" && summary.isEmpty())) return rollback();
    QJsonObject snapshot;
    if (!readPreference(database, profile, &snapshot)) return rollback();
    if (confirmed && (snapshot.value("preference").toString() != row.preferenceBase ||
        qint64(snapshot.value("revision").toDouble(-1)) != row.baseRevision)) {
        database.rollback();
        return failJournal(error, QStringLiteral("长期口味在这条提议之后已被更新，本次未用旧摘要覆盖它。请基于当前口味重新提出或编辑。"));
    }
    if (confirmed && !writePreference(database, profile, summary, error)) return rollback();
    if (confirmed && !readPreference(database, profile, &snapshot)) return rollback();
    const QString text = confirmed ? (summary.isEmpty() ? QStringLiteral("已确认清除当前账号的本机长期口味。") :
        QStringLiteral("已确认保存当前账号的本机长期口味。")) : QStringLiteral("已取消这次记忆提议，未改变长期口味。");
    QJsonObject saved{{"confirmed", confirmed}, {"text", text}, {"preferenceDigest", digest(summary)},
        {"preferenceRevision", snapshot.value("revision")}, {"committedAt", double(QDateTime::currentMSecsSinceEpoch())}};
    row.actions.insert(action, confirmed ? "completed" : "cancelled"); row.receipts.insert(action, saved);
    if (!updateJournal(database, profile, device, conversation, row, error) || !database.commit()) return rollback();
    if (receipt) *receipt = saved;
    return true;
}
