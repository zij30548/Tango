#include "appbackendclient.h"
#include "focusmodelclient.h"
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>
#include <functional>
#include <memory>

namespace
{
class ScopedEnvironment
{
  public:
    ~ScopedEnvironment()
    {
        for (auto i = saved.crbegin(); i != saved.crend(); ++i)
        {
            if (i->existed)
                qputenv(i->name.constData(), i->value);
            else
                qunsetenv(i->name.constData());
        }
    }
    void set(const QByteArray &name, const QByteArray &value)
    {
        saved.append(
            {name, qgetenv(name.constData()), qEnvironmentVariableIsSet(name.constData())});
        qputenv(name.constData(), value);
    }

  private:
    struct Value
    {
        QByteArray name, value;
        bool existed;
    };
    QVector<Value> saved;
};

QJsonObject validRequest()
{
    return {{"minutes", 50},
            {"scene", "work"},
            {"source", "favorites"},
            {"excludeLive", true},
            {"instrumentalOnly", false},
            {"excludedArtists", QJsonArray{QStringLiteral("测试歌手")}}};
}

QByteArray completion(const QJsonObject &request)
{
    return QJsonDocument(QJsonObject{{"request", request}}).toJson(QJsonDocument::Compact);
}

QByteArray loginResult(const QString &id, const QString &username,
                       const QString &token = QStringLiteral("replacement-session-token-123456789"),
                       const QDateTime &until = QDateTime::currentDateTimeUtc().addSecs(3600))
{
    return QJsonDocument(QJsonObject{
                             {"token", token},
                             {"user", QJsonObject{{"id", id}, {"username", username}}},
                             {"expiresAt", until.toString(Qt::ISODateWithMs)}})
        .toJson(QJsonDocument::Compact);
}

class FakeModel : public QObject
{
  public:
    struct Request
    {
        QByteArray firstLine, headers, body;
    };
    struct Response
    {
        int status = 200;
        QByteArray body;
        QByteArray location;
        int delay = 0;
        bool hang = false;
    };
    QVector<Request> requests;
    QVector<Response> responses;
    QHash<QByteArray, QVector<Response>> overrides;
    QTcpServer server;
    FakeModel()
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(
            &server, &QTcpServer::newConnection, this,
            [this]
            {
                while (server.hasPendingConnections())
                {
                    auto *socket = server.nextPendingConnection();
                    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    auto bytes = std::make_shared<QByteArray>();
                    auto handled = std::make_shared<bool>(false);
                    connect(
                        socket, &QTcpSocket::readyRead, this,
                        [this, socket, bytes, handled]
                        {
                            bytes->append(socket->readAll());
                            if (*handled)
                                return;
                            const int headerEnd = bytes->indexOf("\r\n\r\n");
                            if (headerEnd < 0)
                                return;
                            const QByteArray headers = bytes->left(headerEnd);
                            int contentLength = 0;
                            for (const auto &line : headers.split('\n'))
                            {
                                if (line.toLower().startsWith("content-length:"))
                                    contentLength =
                                        line.mid(line.indexOf(':') + 1).trimmed().toInt();
                            }
                            if (bytes->size() < headerEnd + 4 + contentLength)
                                return;
                            *handled = true;
                            requests.append({headers.split('\n').first().trimmed(), headers,
                                             bytes->mid(headerEnd + 4, contentLength)});
                            const auto route = headers.split(' ').value(1);
                            Response response;
                            if (!overrides[route].isEmpty())
                                response = overrides[route].takeFirst();
                            else if (route.endsWith("/v1/auth/login") ||
                                     route.endsWith("/v1/auth/register"))
                                response.body =
                                    QJsonDocument(
                                        QJsonObject{
                                            {"token", "app-session-only-token-123456789"},
                                            {"user", QJsonObject{{"id", "user-123"},
                                                                 {"username", "alice"}}},
                                            {"expiresAt",
                                             QDateTime::currentDateTimeUtc().addSecs(3600).toString(
                                                 Qt::ISODate)}})
                                        .toJson();
                            else if (route.endsWith("/v1/agent/status"))
                                response.body = "{\"configured\":true,\"model\":\"server-model\"}";
                            else if (route.endsWith("/v1/auth/me"))
                                response.body =
                                    QJsonDocument(
                                        QJsonObject{
                                            {"user", QJsonObject{{"id", "user-123"},
                                                                 {"username", "alice"}}},
                                            {"expiresAt",
                                             QDateTime::currentDateTimeUtc().addSecs(3600).toString(
                                                 Qt::ISODate)}})
                                        .toJson();
                            else if (route.endsWith("/v1/auth/logout"))
                                response.body = "{\"ok\":true}";
                            else
                                response =
                                    responses.isEmpty()
                                        ? Response{200, completion(validRequest()), {}, 0, false}
                                        : responses.takeFirst();
                            if (response.hang)
                                return;
                            QPointer<QTcpSocket> guard(socket);
                            QTimer::singleShot(
                                response.delay, this,
                                [guard, response]
                                {
                                    if (!guard || guard->state() != QAbstractSocket::ConnectedState)
                                        return;
                                    QByteArray headers = "HTTP/1.1 " +
                                                         QByteArray::number(response.status) +
                                                         " Response\r\n";
                                    headers += "Content-Type: application/json\r\nConnection: "
                                               "close\r\nContent-Length: " +
                                               QByteArray::number(response.body.size()) + "\r\n";
                                    if (!response.location.isEmpty())
                                        headers += "Location: " + response.location + "\r\n";
                                    guard->write(headers + "\r\n" + response.body);
                                    guard->disconnectFromHost();
                                });
                        });
                }
            });
    }
    QByteArray base() const
    {
        return "http://127.0.0.1:" + QByteArray::number(server.serverPort()) + "/app";
    }
};
} // namespace

