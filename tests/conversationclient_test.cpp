#include "appbackendclient.h"
#include "conversationclient.h"
#include "musictaskclient.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <functional>
#include <memory>

namespace {
struct Request {
    QByteArray method;
    QString path;
    QJsonObject body;
};
struct Response { QJsonObject body; int status = 200; int delay = 0; bool hang = false; };
class FakeServer : public QObject {
public:
    QTcpServer server;
    QVector<Request> requests;
    std::function<Response(const Request &)> handler;
    FakeServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                auto bytes = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes, handled] {
                    bytes->append(socket->readAll());
                    if (*handled) return;
                    const auto end = bytes->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const auto header = bytes->left(end);
                    int length = 0;
                    for (const auto &line : header.split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    if (bytes->size() < end + 4 + length) return;
                    *handled = true;
                    Request request{header.split(' ').value(0), QUrl::fromEncoded(header.split(' ').value(1)).path(),
                        QJsonDocument::fromJson(bytes->mid(end + 4, length)).object()};
                    requests.append(request);
                    const auto response = handler(request);
                    if (response.hang) return;
                    QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(response.delay, this, [guard, response] {
                        if (!guard || guard->state() != QAbstractSocket::ConnectedState) return;
                        const auto body = QJsonDocument(response.body).toJson(QJsonDocument::Compact);
                        guard->write("HTTP/1.1 " + QByteArray::number(response.status) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        guard->disconnectFromHost();
                    });
                });
            }
        });
    }
    QByteArray url() const { return "http://127.0.0.1:" + QByteArray::number(server.serverPort()); }
    int count(const QByteArray &method, const QString &path) const {
        int count = 0; for (const auto &r : requests) if (r.method == method && r.path == path) ++count; return count;
    }
};
struct Fixture {
    QTemporaryDir directory;
    QByteArray previousUrl;
    bool hadUrl;
    QByteArray previousDirectory;
    bool hadDirectory;
    FakeServer server;
    AppBackendClient backend{nullptr, 90};
    ConversationClient *client = nullptr;
    QJsonObject current;
    QHash<QString, QJsonObject> receipts;
    QSet<QString> events;
    QStringList createIds;
    QStringList postedIds;
    int messageDelay = 0;
    bool hangMessage = false, dropMessage = false, hangCreate = false, hangEvent = false;
    bool invalidHint = false;
    int rejectMessage = 0;
    int latestDelay = 0;
    QJsonObject eventOverride;
    Fixture() : previousUrl(qgetenv("MUSICPLAYER_BACKEND_URL")), hadUrl(qEnvironmentVariableIsSet("MUSICPLAYER_BACKEND_URL")),
        previousDirectory(qgetenv("MUSICPLAYER_DATA_DIR")), hadDirectory(qEnvironmentVariableIsSet("MUSICPLAYER_DATA_DIR")) {
        Q_ASSERT(directory.isValid());
        qputenv("MUSICPLAYER_DATA_DIR", directory.path().toUtf8());
        qputenv("MUSICPLAYER_BACKEND_URL", server.url());
        server.handler = [this](const Request &r) -> Response {
            if (r.path == "/v1/auth/login") {
                const auto name = r.body.value("username").toString();
                return {{{"token", "test-session-token-123456789012345"},
                    {"user", QJsonObject{{"id", "account-"+name}, {"username", name}}},
                    {"expiresAt", QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODate)}}};
            }
            if (r.path == "/v1/auth/logout") return {{{"ok", true}}};
            if (r.path == "/v1/agent/status")
                return {{{"configured", true}, {"model", "fixture"}, {"taskRuntime", true}, {"taskVersion", 1}, {"conversationVersion", 1}}};
            if (r.path == "/v1/conversations" && r.method == "POST") {
                createIds.append(r.body.value("requestId").toString());
                if (current.isEmpty()) current = {{"id", "conversation-1"}, {"deviceId", r.body.value("deviceId")},
                    {"version", 0}, {"messages", QJsonArray()}, {"hasEarlier", false}, {"earliestSequence", 0}};
                return {{{"conversation", current}}, 200, 0, hangCreate};
            }
            if (r.path.startsWith("/v1/conversations/latest/"))
                return {{{"conversation", current.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(current)}}, 200, latestDelay};
            if (r.path == "/v1/conversations/conversation-1/messages" && r.method == "POST") {
                const auto id = r.body.value("messageId").toString(); postedIds.append(id);
                if (rejectMessage) return {{{"error", QJsonObject{{"message", "fixture rejection"}}}}, rejectMessage};
                if (!dropMessage && !receipts.contains(id)) {
                    const auto number = current.value("version").toInt() + 1;
                    auto messages = current.value("messages").toArray();
                    messages.append(QJsonObject{{"id", id}, {"turnId", id}, {"sequence", number}, {"role", "user"},
                        {"content", r.body.value("content")}, {"source", "user"}, {"createdAt", "2026-10-20T10:00:00.000Z"}});
                    current.insert("messages", messages); current.insert("version", number); current.insert("earliestSequence", 1);
                    receipts.insert(id, {{"messageId", id}, {"turnId", id}, {"status", "accepted"}, {"sequence", number},
                        {"dispatchHint", invalidHint ? "unrecognized" : "task"}, {"conversationVersion", number}});
                }
                return {{{"conversation", current}, {"receipt", receipts.value(id)}}, 200, messageDelay, hangMessage};
            }
            if (r.path.startsWith("/v1/conversations/conversation-1/messages/") && r.method == "GET") {
                const auto id = r.path.section('/', -1);
                if (!receipts.contains(id)) return {{{"error", QJsonObject{{"message", "not found"}}}}, 404};
                return {{{"conversation", current}, {"receipt", receipts.value(id)}}};
            }
            if (r.path == "/v1/conversations/conversation-1/events") {
                const auto id = r.body.value("eventId").toString();
                if (!events.contains(id)) {
                    events.insert(id);
                    auto messages = current.value("messages").toArray();
                    const auto number = current.value("version").toInt() + 1;
                    messages.append(QJsonObject{{"id", id}, {"turnId", r.body.value("turnId")}, {"sequence", number},
                        {"role", "assistant"}, {"source", "device_report"},
                        {"content", r.body.value("content").toString("task fixture reply")}, {"createdAt", "2026-10-20T10:00:01.000Z"}});
                    current.insert("messages", messages); current.insert("version", number);
                }
                const bool hang = hangEvent; hangEvent = false;
                return {{{"conversation", eventOverride.isEmpty() ? current : eventOverride},
                    {"receipt", QJsonObject{{"eventId", id}, {"status", "recorded"}}}}, 200, 0, hang};
            }
            return {{{"error", QJsonObject{{"message", "unknown fixture route"}}}}, 404};
        };
        client = new ConversationClient(nullptr, &backend);
    }
    ~Fixture() {
        delete client;
        if (hadUrl) qputenv("MUSICPLAYER_BACKEND_URL", previousUrl); else qunsetenv("MUSICPLAYER_BACKEND_URL");
        if (hadDirectory) qputenv("MUSICPLAYER_DATA_DIR", previousDirectory); else qunsetenv("MUSICPLAYER_DATA_DIR");
    }
    bool login(const QString &name = "alice") {
        bool done = false; QString error;
        backend.login(name, "fixture-password", &backend, [&](const QString &value) { error = value; done = true; });
        if (!QTest::qWaitFor([&] { return done; }, 1000)) return false;
        if (!done || !error.isEmpty()) return false;
        done = false;
        backend.refreshAgentStatus(&backend, [&](const QString &value) { error = value; done = true; });
        if (!QTest::qWaitFor([&] { return done; }, 1000)) return false;
        return done && error.isEmpty();
    }
    QString messagesPath() const { return QStringLiteral("/v1/conversations/conversation-1/messages"); }
};
}

class ConversationClientTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QCoreApplication::setOrganizationName("TangoFixture"); QCoreApplication::setApplicationName("ConversationTest"); }
    void durableAckExecutesOnlyOnce() {
        Fixture f; QVERIFY(f.login()); QVERIFY(f.client->available());
        f.messageDelay = 30;
        QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        f.client->submit("喝酒听的歌"); f.client->submit("喝酒听的歌");
        QCOMPARE(accepted.count(), 0);
        QTRY_COMPARE(accepted.count(), 1);
        QCOMPARE(f.server.count("POST", f.messagesPath()), 1);
        QCOMPARE(accepted.at(0).at(1).toString(), QString("喝酒听的歌"));
        QCOMPARE(accepted.at(0).at(2).toString(), QString("task"));
        QVERIFY(!f.client->busy()); QVERIFY(!f.client->snapshot().contains("pendingMessageId"));
    }
    void deviceIdentityMatchesMusicTaskAndIsConfinedToFixture() {
        Fixture f;
        MusicTaskClient tasks(nullptr, &f.backend);
        QCOMPARE(f.client->deviceId(), tasks.deviceId());
        const auto path = f.directory.filePath("music-task-device.ini");
        QVERIFY(QFile::exists(path));
        QSettings settings(path, QSettings::IniFormat);
        QCOMPARE(settings.value("musicTasks/deviceId").toString(), f.client->deviceId());
        ConversationClient another(nullptr, &f.backend);
        QCOMPARE(another.deviceId(), f.client->deviceId());
    }
    void timeoutReadsCommittedReceiptWithoutSecondPost() {
        Fixture f; QVERIFY(f.login()); f.hangMessage = true;
        QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        QSignalSpy failed(f.client, &ConversationClient::submissionFailed);
        f.client->submit("安静一点");
        QTRY_COMPARE(accepted.count(), 1);
        QCOMPARE(failed.count(), 0); QCOMPARE(f.postedIds.size(), 1);
        QCOMPARE(f.server.count("GET", f.messagesPath()+"/"+f.postedIds.first()), 1);
    }
    void unknownSubmissionKeepsDraftAndExplicitRetryUsesSameId() {
        Fixture f; QVERIFY(f.login()); f.hangMessage = true; f.dropMessage = true;
        QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        QSignalSpy failed(f.client, &ConversationClient::submissionFailed);
        f.client->submit("不要人声");
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(accepted.count(), 0); QCOMPARE(f.postedIds.size(), 1);
        const auto id = f.client->snapshot().value("pendingMessageId").toString(); QVERIFY(!id.isEmpty());
        QCOMPARE(f.client->snapshot().value("pendingInstruction").toString(), QString("不要人声"));
        f.client->submit("别的消息"); QCOMPARE(failed.count(), 2); QCOMPARE(f.postedIds.size(), 1);
        f.hangMessage = false; f.dropMessage = false; f.client->submit("不要人声");
        QTRY_COMPARE(accepted.count(), 1); QCOMPARE(f.postedIds.size(), 2); QCOMPARE(f.postedIds.last(), id);
    }
    void definiteRejectionAllowsEditing() {
        Fixture f; QVERIFY(f.login()); f.rejectMessage = 400;
        QSignalSpy failed(f.client, &ConversationClient::submissionFailed);
        QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        f.client->submit("错误需求"); QTRY_COMPARE(failed.count(), 1);
        QVERIFY(!f.client->snapshot().contains("pendingMessageId"));
        f.rejectMessage = 0; f.client->submit("新的需求"); QTRY_COMPARE(accepted.count(), 1);
        QVERIFY(f.postedIds.first() != f.postedIds.last());
    }
    void restoreDoesNotExecuteAndDoesNotLoseLocalReplies() {
        Fixture f; QVERIFY(f.login());
        QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        f.client->submit("记住我喜欢爵士"); QTRY_COMPARE(accepted.count(), 1);
        const auto turn = accepted.at(0).at(0).toString();
        f.client->appendLocalReply(turn, "请确认偏好");
        QTRY_COMPARE(f.client->snapshot().value("messages").toArray().size(), 2);
        f.client->detach(); f.client->restoreLatest();
        QTRY_VERIFY(!f.client->busy());
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(f.client->snapshot().value("messages").toArray().size(), 2);
        QCOMPARE(f.client->snapshot().value("messages").toArray().last().toObject().value("content").toString(), QString("请确认偏好"));
    }
    void replyOutboxUsesSameEventAndDeduplicatesCallbacks() {
        Fixture f; QVERIFY(f.login()); QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        f.client->submit("记住爵士"); QTRY_COMPARE(accepted.count(), 1);
        const auto turn = accepted.at(0).at(0).toString(); f.hangEvent = true;
        f.client->appendLocalReply(turn, "请确认"); f.client->appendLocalReply(turn, "请确认");
        QTRY_VERIFY(!f.client->snapshot().value("journalPending").toBool());
        QCOMPARE(f.events.size(), 1); QCOMPARE(f.current.value("messages").toArray().size(), 2);
        QCOMPARE(f.server.count("POST", "/v1/conversations/conversation-1/events"), 2);
        f.client->appendLocalReply(turn, "请确认"); QTest::qWait(40);
        QCOMPARE(f.server.count("POST", "/v1/conversations/conversation-1/events"), 2);
    }
    void accountSwitchDiscardsLateReceipt() {
        Fixture f; QVERIFY(f.login()); f.messageDelay = 70;
        QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        f.client->submit("旧账号消息");
        QTRY_COMPARE(f.postedIds.size(), 1);
        QVERIFY(f.login("bob")); QTest::qWait(100);
        QCOMPARE(accepted.count(), 0); QVERIFY(f.client->snapshot().value("id").toString().isEmpty());
        QVERIFY(!f.client->snapshot().contains("pendingMessageId"));
    }
    void oldVersionCannotOverwriteNewerSnapshot() {
        Fixture f; QVERIFY(f.login()); QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        f.client->submit("找歌"); QTRY_COMPARE(accepted.count(), 1);
        const auto old = f.current;
        f.current.insert("version", 9); f.client->restoreLatest(); QTRY_VERIFY(!f.client->busy());
        QCOMPARE(f.client->snapshot().value("version").toInt(), 9);
        f.eventOverride = old;
        f.client->appendLocalReply(accepted.at(0).at(0).toString(), "旧快照回复");
        QTRY_VERIFY(!f.client->snapshot().value("journalPending").toBool());
        QCOMPARE(f.client->snapshot().value("version").toInt(), 9);
    }
    void unknownHintNeverExecutes() {
        Fixture f; QVERIFY(f.login()); f.invalidHint = true;
        QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        QSignalSpy failed(f.client, &ConversationClient::submissionFailed);
        f.client->submit("未知能力"); QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(accepted.count(), 0); QVERIFY(f.client->snapshot().contains("pendingMessageId"));
        QCOMPARE(f.postedIds.size(), 1);
    }
    void createRetryKeepsRequestAndMessageIds() {
        Fixture f; QVERIFY(f.login()); f.hangCreate = true;
        QSignalSpy failed(f.client, &ConversationClient::submissionFailed);
        QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        f.client->submit("睡前听歌"); QTRY_COMPARE(failed.count(), 1);
        const auto id = f.client->snapshot().value("pendingMessageId").toString();
        f.hangCreate = false; f.client->submit("睡前听歌"); QTRY_COMPARE(accepted.count(), 1);
        QCOMPARE(f.createIds.size(), 2); QCOMPARE(f.createIds.at(0), f.createIds.at(1));
        QCOMPARE(f.postedIds.first(), id);
    }
    void invalidInputDoesNotPost() {
        Fixture f; QVERIFY(f.login()); QSignalSpy failed(f.client, &ConversationClient::submissionFailed);
        f.client->submit("  "); f.client->submit(QString(1001, QChar('x')));
        QCOMPARE(failed.count(), 2); QCOMPARE(f.server.count("POST", "/v1/conversations"), 0);
    }
    void detachDropsDelayedSnapshot() {
        Fixture f; QVERIFY(f.login()); QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        f.client->submit("找歌"); QTRY_COMPARE(accepted.count(), 1);
        f.latestDelay = 70; f.client->restoreLatest();
        QTRY_COMPARE(f.server.count("GET", "/v1/conversations/latest/"+f.client->deviceId()), 1);
        f.client->detach(); QTest::qWait(100);
        QVERIFY(f.client->snapshot().value("id").toString().isEmpty()); QCOMPARE(accepted.count(), 1);
    }
    void earlierPagesMergeByStableIdWithoutLosingLoadedHistory() {
        Fixture f; QVERIFY(f.login());
        QJsonArray all;
        for (int i = 1; i <= 45; ++i) {
            const auto id = QStringLiteral("message-%1").arg(i, 4, 10, QLatin1Char('0'));
            all.append(QJsonObject{{"id", id}, {"turnId", id}, {"role", "user"}, {"source", "user"},
                {"content", QStringLiteral("历史 %1").arg(i)}, {"sequence", i}, {"createdAt", "2026-10-20T10:00:00.000Z"}});
        }
        QJsonArray tail, earlier;
        for (int i = 0; i < all.size(); ++i) (i < 5 ? earlier : tail).append(all.at(i));
        f.current = {{"id", "conversation-1"}, {"deviceId", f.client->deviceId()}, {"version", 45},
            {"messages", tail}, {"hasEarlier", true}, {"earliestSequence", 6}};
        const auto later = f.current;
        f.current.insert("messages", QJsonArray{all.at(0), all.at(1)}); f.current.insert("version", 2);
        f.current.insert("hasEarlier", false); f.current.insert("earliestSequence", 1);
        f.client->restoreLatest(); QTRY_VERIFY(!f.client->busy());
        QCOMPARE(f.client->snapshot().value("messages").toArray().size(), 2);
        f.current = later;
        const auto handler = f.server.handler;
        f.server.handler = [handler, &f, earlier](const Request &request) -> Response {
            if (request.path == "/v1/conversations/conversation-1/before/6") {
                auto page = f.current; page.insert("messages", earlier); page.insert("hasEarlier", false); page.insert("earliestSequence", 1);
                return {{{"conversation", page}}};
            }
            return handler(request);
        };
        QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        f.client->restoreLatest(); QTRY_VERIFY(!f.client->busy());
        QCOMPARE(f.client->snapshot().value("messages").toArray().size(), 42);
        QCOMPARE(f.client->snapshot().value("earliestSequence").toInt(), 6);
        f.client->loadEarlier(); QTRY_VERIFY(!f.client->busy());
        QCOMPARE(f.client->snapshot().value("messages").toArray().size(), 45);
        QCOMPARE(f.client->snapshot().value("earliestSequence").toInt(), 1);
        QVERIFY(!f.client->snapshot().value("hasEarlier").toBool());
        f.client->restoreLatest(); QTRY_VERIFY(!f.client->busy());
        QCOMPARE(f.client->snapshot().value("messages").toArray().size(), 45);
        QCOMPARE(accepted.count(), 0);
    }
    void malformedReplacementKeepsExistingMessages() {
        Fixture f; QVERIFY(f.login()); QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        QSignalSpy errors(f.client, &ConversationClient::error);
        f.client->submit("保留原文"); QTRY_COMPARE(accepted.count(), 1);
        auto messages = f.current.value("messages").toArray();
        auto changed = messages.first().toObject(); changed.insert("content", "试图覆盖原文"); messages.replace(0, changed);
        f.current.insert("messages", messages); f.current.insert("version", 10);
        f.client->restoreLatest(); QTRY_VERIFY(!f.client->busy());
        QCOMPARE(errors.count(), 1);
        QCOMPARE(f.client->snapshot().value("messages").toArray().first().toObject().value("content").toString(), QString("保留原文"));
        QCOMPARE(f.client->snapshot().value("version").toInt(), 1);
    }
    void taskSyncDedupeIncludesStateVersion() {
        Fixture f; QVERIFY(f.login()); QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        f.client->submit("找歌"); QTRY_COMPARE(accepted.count(), 1);
        const auto turn = accepted.at(0).at(0).toString();
        f.client->syncTask(turn, "task-0001", 1, 1); f.client->syncTask(turn, "task-0001", 1, 1);
        QTRY_VERIFY(!f.client->snapshot().value("journalPending").toBool());
        QCOMPARE(f.server.count("POST", "/v1/conversations/conversation-1/events"), 1);
        f.client->syncTask(turn, "task-0001", 1, 2);
        QTRY_VERIFY(!f.client->snapshot().value("journalPending").toBool());
        QCOMPARE(f.server.count("POST", "/v1/conversations/conversation-1/events"), 2);
    }
    void unacknowledgedReplyRemainsInOutbox() {
        Fixture f; QVERIFY(f.login()); QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        f.client->submit("找歌"); QTRY_COMPARE(accepted.count(), 1);
        const auto handler = f.server.handler;
        f.server.handler = [handler, &f](const Request &request) -> Response {
            if (request.path == "/v1/conversations/conversation-1/events") return {{{"conversation", f.current}}};
            return handler(request);
        };
        f.client->appendLocalReply(accepted.at(0).at(0).toString(), "无回执");
        QTRY_COMPARE(f.server.count("POST", "/v1/conversations/conversation-1/events"), 2);
        QTest::qWait(50);
        QVERIFY(f.client->snapshot().value("journalPending").toBool());
    }
    void utf16LimitsMatchUiAndTaskProtocol() {
        Fixture f; QVERIFY(f.login());
        const uint codepoint[] = {0x1f3b5}; const auto emoji = QString::fromUcs4(codepoint, 1);
        QCOMPARE(emoji.size(), 2);
        QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        QSignalSpy failed(f.client, &ConversationClient::submissionFailed);
        f.client->submit(emoji.repeated(500)); QTRY_COMPARE(accepted.count(), 1);
        f.client->submit(emoji.repeated(501)); QCOMPARE(failed.count(), 1);
        QCOMPARE(f.postedIds.size(), 1);
        const auto turn = accepted.at(0).at(0).toString();
        f.client->appendLocalReply(turn, emoji.repeated(800));
        QTRY_VERIFY(!f.client->snapshot().value("journalPending").toBool());
        QCOMPARE(f.server.count("POST", "/v1/conversations/conversation-1/events"), 1);
        f.client->appendLocalReply(turn, emoji.repeated(801)); QTest::qWait(20);
        QCOMPARE(f.server.count("POST", "/v1/conversations/conversation-1/events"), 1);
    }
    void permanentEventRejectionStopsRetriesAndReleasesOutbox() {
        Fixture f; QVERIFY(f.login()); QSignalSpy accepted(f.client, &ConversationClient::messageAccepted);
        f.client->submit("找歌"); QTRY_COMPARE(accepted.count(), 1);
        const auto handler = f.server.handler;
        f.server.handler = [handler](const Request &request) -> Response {
            if (request.path == "/v1/conversations/conversation-1/events")
                return {{{"error", QJsonObject{{"message", "会话已满，请新建会话"}}}}, 409};
            return handler(request);
        };
        QSignalSpy errors(f.client, &ConversationClient::error);
        f.client->appendLocalReply(accepted.at(0).at(0).toString(), "回复");
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(!f.client->snapshot().value("journalPending").toBool());
        QTest::qWait(300);
        QCOMPARE(f.server.count("POST", "/v1/conversations/conversation-1/events"), 1);
        QCOMPARE(accepted.count(), 1);
        f.client->startNew(); QVERIFY(f.client->snapshot().value("id").toString().isEmpty());
    }
};
QTEST_GUILESS_MAIN(ConversationClientTest)
#include "conversationclient_test.moc"