class FocusModelTest : public QObject
{
    Q_OBJECT
  private slots:
    void init();
    void cleanup();
    void unconfiguredDoesNotConnect();
    void rejectsUnsafeConfiguration_data();
    void rejectsUnsafeConfiguration();
    void parsesAndSendsOnlyConstraints();
    void rejectsInvalidSchema_data();
    void rejectsInvalidSchema();
    void rejectsIncompleteEnvelope_data();
    void rejectsIncompleteEnvelope();
    void applicationSessionAndLogout();
    void unauthorizedClearsSession();
    void switchingBackendNeverLeaksSession();
    void loginLifetimeAndMalformedResponses();
    void failedAccountSwitchKeepsCurrentSession_data();
    void failedAccountSwitchKeepsCurrentSession();
    void cancelledAccountSwitchKeepsCurrentSession();
    void accountSwitchTimeoutKeepsCurrentSession();
    void newestAuthenticationWinsWithoutGuestTransition();
    void logoutInvalidatesPendingAccountSwitch();
    void backendChangeInvalidatesPendingAccountSwitch();
    void staleAccountRepliesCannotClearNewSession_data();
    void staleAccountRepliesCannotClearNewSession();
    void sessionExpiryInvalidatesPendingAccountSwitch();
    void unavailableServiceAndUnsupportedRequest();
    void boundsOversizedReplyAndTimesOut();
    void rejectsRedirectWithoutLeakingSession();
    void cancellationAndLifetimeSuppressStaleCallbacks();

  private:
    std::unique_ptr<ScopedEnvironment> environment;
    void provider(FakeModel &server)
    {
        environment->set("MUSICPLAYER_BACKEND_URL", server.base());
        auto *backend = AppBackendClient::instance();
        int callbacks = 0;
        QString failure;
        backend->login("alice", "test-password", this,
                       [&](const QString &error)
                       {
                           ++callbacks;
                           failure = error;
                       });
        QTRY_COMPARE(callbacks, 1);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        backend->refreshAgentStatus(this,
                                    [&](const QString &error)
                                    {
                                        ++callbacks;
                                        failure = error;
                                    });
        QTRY_COMPARE(callbacks, 2);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(backend->agentReady());
        server.requests.clear();
    }
};

void FocusModelTest::init()
{
    environment.reset(new ScopedEnvironment);
    for (const QByteArray &name :
         {QByteArray("MUSICPLAYER_BACKEND_URL"), QByteArray("MUSICPLAYER_AGENT_MODEL"),
          QByteArray("MUSICPLAYER_AGENT_API_KEY"), QByteArray("DASHSCOPE_API_KEY")})
        environment->set(name, {});
}
void FocusModelTest::cleanup() { environment.reset(); }

void FocusModelTest::unconfiguredDoesNotConnect()
{
    FakeModel server;
    environment->set("MUSICPLAYER_AGENT_BASE_URL", server.base());
    environment->set("MUSICPLAYER_AGENT_API_KEY", "ignored-provider-secret");
    environment->set("DASHSCOPE_API_KEY", "ignored-provider-alias");
    QVERIFY(!AppBackendClient::enabled());
    QVERIFY(AppBackendClient::configurationError().isEmpty());
    QVERIFY(!FocusModelClient::configured());
    FocusModelClient client;
    int called = 0;
    client.parse(QStringLiteral("工作25分钟"), {}, this,
                 [&](const FocusRequest &, const QString &error)
                 {
                     ++called;
                     QVERIFY(error.contains(QStringLiteral("尚未配置")));
                 });
    QCOMPARE(called, 1);
    environment->set("MUSICPLAYER_BACKEND_URL", server.base());
    client.parse(QStringLiteral("工作25分钟"), {}, this,
                 [&](const FocusRequest &, const QString &error)
                 {
                     ++called;
                     QVERIFY(error.contains(QStringLiteral("登录探歌")));
                 });
    QCOMPARE(called, 2);
    QTest::qWait(30);
    QCOMPARE(server.requests.size(), 0);
}

void FocusModelTest::rejectsUnsafeConfiguration_data()
{
    QTest::addColumn<QByteArray>("base");
    QTest::newRow("insecure remote") << QByteArray("http://example.com/v1");
    QTest::newRow("userinfo") << QByteArray("https://name:secret@example.com/v1");
    QTest::newRow("query") << QByteArray("https://example.com/v1?api_key=x");
    QTest::newRow("fragment") << QByteArray("https://example.com/v1#fragment");
    QTest::newRow("localhost lookalike") << QByteArray("http://localhost.example.com/v1");
    QTest::newRow("traversal") << QByteArray("https://example.com/v1/../api");
    QTest::newRow("encoded path") << QByteArray("https://example.com/v1%2Fapi");
    QTest::newRow("zero port") << QByteArray("https://example.com:0");
}
void FocusModelTest::rejectsUnsafeConfiguration()
{
    QFETCH(QByteArray, base);
    environment->set("MUSICPLAYER_BACKEND_URL", base);
    environment->set("MUSICPLAYER_AGENT_API_KEY", "test-local-secret");
    QVERIFY(!FocusModelClient::configured());
    QVERIFY(!FocusModelClient::configurationError().isEmpty());
}

void FocusModelTest::parsesAndSendsOnlyConstraints()
{
    FakeModel server;
    provider(server);
    FocusModelClient client;
    FocusRequest base;
    base.originalText = QStringLiteral("private previous request not sent");
    base.minutes = 35;
    int callbacks = 0;
    FocusRequest parsed;
    QString failure;
    const QString text = QStringLiteral("用收藏里的歌工作 50 分钟，不要现场版，不要测试歌手");
    client.parse(text, base, this,
                 [&](const FocusRequest &value, const QString &error)
                 {
                     ++callbacks;
                     parsed = value;
                     failure = error;
                 });
    QTRY_COMPARE(callbacks, 1);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QCOMPARE(parsed.minutes, 50);
    QCOMPARE(parsed.source, QStringLiteral("favorites"));
    QVERIFY(parsed.excludeLive);
    QCOMPARE(parsed.excludedArtists, QStringList{QStringLiteral("测试歌手")});
    QCOMPARE(parsed.originalText, text);
    QCOMPARE(server.requests.size(), 1);
    const auto request = server.requests.first();
    QCOMPARE(request.firstLine, QByteArray("POST /app/v1/focus/parse HTTP/1.1"));
    QVERIFY(request.headers.contains("Authorization: Bearer app-session-only-token-123456789"));
    const auto input = QJsonDocument::fromJson(request.body).object();
    QCOMPARE(input.keys(), QStringList({"currentRequest", "instruction"}));
    QCOMPARE(input.value("currentRequest").toObject().value("minutes").toInt(), 35);
    QVERIFY(!request.body.contains("private previous"));
    QVERIFY(!request.body.contains("test-local-secret"));
    QVERIFY(!request.body.contains("messages"));
    QVERIFY(!request.body.contains("api_key"));
}

void FocusModelTest::rejectsInvalidSchema_data()
{
    QTest::addColumn<QJsonObject>("object");
    auto object = validRequest();
    object.insert("songs", QJsonArray{1});
    QTest::newRow("unknown field") << object;
    object = validRequest();
    object.remove("excludeLive");
    QTest::newRow("missing field") << object;
    object = validRequest();
    object.insert("minutes", 25.5);
    QTest::newRow("fractional minutes") << object;
    object = validRequest();
    object.insert("minutes", 181);
    QTest::newRow("out of range") << object;
    object = validRequest();
    object.insert("scene", "relax");
    QTest::newRow("unknown scene") << object;
    object = validRequest();
    object.insert("source", "cloud");
    QTest::newRow("unknown source") << object;
    object = validRequest();
    object.insert("excludeLive", "true");
    QTest::newRow("coerced boolean") << object;
    object = validRequest();
    object.insert("excludedArtists", QJsonArray{3});
    QTest::newRow("non string artist") << object;
    object = validRequest();
    object.insert("excludedArtists", QJsonArray{"Test", "test"});
    QTest::newRow("duplicate artist") << object;
    object = validRequest();
    object.insert("unsupported", QJsonArray{QStringLiteral("只要日语歌")});
    QTest::newRow("unsupported constraint") << object;
}
void FocusModelTest::rejectsInvalidSchema()
{
    QFETCH(QJsonObject, object);
    FakeModel server;
    server.responses.append({200, completion(object), {}, 0, false});
    provider(server);
    FocusModelClient client;
    int callbacks = 0;
    QString failure;
    client.parse(QStringLiteral("测试"), {}, this,
                 [&](const FocusRequest &value, const QString &error)
                 {
                     ++callbacks;
                     failure = error;
                     QCOMPARE(value.minutes, 25);
                 });
    QTRY_COMPARE(callbacks, 1);
    QVERIFY(!failure.isEmpty());
}

void FocusModelTest::rejectsIncompleteEnvelope_data()
{
    QTest::addColumn<QByteArray>("body");
    QTest::newRow("invalid JSON") << QByteArray("not JSON");
    QTest::newRow("provider envelope") << QByteArray("{\"choices\":[]}");
    QTest::newRow("missing request") << QByteArray("{}");
    QTest::newRow("wrong request") << QByteArray("{\"request\":[]}");
    QTest::newRow("tool calls") << QJsonDocument(QJsonObject{{"request", validRequest()},
                                                             {"tool_calls", QJsonArray{}}})
                                       .toJson();
}

void FocusModelTest::rejectsIncompleteEnvelope()
{
    QFETCH(QByteArray, body);
    FakeModel server;
    server.responses.append({200, body, {}, 0, false});
    provider(server);
    FocusModelClient client;
    int callbacks = 0;
    QString failure;
    client.parse(QStringLiteral("测试"), {}, this,
                 [&](const FocusRequest &, const QString &error)
                 {
                     ++callbacks;
                     failure = error;
                 });
    QTRY_COMPARE(callbacks, 1);
    QVERIFY(!failure.isEmpty());
}

void FocusModelTest::applicationSessionAndLogout()
{
    FakeModel server;
    provider(server);
    auto *backend = AppBackendClient::instance();
    QCOMPARE(backend->userId(), QStringLiteral("user-123"));
    QCOMPARE(backend->username(), QStringLiteral("alice"));
    QCOMPARE(backend->modelName(), QStringLiteral("server-model"));
    environment->set("MUSICPLAYER_AGENT_API_KEY", "ignored-provider-secret");
    environment->set("DASHSCOPE_API_KEY", "ignored-provider-alias");
    int calls = 0;
    backend->me(this,
                [&](const QString &error)
                {
                    QVERIFY(error.isEmpty());
                    ++calls;
                });
    QTRY_COMPARE(calls, 1);
    FocusModelClient client;
    client.parse("work", {}, this,
                 [&](const FocusRequest &, const QString &error)
                 {
                     QVERIFY(error.isEmpty());
                     ++calls;
                 });
    QTRY_COMPARE(calls, 2);
    for (const auto &request : server.requests)
    {
        QVERIFY(!request.headers.contains("ignored-provider"));
        QVERIFY(!request.body.contains("ignored-provider"));
    }
    backend->logout(this,
                    [&](const QString &error)
                    {
                        QVERIFY(error.isEmpty());
                        ++calls;
                    });
    QVERIFY(!backend->authenticated());
    QVERIFY(!FocusModelClient::configured());
    QTRY_COMPARE(calls, 3);
}
void FocusModelTest::unauthorizedClearsSession()
{
    FakeModel server;
    provider(server);
    auto *backend = AppBackendClient::instance();
    QSignalSpy auth(backend, &AppBackendClient::authChanged);
    server.responses.append({401,
                             "{\"error\":{\"code\":\"AUTH_REQUIRED\",\"message\":"
                             "\"session expired\"}}",
                             {},
                             0,
                             false});
    FocusModelClient client;
    int calls = 0;
    // authChanged cancels the focus generation: session invalidation is the
    // result.
    client.parse("work", {}, this, [&](const FocusRequest &, const QString &) { ++calls; });
    QTRY_VERIFY(!backend->authenticated());
    QCOMPARE(auth.count(), 1);
    QVERIFY(!backend->agentReady());
    QCOMPARE(calls, 0);
}
void FocusModelTest::switchingBackendNeverLeaksSession()
{
    FakeModel server, other;
    provider(server);
    auto *backend = AppBackendClient::instance();
    const auto previous = backend->serviceIdentity();
    environment->set("MUSICPLAYER_BACKEND_URL", other.base());
    backend = AppBackendClient::instance();
    QVERIFY(!backend->authenticated());
    QVERIFY(backend->serviceIdentity() != previous);
    int calls = 0;
    backend->me(this,
                [&](const QString &error)
                {
                    QVERIFY(!error.isEmpty());
                    ++calls;
                });
    QCOMPARE(calls, 1);
    QTest::qWait(20);
    QCOMPARE(other.requests.size(), 0);
}

void FocusModelTest::loginLifetimeAndMalformedResponses()
{
    FakeModel server;
    environment->set("MUSICPLAYER_BACKEND_URL", server.base());
    auto *backend = AppBackendClient::instance();
    const auto valid =
        QJsonDocument(
            QJsonObject{
                {"token", "app-session-only-token-123456789"},
                {"user", QJsonObject{{"id", "user-123"}, {"username", "alice"}}},
                {"expiresAt", QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODate)}})
            .toJson();
    server.overrides["/app/v1/auth/login"].append({200, valid, {}, 100, false});
    auto *context = new QObject;
    int callbacks = 0;
    backend->login("alice", "password", context, [&](const QString &) { ++callbacks; });
    QTRY_COMPARE(server.requests.size(), 1);
    delete context;
    QTest::qWait(140);
    QCOMPARE(callbacks, 0);
    QVERIFY(!backend->authenticated());
    auto invalid = QJsonDocument::fromJson(valid).object();
    invalid.insert("expiresAt", QDateTime::currentDateTimeUtc().addSecs(-1).toString(Qt::ISODate));
    for (const auto &body : QVector<QByteArray>{QJsonDocument(invalid).toJson(), QByteArray("{}")})
    {
        server.overrides["/app/v1/auth/login"].append({200, body, {}, 0, false});
        const auto before = callbacks;
        backend->login("alice", "password", this,
                       [&](const QString &error)
                       {
                           QVERIFY(!error.isEmpty());
                           ++callbacks;
                       });
        QTRY_COMPARE(callbacks, before + 1);
        QVERIFY(!backend->authenticated());
    }
}

void FocusModelTest::failedAccountSwitchKeepsCurrentSession_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::newRow("wrong password") << 401
                                    << QByteArray("{\"error\":{\"message\":\"Wrong password\"}}");
    QTest::newRow("malformed session") << 200 << QByteArray("{}");
    QTest::newRow("expired replacement")
        << 200 << loginResult("user-b", "bob", "expired-session-token-123456789",
                              QDateTime::currentDateTimeUtc().addSecs(-1));
}

void FocusModelTest::failedAccountSwitchKeepsCurrentSession()
{
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    FakeModel server;
    provider(server);
    auto *backend = AppBackendClient::instance();
    QSignalSpy auth(backend, &AppBackendClient::authChanged);
    server.overrides["/app/v1/auth/login"].append({status, body, {}, 20, false});
    int calls = 0;
    backend->login("bob", "wrong-password", this,
                   [&](const QString &error)
                   {
                       QVERIFY(!error.isEmpty());
                       ++calls;
                   });
    QVERIFY(backend->authenticated());
    QCOMPARE(backend->username(), QStringLiteral("alice"));
    QTRY_COMPARE(calls, 1);
    QVERIFY(backend->authenticated());
    QCOMPARE(backend->userId(), QStringLiteral("user-123"));
    QVERIFY(backend->agentReady());
    QCOMPARE(backend->modelName(), QStringLiteral("server-model"));
    QCOMPARE(auth.count(), 0);
    backend->me(this,
                [&](const QString &error)
                {
                    QVERIFY(error.isEmpty());
                    ++calls;
                });
    QTRY_COMPARE(calls, 2);
    QVERIFY(server.requests.last().headers.contains(
        "Authorization: Bearer app-session-only-token-123456789"));
}

void FocusModelTest::cancelledAccountSwitchKeepsCurrentSession()
{
    FakeModel server;
    provider(server);
    auto *backend = AppBackendClient::instance();
    QSignalSpy auth(backend, &AppBackendClient::authChanged);
    server.overrides["/app/v1/auth/login"].append(
        {200, loginResult("user-b", "bob"), {}, 150, false});
    auto *context = new QObject;
    int calls = 0;
    backend->login("bob", "password", context, [&](const QString &) { ++calls; });
    QTRY_COMPARE(server.requests.size(), 1);
    delete context;
    QTest::qWait(200);
    QCOMPARE(calls, 0);
    QCOMPARE(auth.count(), 0);
    QVERIFY(backend->authenticated());
    QCOMPARE(backend->username(), QStringLiteral("alice"));
    QVERIFY(backend->agentReady());
}

void FocusModelTest::accountSwitchTimeoutKeepsCurrentSession()
{
    FakeModel server;
    environment->set("MUSICPLAYER_BACKEND_URL", server.base());
    AppBackendClient backend(nullptr, 50);
    int calls = 0;
    backend.login("alice", "password", this,
                  [&](const QString &error)
                  {
                      QVERIFY(error.isEmpty());
                      ++calls;
                  });
    QTRY_COMPARE(calls, 1);
    QSignalSpy auth(&backend, &AppBackendClient::authChanged);
    server.overrides["/app/v1/auth/login"].append({200, {}, {}, 0, true});
    backend.login("bob", "password", this,
                  [&](const QString &error)
                  {
                      QVERIFY(error.contains(QStringLiteral("超时")));
                      ++calls;
                  });
    QTRY_COMPARE(calls, 2);
    QVERIFY(backend.authenticated());
    QCOMPARE(backend.username(), QStringLiteral("alice"));
    QCOMPARE(auth.count(), 0);
}

void FocusModelTest::newestAuthenticationWinsWithoutGuestTransition()
{
    FakeModel server;
    provider(server);
    auto *backend = AppBackendClient::instance();
    QSignalSpy auth(backend, &AppBackendClient::authChanged);
    server.overrides["/app/v1/auth/login"].append(
        {200, loginResult("user-b", "bob"), {}, 200, false});
    server.overrides["/app/v1/auth/login"].append(
        {200, loginResult("user-c", "carol"), {}, 0, false});
    int calls = 0;
    backend->login("bob", "password", this,
                   [&](const QString &error)
                   {
                       QVERIFY(!error.isEmpty());
                       ++calls;
                   });
    QTRY_COMPARE(server.requests.size(), 1);
    backend->login("carol", "password", this,
                   [&](const QString &error)
                   {
                       QVERIFY(error.isEmpty());
                       ++calls;
                   });
    QCOMPARE(backend->username(), QStringLiteral("alice"));
    QTRY_COMPARE(backend->username(), QStringLiteral("carol"));
    QCOMPARE(auth.count(), 1);
    QCOMPARE(auth.first().first().toString(), QStringLiteral("user-c"));
    QVERIFY(!backend->agentReady());
    QTRY_COMPARE(calls, 2);
    QCOMPARE(backend->userId(), QStringLiteral("user-c"));
    QCOMPARE(auth.count(), 1);
}

void FocusModelTest::logoutInvalidatesPendingAccountSwitch()
{
    FakeModel server;
    provider(server);
    auto *backend = AppBackendClient::instance();
    QSignalSpy auth(backend, &AppBackendClient::authChanged);
    server.overrides["/app/v1/auth/login"].append(
        {200, loginResult("user-b", "bob"), {}, 150, false});
    int calls = 0;
    backend->login("bob", "password", this,
                   [&](const QString &error)
                   {
                       QVERIFY(!error.isEmpty());
                       ++calls;
                   });
    QTRY_COMPARE(server.requests.size(), 1);
    backend->logout(this);
    QVERIFY(!backend->authenticated());
    QTRY_COMPARE(calls, 1);
    QVERIFY(!backend->authenticated());
    QCOMPARE(auth.count(), 1);
    QVERIFY(auth.first().first().toString().isEmpty());
}

void FocusModelTest::backendChangeInvalidatesPendingAccountSwitch()
{
    FakeModel server, other;
    provider(server);
    auto *backend = AppBackendClient::instance();
    server.overrides["/app/v1/auth/login"].append(
        {200, loginResult("user-b", "bob"), {}, 150, false});
    int calls = 0;
    backend->login("bob", "password", this,
                   [&](const QString &error)
                   {
                       QVERIFY(!error.isEmpty());
                       ++calls;
                   });
    QTRY_COMPARE(server.requests.size(), 1);
    environment->set("MUSICPLAYER_BACKEND_URL", other.base());
    AppBackendClient::instance();
    QVERIFY(!backend->authenticated());
    QTRY_COMPARE(calls, 1);
    QVERIFY(!backend->authenticated());
    QCOMPARE(other.requests.size(), 0);
}

void FocusModelTest::staleAccountRepliesCannotClearNewSession_data()
{
    QTest::addColumn<int>("meStatus");
    QTest::addColumn<QByteArray>("meBody");
    QTest::newRow("old identity") << 200
                                  << QByteArray("{\"user\":{\"id\":\"user-123\","
                                                "\"username\":\"alice\"},"
                                                "\"expiresAt\":\"2099-01-01T00:00:00Z\"}");
    QTest::newRow("old unauthorized") << 401
                                      << QByteArray("{\"error\":{\"message\":\"Expired\"}}");
}

void FocusModelTest::staleAccountRepliesCannotClearNewSession()
{
    QFETCH(int, meStatus);
    QFETCH(QByteArray, meBody);
    FakeModel server;
    provider(server);
    auto *backend = AppBackendClient::instance();
    server.overrides["/app/v1/agent/status"].append(
        {200, "{\"configured\":false,\"model\":\"\"}", {}, 200, false});
    server.overrides["/app/v1/auth/me"].append({meStatus, meBody, {}, 200, false});
    int staleCalls = 0;
    const auto stale = [&](const QString &error)
    {
        QVERIFY(!error.isEmpty());
        ++staleCalls;
    };
    backend->refreshAgentStatus(this, stale);
    backend->me(this, stale);
    QTRY_COMPARE(server.requests.size(), 2);
    server.overrides["/app/v1/auth/login"].append(
        {200, loginResult("user-b", "bob"), {}, 0, false});
    int freshCalls = 0;
    backend->login("bob", "password", this,
                   [&](const QString &error)
                   {
                       QVERIFY(error.isEmpty());
                       ++freshCalls;
                   });
    QTRY_COMPARE(freshCalls, 1);
    server.overrides["/app/v1/agent/status"].append(
        {200, "{\"configured\":true,\"model\":\"new-account-model\"}", {}, 0, false});
    backend->refreshAgentStatus(this,
                                [&](const QString &error)
                                {
                                    QVERIFY(error.isEmpty());
                                    ++freshCalls;
                                });
    QTRY_COMPARE(freshCalls, 2);
    QVERIFY(backend->agentReady());
    QTRY_COMPARE(staleCalls, 2);
    QVERIFY(backend->authenticated());
    QCOMPARE(backend->username(), QStringLiteral("bob"));
    QVERIFY(backend->agentReady());
    QCOMPARE(backend->modelName(), QStringLiteral("new-account-model"));
}

void FocusModelTest::sessionExpiryInvalidatesPendingAccountSwitch()
{
    FakeModel server;
    environment->set("MUSICPLAYER_BACKEND_URL", server.base());
    AppBackendClient backend;
    server.overrides["/app/v1/auth/login"].append(
        {200, loginResult("user-123", "alice", "expiring-session-token-123456789",
                          QDateTime::currentDateTimeUtc().addMSecs(700)),
         {}, 0, false});
    int calls = 0;
    backend.login("alice", "password", this,
                  [&](const QString &error)
                  {
                      QVERIFY(error.isEmpty());
                      ++calls;
                  });
    QTRY_COMPARE(calls, 1);
    QSignalSpy auth(&backend, &AppBackendClient::authChanged);
    server.overrides["/app/v1/auth/login"].append(
        {200, loginResult("user-b", "bob"), {}, 1000, false});
    backend.login("bob", "password", this,
                  [&](const QString &error)
                  {
                      QVERIFY(!error.isEmpty());
                      ++calls;
                  });
    QTRY_VERIFY(!backend.authenticated());
    QTRY_COMPARE(calls, 2);
    QVERIFY(!backend.authenticated());
    QCOMPARE(auth.count(), 1);
    QVERIFY(auth.first().first().toString().isEmpty());
}

void FocusModelTest::unavailableServiceAndUnsupportedRequest()
{
    FakeModel server;
    provider(server);
    auto *backend = AppBackendClient::instance();
    server.overrides["/app/v1/agent/status"].append(
        {200, "{\"configured\":false,\"model\":\"\"}", {}, 0, false});
    int callbacks = 0;
    backend->refreshAgentStatus(this,
                                [&](const QString &error)
                                {
                                    QVERIFY(error.isEmpty());
                                    ++callbacks;
                                });
    QTRY_COMPARE(callbacks, 1);
    QVERIFY(!FocusModelClient::configured());
    FocusModelClient client;
    client.parse("work", {}, this,
                 [&](const FocusRequest &, const QString &error)
                 {
                     QVERIFY(error.contains(QStringLiteral("尚未就绪")));
                     ++callbacks;
                 });
    QCOMPARE(callbacks, 2);
    provider(server);
    server.responses.append({422,
                             "{\"error\":{\"code\":\"UNSUPPORTED_REQUEST\","
                             "\"message\":\"unsupported tempo\"}}",
                             {},
                             0,
                             false});
    client.parse("work", {}, this,
                 [&](const FocusRequest &, const QString &error)
                 {
                     QCOMPARE(error, QStringLiteral("unsupported tempo"));
                     ++callbacks;
                 });
    QTRY_COMPARE(callbacks, 3);
}

void FocusModelTest::boundsOversizedReplyAndTimesOut()
{
    FakeModel server;
    server.responses.append({200, QByteArray(300 * 1024, 'x'), {}, 0, false});
    server.responses.append({200, {}, {}, 0, true});
    provider(server);
    FocusModelClient client(nullptr, 80);
    int callbacks = 0;
    QString failure;
    const auto callback = [&](const FocusRequest &, const QString &error)
    {
        ++callbacks;
        failure = error;
    };
    client.parse(QStringLiteral("测试"), {}, this, callback);
    QTRY_COMPARE(callbacks, 1);
    QVERIFY(failure.contains(QStringLiteral("256 KiB")));
    client.parse(QStringLiteral("测试"), {}, this, callback);
    QTRY_COMPARE(callbacks, 2);
    QVERIFY(failure.contains(QStringLiteral("超时")));
}

void FocusModelTest::rejectsRedirectWithoutLeakingSession()
{
    FakeModel server, destination;
    server.responses.append({302, {}, destination.base() + "/v1/focus/parse", 0, false});
    provider(server);
    FocusModelClient client;
    int callbacks = 0;
    QString failure;
    client.parse(QStringLiteral("测试"), {}, this,
                 [&](const FocusRequest &, const QString &error)
                 {
                     ++callbacks;
                     failure = error;
                 });
    QTRY_COMPARE(callbacks, 1);
    QVERIFY(failure.contains(QStringLiteral("重定向")));
    QTest::qWait(30);
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(destination.requests.size(), 0);
}

void FocusModelTest::cancellationAndLifetimeSuppressStaleCallbacks()
{
    FakeModel server;
    for (int i = 0; i < 4; ++i)
        server.responses.append({200, completion(validRequest()), {}, 150, false});
    provider(server);
    FocusModelClient client;
    int callbacks = 0;
    const auto callback = [&](const FocusRequest &, const QString &) { ++callbacks; };
    client.parse(QStringLiteral("第一个请求"), {}, this, callback);
    QTRY_COMPARE(server.requests.size(), 1);
    client.cancel();
    QTest::qWait(170);
    QCOMPARE(callbacks, 0);
    auto *context = new QObject;
    client.parse(QStringLiteral("第二个请求"), {}, context, callback);
    QTRY_COMPARE(server.requests.size(), 2);
    delete context;
    QTest::qWait(170);
    QCOMPARE(callbacks, 0);
    client.parse(QStringLiteral("第三个请求"), {}, this, callback);
    QTRY_COMPARE(server.requests.size(), 3);
    client.parse(QStringLiteral("第四个请求"), {}, this, callback);
    QTRY_COMPARE(callbacks, 1);
    QCOMPARE(server.requests.size(), 4);
    auto *shortLived = new FocusModelClient;
    server.responses.append({200, completion(validRequest()), {}, 150, false});
    shortLived->parse(QStringLiteral("第五个请求"), {}, this, callback);
    QTRY_COMPARE(server.requests.size(), 5);
    delete shortLived;
    QTest::qWait(170);
    QCOMPARE(callbacks, 1);
}

QTEST_GUILESS_MAIN(FocusModelTest)
#include "focusmodel_test.moc"
