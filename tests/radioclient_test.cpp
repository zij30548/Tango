#include "appbackendclient.h"
#include "radioclient.h"
#include "radiostore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QtTest>
#include <functional>
#include <memory>

namespace {
class ScopedEnvironment {
public:
    void set(const QByteArray &name, const QByteArray &value) {
        saved.append({name, qgetenv(name.constData()), qEnvironmentVariableIsSet(name.constData())});
        qputenv(name.constData(), value);
    }
    ~ScopedEnvironment() {
        for (auto it = saved.crbegin(); it != saved.crend(); ++it)
            if (it->existed) qputenv(it->name.constData(), it->value); else qunsetenv(it->name.constData());
    }
private:
    struct Value { QByteArray name, value; bool existed; };
    QVector<Value> saved;
};

class FakeServer : public QObject {
public:
    struct Request {
        QByteArray target, headers, body;
        QString path() const { return QUrl::fromEncoded(target).path(); }
        QUrlQuery query() const { return QUrlQuery(QUrl::fromEncoded(target)); }
        QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
    };
    struct Response {
        QJsonObject body;
        int status = 200;
        int delay = 0;
        bool hang = false;
    };
    QTcpServer server;
    QVector<Request> requests;
    QHash<QString, QVector<Response>> overrides;
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
                    const int end = bytes->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray headers = bytes->left(end);
                    int length = 0;
                    for (const auto &line : headers.split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    if (bytes->size() < end + 4 + length) return;
                    *handled = true;
                    Request request{headers.split(' ').value(1), headers, bytes->mid(end + 4, length)};
                    requests.append(request);
                    const QString path = request.path();
                    Response response;
                    if (!overrides[path].isEmpty()) response = overrides[path].takeFirst();
                    else if (handler) response = handler(request);
                    else response = {{{"error", "unhandled test route"}}, 404};
                    if (response.hang) return;
                    QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(response.delay, this, [guard, response] {
                        if (!guard || guard->state() != QAbstractSocket::ConnectedState) return;
                        const QByteArray body = QJsonDocument(response.body).toJson(QJsonDocument::Compact);
                        guard->write("HTTP/1.1 " + QByteArray::number(response.status) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        guard->disconnectFromHost();
                    });
                });
            }
        });
    }
    QByteArray base() const { return "http://127.0.0.1:" + QByteArray::number(server.serverPort()); }
    int count(const QString &path) const {
        int value = 0; for (const auto &request : requests) if (request.path() == path) ++value; return value;
    }
    Request last(const QString &path) const {
        for (auto it = requests.crbegin(); it != requests.crend(); ++it) if (it->path() == path) return *it;
        return {};
    }
};

QJsonObject plan() {
    return {{"title", "下班后的音乐"}, {"queries", QJsonArray{"轻快中文", "夜间散步"}},
            {"excludedArtists", QJsonArray{"Avoid Artist"}}};
}
QJsonObject selection(const QJsonArray &tracks = QJsonArray{QJsonObject{{"id", 16}, {"reason", "来自适合散步的歌单"}}, QJsonObject{{"id", 11}, {"reason", "为今晚留一点节奏"}}}) {
    return {{"title", "晚风私人电台"}, {"note", "为下班路上找到的真实曲目"}, {"tracks", tracks}};
}
QJsonObject song(qint64 id, const QString &title, const QString &artist) {
    return {{"id", id}, {"title", title}, {"artist", artist}, {"album", "API album"},
            {"coverUrl", QStringLiteral("https://p1.music.126.net/real-%1.jpg").arg(id)},
            {"durationMs", 188000}, {"fee", 1}};
}
QJsonObject assistantPlan(const QString &kind = QStringLiteral("radio")) {
    auto value = plan();
    value.insert("kind", kind);
    value.insert("note", QStringLiteral("根据这次音乐请求选择工具"));
    if (kind == QStringLiteral("releases")) value.insert("artist", QStringLiteral("邓紫棋"));
    if (kind == QStringLiteral("remember")) value.insert("preference", QStringLiteral("喜欢舒缓华语，少一些现场版"));
    return value;
}
QJsonObject artistRow(qint64 id = 7763, const QString &name = QStringLiteral("G.E.M.邓紫棋")) {
    return {{"id", id}, {"name", name}, {"aliases", QJsonArray{QStringLiteral("邓紫棋")}},
            {"coverUrl", QStringLiteral("https://p1.music.126.net/artist-%1.jpg").arg(id)}};
}
QJsonObject albumRow(qint64 id, qint64 published) {
    return {{"id", id}, {"name", QStringLiteral("真实专辑 %1").arg(id)},
            {"publishTime", published}, {"type", "Single"},
            {"coverUrl", QStringLiteral("https://p1.music.126.net/album-%1.jpg").arg(id)}};
}
AssistantArtist selectedArtist(qint64 id = 7763) {
    return {id, QStringLiteral("G.E.M.邓紫棋"), {QStringLiteral("邓紫棋")},
            QStringLiteral("https://p1.music.126.net/artist-7763.jpg")};
}

struct Fixture {
    ScopedEnvironment environment;
    QTemporaryDir directory;
    FakeServer backend, music;
    QObject context;
    QByteArray localToken = "local-bridge-token-only-123456789";
    qint64 releaseCheckedAt = QDateTime::currentMSecsSinceEpoch();
    qint64 published(qint64 id) const {
        const qint64 days = id == 1001 ? 1 : id == 1002 ? 30 : 400;
        return releaseCheckedAt - days * 86400000LL;
    }
    QJsonObject releaseResponse() const {
        return {{"artist", QJsonObject{{"id", 7763}, {"name", "G.E.M.邓紫棋"}}},
                {"checkedAt", releaseCheckedAt},
                {"albums", QJsonArray{albumRow(1003, published(1003)), albumRow(1001, published(1001)),
                                      albumRow(1002, published(1002))}}};
    }
    QJsonObject albumResponse(qint64 id) const {
        auto metadata = albumRow(id, published(id));
        metadata.insert("artistId", 7763); metadata.insert("artistName", "G.E.M.邓紫棋");
        auto track = song(5000000000LL + id, QStringLiteral("曲库新歌 %1").arg(id), QStringLiteral("G.E.M.邓紫棋"));
        track.insert("album", metadata.value("name"));
        return {{"album", metadata}, {"songs", QJsonArray{track}}};
    }
    Fixture() {
        Q_ASSERT(directory.isValid());
        QDir(directory.path()).mkpath("netease");
        QFile token(QDir(directory.path()).filePath("netease/token"));
        if (!token.open(QIODevice::WriteOnly)) qFatal("Cannot create isolated bridge token");
        token.write(localToken); token.close();
        environment.set("MUSICPLAYER_DATA_DIR", directory.path().toUtf8());
        environment.set("MUSICPLAYER_NETEASE_PORT", QByteArray::number(music.server.serverPort()));
        environment.set("MUSICPLAYER_BACKEND_URL", backend.base() + "/app");
        environment.set("MUSICPLAYER_AGENT_API_KEY", "fake-provider-secret-must-stay-server-side");
        environment.set("DASHSCOPE_API_KEY", "fake-provider-secret-must-stay-server-side");
        environment.set("QWEN_API_KEY", "fake-provider-secret-must-stay-server-side");
        backend.handler = [](const FakeServer::Request &request) -> FakeServer::Response {
            const QString path = request.path();
            if (path == "/app/v1/auth/login") {
                const QString name = request.json().value("username").toString();
                return {{{"token", QStringLiteral("app-session-only-%1-123456789").arg(name)},
                         {"user", QJsonObject{{"id", "id-" + name}, {"username", name}}},
                         {"expiresAt", QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODate)}}};
            }
            if (path == "/app/v1/auth/logout") return {{{"ok", true}}};
            if (path == "/app/v1/radio/plan") return {plan()};
            if (path == "/app/v1/assistant/plan") return {assistantPlan()};
            if (path == "/app/v1/radio/select") return {selection()};
            return {{{"error", "unhandled test route"}}, 404};
        };
        music.handler = [this](const FakeServer::Request &request) -> FakeServer::Response {
            const QString path = request.path();
            if (path == "/health") return {{{"app", "quiet-netease"}, {"version", 1}, {"catalogVersion", 2}, {"profileIsolation", 1}}};
            if (!request.headers.contains("X-MusicPlayer-Token: " + localToken))
                return {{{"error", QJsonObject{{"message", "test bridge authorization failed"}}}}, 401};
            if (path == "/assistant/artists")
                return {{{"artists", QJsonArray{artistRow()}}, {"total", 1}}};
            if (path == "/assistant/releases") return {releaseResponse()};
            if (path == "/assistant/album")
                return {albumResponse(request.query().queryItemValue("id").toLongLong())};
            if (path == "/radio/search") {
                const bool first = request.query().queryItemValue("keywords") == QStringLiteral("轻快中文");
                return {{{"playlists", first
                    ? QJsonArray{QJsonObject{{"id", 101}, {"name", "API 来源 A"}}, QJsonObject{{"id", 102}, {"name", "API 备用来源"}}}
                    : QJsonArray{QJsonObject{{"id", 101}, {"name", "重复来源"}}, QJsonObject{{"id", 103}, {"name", "API 来源 B"}}}}}};
            }
            if (path == "/radio/playlist") {
                const auto id = request.query().queryItemValue("id").toLongLong();
                QJsonArray tracks;
                if (id == 101) tracks = {song(11, "API 曲目 A", "Artist A"), song(12, "应排除歌手", "Avoid Artist"),
                    song(13, "已听过的曲目", "Artist C"), song(14, "合唱也应排除", "Guest / aVoId ArTiSt"),
                    song(15, "跨歌单重复曲目", "Artist D"), song(11, "错误的重复标题", "Wrong"), song(0, "无效 ID", "Nobody")};
                else tracks = {song(15, "错误的重复版本", "Wrong"), song(16, "API 曲目 B", "Artist B")};
                return {{{"playlist", QJsonObject{{"id", id}, {"name", id == 101 ? "API 来源 A" : "API 来源 B"}, {"tags", QJsonArray{"夜晚", "流行"}}}},
                         {"songs", tracks}}};
            }
            return {{{"error", "unhandled test route"}}, 404};
        };
    }
    bool login(const QString &name = QStringLiteral("alice")) {
        bool done = false; QString error;
        AppBackendClient::instance()->login(name, "password-for-test", &context, [&](const QString &value) { error = value; done = true; });
        QElapsedTimer timer; timer.start();
        while (!done && timer.elapsed() < 2000) QTest::qWait(5);
        return done && error.isEmpty() && AppBackendClient::instance()->userId() == "id-" + name;
    }
};
}

class RadioClientTest : public QObject {
    Q_OBJECT
private slots:
    void completeFlowUsesRealMetadataAndOnlyBoundedCandidates();
    void unauthenticatedNeverContactsMusicOrModel();
    void longUtf8PlanningBodyFitsEndpointLimit_data();
    void longUtf8PlanningBodyFitsEndpointLimit();
    void maliciousSelectionsAreRejected_data();
    void maliciousSelectionsAreRejected();
    void invalidPlanStopsBeforeMusicSearch_data();
    void invalidPlanStopsBeforeMusicSearch();
    void cancelSuppressesLateCallback();
    void cancelDuringLaterStages_data();
    void cancelDuringLaterStages();
    void destroyedContextSuppressesLateCallback();
    void destroyedClientSuppressesLateCallback();
    void newestGenerationWins();
    void accountSwitchRejectsOldAuthorizedResponse_data();
    void accountSwitchRejectsOldAuthorizedResponse();
    void storePersistsSeparatesAndExplicitlyClears();
    void storeRejectsInvalidWritesWithoutLosingPreference();
    void assistantRadioUsesUnifiedPlanningAndExistingSelection();
    void assistantReleaseMatchesUniqueAliasAndKeepsDatesAndMetadata();
    void assistantArtistAmbiguityRequiresExplicitSelection_data();
    void assistantArtistAmbiguityRequiresExplicitSelection();
    void assistantNonMusicActionsNeverCallMusicOrSelection_data();
    void assistantNonMusicActionsNeverCallMusicOrSelection();
    void assistantUtf8HistoryAndPreferenceFitPlanningLimit();
    void directReleaseRejectsInvalidArtist_data();
    void directReleaseRejectsInvalidArtist();
    void releaseIdentityAndQueryDateAreValidated_data();
    void releaseIdentityAndQueryDateAreValidated();
    void releasesRejectFutureAndInvalidDatesWithoutClaimingNoNewMusic();
    void albumIdentityOrDateMismatchNeverProvidesTracks_data();
    void albumIdentityOrDateMismatchNeverProvidesTracks();
    void releaseRequestsAndTrackCountAreBounded_data();
    void releaseRequestsAndTrackCountAreBounded();
    void releaseLifecycleSuppressesStaleResponses_data();
    void releaseLifecycleSuppressesStaleResponses();
};

void RadioClientTest::completeFlowUsesRealMetadataAndOnlyBoundedCandidates()
{
    Fixture fixture;
    QVERIFY(fixture.login());
    RadioClient client;
    QSignalSpy stages(&client, &RadioClient::stageChanged);
    bool done = false; RadioResult result; QString error;
    // Extra metadata in a model response is untrusted and never replaces API fields.
    fixture.backend.overrides["/app/v1/radio/select"].append({selection(QJsonArray{
        QJsonObject{{"id", 16}, {"reason", "真实候选中的第二首"}, {"title", "模型编造标题"}, {"artist", "模型编造歌手"}, {"coverUrl", "https://invalid.test/invented.jpg"}},
        QJsonObject{{"id", 11}, {"reason", "真实候选中的第一首"}}})});
    QStringList history; for (int i = 0; i < 9; ++i) history.append(QStringLiteral("history-%1").arg(i));
    client.generate(QStringLiteral("下班后想听轻快中文歌"), history, QStringLiteral("少一些现场版"), {13}, &fixture.context,
                    [&](const RadioResult &value, const QString &failure) { result = value; error = failure; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(stages.count(), 6);
    QCOMPARE(fixture.music.count("/radio/search"), 2);
    QCOMPARE(fixture.music.count("/radio/playlist"), 2);
    QCOMPARE(fixture.backend.count("/app/v1/radio/plan"), 1);
    QCOMPARE(fixture.backend.count("/app/v1/radio/select"), 1);
    const auto planRequest = fixture.backend.last("/app/v1/radio/plan");
    QCOMPARE(planRequest.json().value("history").toArray().size(), 4);
    QCOMPARE(planRequest.json().value("history").toArray().first().toString(), QStringLiteral("history-5"));
    QCOMPARE(planRequest.json().value("preference").toString(), QStringLiteral("少一些现场版"));
    QVERIFY(planRequest.headers.contains("Authorization: Bearer app-session-only-alice-123456789"));
    const auto selectRequest = fixture.backend.last("/app/v1/radio/select").json();
    const auto candidates = selectRequest.value("candidates").toArray();
    QCOMPARE(candidates.size(), 3);
    QCOMPARE(candidates.at(0).toObject().value("id").toInt(), 11);
    QCOMPARE(candidates.at(1).toObject().value("id").toInt(), 15);
    QCOMPARE(candidates.at(2).toObject().value("id").toInt(), 16);
    QCOMPARE(candidates.at(1).toObject().value("title").toString(), QStringLiteral("跨歌单重复曲目"));
    QCOMPARE(selectRequest.value("excludedIds").toArray(), QJsonArray{13});
    for (const auto &request : fixture.music.requests) {
        const auto path = request.path();
        if (path == "/radio/search") QCOMPARE(request.query().queryItemValue("limit"), QStringLiteral("5"));
        if (path == "/radio/playlist") QCOMPARE(request.query().queryItemValue("limit"), QStringLiteral("20"));
        QVERIFY(!request.headers.contains("app-session-only"));
        QVERIFY(!request.body.contains("fake-provider-secret"));
        QVERIFY(!request.headers.contains("fake-provider-secret"));
    }
    for (const auto &request : fixture.backend.requests) {
        QVERIFY(!request.headers.contains("fake-provider-secret"));
        QVERIFY(!request.body.contains("fake-provider-secret"));
        QVERIFY(!request.headers.contains(fixture.localToken));
    }
    QCOMPARE(result.tracks.size(), 2);
    QCOMPARE(result.tracks.at(0).song.id, qint64(16));
    QCOMPARE(result.tracks.at(0).song.title, QStringLiteral("API 曲目 B"));
    QCOMPARE(result.tracks.at(0).song.artist, QStringLiteral("Artist B"));
    QCOMPARE(result.tracks.at(0).song.coverUrl, QStringLiteral("https://p1.music.126.net/real-16.jpg"));
    QCOMPARE(result.tracks.at(0).sourceName, QStringLiteral("API 来源 B"));
    QCOMPARE(result.tracks.at(0).sourceTags, QStringList({QStringLiteral("夜晚"), QStringLiteral("流行")}));
    QCOMPARE(result.tracks.at(0).song.durationMs, 188000);
    QCOMPARE(result.tracks.at(0).song.fee, 1);
}

void RadioClientTest::unauthenticatedNeverContactsMusicOrModel()
{
    Fixture fixture;
    RadioClient client;
    bool done = false; QString error;
    client.generate("music", {}, {}, {}, &fixture.context, [&](const RadioResult &, const QString &failure) { done = true; error = failure; });
    QVERIFY(done);
    QVERIFY(error.contains(QStringLiteral("登录")));
    QVERIFY(fixture.backend.requests.isEmpty());
    QVERIFY(fixture.music.requests.isEmpty());
}

void RadioClientTest::longUtf8PlanningBodyFitsEndpointLimit_data()
{
    QTest::addColumn<bool>("includeEmoji");
    QTest::newRow("maximum-length-chinese") << false;
    QTest::newRow("maximum-length-chinese-and-emoji") << true;
}

void RadioClientTest::longUtf8PlanningBodyFitsEndpointLimit()
{
    QFETCH(bool, includeEmoji);
    Fixture fixture; QVERIFY(fixture.login());
    const QString emoji = QString::fromUtf8("🎵");
    const QString instruction = includeEmoji ? QStringLiteral("中文").append(emoji).repeated(250)
                                            : QString(1000, QChar(0x97f3));
    const QString preference = includeEmoji ? QStringLiteral("轻快").append(emoji).repeated(125)
                                           : QString(500, QChar(0x4e50));
    QCOMPARE(instruction.size(), 1000);
    QCOMPARE(preference.size(), 500);
    QStringList history;
    for (int i = 0; i < 4; ++i) {
        const QString prefix = QStringLiteral("历史%1").arg(i);
        const QString tail = includeEmoji ? emoji.repeated(25) : QString();
        history.append(prefix + QString(1000 - prefix.size() - tail.size(), QChar(0x6b4c)) + tail);
    }
    QJsonArray originalHistory;
    for (const auto &message : history) originalHistory.append(message);
    const QJsonObject originalBody{{"instruction", instruction}, {"preference", preference}, {"history", originalHistory}};
    // This is the regression trigger: character limits alone exceed the HTTP
    // body budget once the conversation is serialized as UTF-8.
    QVERIFY(QJsonDocument(originalBody).toJson(QJsonDocument::Compact).size() > 15 * 1024);
    RadioClient client; bool done = false; QString error;
    client.generate(instruction, history, preference, {13}, &fixture.context,
                    [&](const RadioResult &, const QString &failure) { done = true; error = failure; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto request = fixture.backend.last("/app/v1/radio/plan");
    QVERIFY(request.body.size() <= 16 * 1024);
    const auto body = request.json();
    QCOMPARE(body.value("instruction").toString(), instruction);
    QCOMPARE(body.value("preference").toString(), preference);
    const auto retained = body.value("history").toArray();
    QVERIFY(!retained.isEmpty());
    QVERIFY(retained.size() < history.size());
    QVERIFY(retained.size() <= 4);
    for (int i = 0; i < retained.size(); ++i) {
        QVERIFY(retained.at(i).isString());
        const QString message = retained.at(i).toString();
        QCOMPARE(message, history.at(history.size() - retained.size() + i));
        QVERIFY(message.size() <= 1000);
        QCOMPARE(QString::fromUtf8(message.toUtf8()), message);
    }
}

void RadioClientTest::maliciousSelectionsAreRejected_data()
{
    QTest::addColumn<QJsonObject>("response");
    QTest::newRow("unknown-id") << selection(QJsonArray{QJsonObject{{"id", 999999}, {"reason", "invented"}}});
    QTest::newRow("duplicate-id") << selection(QJsonArray{QJsonObject{{"id", 11}, {"reason", "first"}}, QJsonObject{{"id", 11}, {"reason", "again"}}});
    QTest::newRow("excluded-id") << selection(QJsonArray{QJsonObject{{"id", 13}, {"reason", "must not return"}}});
    QTest::newRow("excluded-artist-id") << selection(QJsonArray{QJsonObject{{"id", 12}, {"reason", "must not return"}}});
    QTest::newRow("fractional-id") << selection(QJsonArray{QJsonObject{{"id", 11.5}, {"reason", "invalid id"}}});
    QTest::newRow("empty-reason") << selection(QJsonArray{QJsonObject{{"id", 11}, {"reason", " "}}});
    QTest::newRow("empty-list") << selection(QJsonArray{});
    QTest::newRow("missing-note") << QJsonObject{{"title", "x"}, {"tracks", QJsonArray{QJsonObject{{"id", 11}, {"reason", "ok"}}}}};
}
void RadioClientTest::maliciousSelectionsAreRejected()
{
    QFETCH(QJsonObject, response);
    Fixture fixture; QVERIFY(fixture.login());
    fixture.backend.overrides["/app/v1/radio/select"].append({response});
    RadioClient client; bool done = false; QString error; RadioResult result;
    client.generate("music", {}, {}, {13}, &fixture.context, [&](const RadioResult &value, const QString &failure) { result = value; error = failure; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QVERIFY(!error.isEmpty());
    QVERIFY(result.tracks.isEmpty());
}

void RadioClientTest::invalidPlanStopsBeforeMusicSearch_data()
{
    QTest::addColumn<QJsonObject>("response");
    QTest::newRow("missing-queries") << QJsonObject{{"title", "x"}, {"excludedArtists", QJsonArray{}}};
    QTest::newRow("too-many-queries") << QJsonObject{{"title", "x"}, {"queries", QJsonArray{"a", "b", "c"}}, {"excludedArtists", QJsonArray{}}};
    QTest::newRow("non-string-query") << QJsonObject{{"title", "x"}, {"queries", QJsonArray{12}}, {"excludedArtists", QJsonArray{}}};
    QTest::newRow("invalid-artist") << QJsonObject{{"title", "x"}, {"queries", QJsonArray{"a"}}, {"excludedArtists", QJsonArray{false}}};
}
void RadioClientTest::invalidPlanStopsBeforeMusicSearch()
{
    QFETCH(QJsonObject, response);
    Fixture fixture; QVERIFY(fixture.login());
    fixture.backend.overrides["/app/v1/radio/plan"].append({response});
    RadioClient client; bool done = false; QString error;
    client.generate("music", {}, {}, {}, &fixture.context, [&](const RadioResult &, const QString &failure) { error = failure; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
    QVERIFY(!error.isEmpty());
    QCOMPARE(fixture.music.requests.size(), 0);
    QCOMPARE(fixture.backend.count("/app/v1/radio/select"), 0);
}

void RadioClientTest::cancelSuppressesLateCallback()
{
    Fixture fixture; QVERIFY(fixture.login());
    fixture.backend.overrides["/app/v1/radio/plan"].append({plan(), 200, 150});
    RadioClient client; int callbacks = 0;
    client.generate("music", {}, {}, {}, &fixture.context, [&](const RadioResult &, const QString &) { ++callbacks; });
    QTRY_COMPARE_WITH_TIMEOUT(fixture.backend.count("/app/v1/radio/plan"), 1, 2000);
    client.cancel();
    QTest::qWait(220);
    QCOMPARE(callbacks, 0);
    QCOMPARE(fixture.music.requests.size(), 0);
}

void RadioClientTest::cancelDuringLaterStages_data()
{
    QTest::addColumn<bool>("duringMusic");
    QTest::newRow("local-playlist-response") << true;
    QTest::newRow("model-selection-response") << false;
}

void RadioClientTest::cancelDuringLaterStages()
{
    QFETCH(bool, duringMusic);
    Fixture fixture; QVERIFY(fixture.login());
    if (duringMusic)
        fixture.music.overrides["/radio/playlist"].append({
            {{"playlist", QJsonObject{{"id", 101}, {"name", "delayed API source"}, {"tags", QJsonArray{}}}},
             {"songs", QJsonArray{song(11, "delayed API song", "Artist A")}}}, 200, 150});
    else fixture.backend.overrides["/app/v1/radio/select"].append({selection(), 200, 150});
    RadioClient client; int callbacks = 0;
    client.generate("music", {}, {}, {13}, &fixture.context, [&](const RadioResult &, const QString &) { ++callbacks; });
    if (duringMusic) QTRY_COMPARE_WITH_TIMEOUT(fixture.music.count("/radio/playlist"), 1, 2000);
    else QTRY_COMPARE_WITH_TIMEOUT(fixture.backend.count("/app/v1/radio/select"), 1, 2000);
    client.cancel();
    QTest::qWait(220);
    QCOMPARE(callbacks, 0);
    if (duringMusic) QCOMPARE(fixture.backend.count("/app/v1/radio/select"), 0);
}

void RadioClientTest::destroyedContextSuppressesLateCallback()
{
    Fixture fixture; QVERIFY(fixture.login());
    fixture.backend.overrides["/app/v1/radio/plan"].append({plan(), 200, 150});
    RadioClient client; int callbacks = 0;
    auto *context = new QObject;
    client.generate("music", {}, {}, {}, context, [&](const RadioResult &, const QString &) { ++callbacks; });
    QTRY_COMPARE_WITH_TIMEOUT(fixture.backend.count("/app/v1/radio/plan"), 1, 2000);
    delete context;
    QTest::qWait(220);
    QCOMPARE(callbacks, 0);
    QCOMPARE(fixture.music.requests.size(), 0);
}

void RadioClientTest::destroyedClientSuppressesLateCallback()
{
    Fixture fixture; QVERIFY(fixture.login());
    fixture.backend.overrides["/app/v1/radio/plan"].append({plan(), 200, 150});
    auto *client = new RadioClient; int callbacks = 0;
    client->generate("music", {}, {}, {}, &fixture.context, [&](const RadioResult &, const QString &) { ++callbacks; });
    QTRY_COMPARE_WITH_TIMEOUT(fixture.backend.count("/app/v1/radio/plan"), 1, 2000);
    delete client;
    QTest::qWait(220);
    QCOMPARE(callbacks, 0);
    QCOMPARE(fixture.music.requests.size(), 0);
}

void RadioClientTest::newestGenerationWins()
{
    Fixture fixture; QVERIFY(fixture.login());
    fixture.backend.overrides["/app/v1/radio/plan"].append({plan(), 200, 150});
    RadioClient client; int oldCallbacks = 0, newCallbacks = 0; QString error;
    client.generate("old instruction", {}, {}, {}, &fixture.context, [&](const RadioResult &, const QString &) { ++oldCallbacks; });
    QTRY_COMPARE_WITH_TIMEOUT(fixture.backend.count("/app/v1/radio/plan"), 1, 2000);
    client.generate("new instruction", {}, {}, {13}, &fixture.context, [&](const RadioResult &, const QString &failure) { error = failure; ++newCallbacks; });
    QTRY_COMPARE_WITH_TIMEOUT(newCallbacks, 1, 3000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QTest::qWait(200);
    QCOMPARE(oldCallbacks, 0);
    QCOMPARE(fixture.backend.last("/app/v1/radio/select").json().value("instruction").toString(), QStringLiteral("new instruction"));
}

void RadioClientTest::accountSwitchRejectsOldAuthorizedResponse_data()
{
    QTest::addColumn<QString>("route");
    QTest::newRow("plan") << QStringLiteral("/app/v1/radio/plan");
    QTest::newRow("select") << QStringLiteral("/app/v1/radio/select");
}
void RadioClientTest::accountSwitchRejectsOldAuthorizedResponse()
{
    QFETCH(QString, route);
    Fixture fixture; QVERIFY(fixture.login());
    fixture.backend.overrides[route].append({route.endsWith("plan") ? plan() : selection(), 200, 180});
    RadioClient client; bool done = false; QString error; RadioResult result;
    client.generate("account one music", {}, {}, {13}, &fixture.context, [&](const RadioResult &value, const QString &failure) { result = value; error = failure; done = true; });
    QTRY_COMPARE_WITH_TIMEOUT(fixture.backend.count(route), 1, 2000);
    QVERIFY(fixture.login("bob"));
    QTest::qWait(260);
    QVERIFY(!done);
    QVERIFY(error.isEmpty());
    QVERIFY(result.tracks.isEmpty());
    QCOMPARE(AppBackendClient::instance()->userId(), QStringLiteral("id-bob"));
}

void RadioClientTest::storePersistsSeparatesAndExplicitlyClears()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString path = directory.filePath("radio.sqlite");
    const QString connection = "radio-test-" + QUuid::createUuid().toString();
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(path); QVERIFY(database.open());
        RadioStore store(database); QString error;
        QVERIFY(store.preference("server:alice").isEmpty());
        QVERIFY(store.save("server:alice", QStringLiteral(" 轻快中文，少一点现场版 "), &error));
        QVERIFY(error.isEmpty());
        QVERIFY(store.save("server:bob", QStringLiteral("雨天纯音乐"), &error));
        QCOMPARE(store.preference("server:alice"), QStringLiteral("轻快中文，少一点现场版"));
        QCOMPARE(store.preference("server:bob"), QStringLiteral("雨天纯音乐"));
        QVERIFY(store.preference("guest").isEmpty());
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(path); QVERIFY(database.open());
        RadioStore store(database); QString error;
        QCOMPARE(store.preference("server:alice"), QStringLiteral("轻快中文，少一点现场版"));
        QVERIFY(store.save("server:alice", " \n ", &error));
        QVERIFY(store.preference("server:alice").isEmpty());
        QCOMPARE(store.preference("server:bob"), QStringLiteral("雨天纯音乐"));
        QVERIFY(store.save("server:alice", QStringLiteral("重新记住的口味"), &error));
        QCOMPARE(store.preference("server:alice"), QStringLiteral("重新记住的口味"));
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void RadioClientTest::storeRejectsInvalidWritesWithoutLosingPreference()
{
    const QString connection = "radio-test-" + QUuid::createUuid().toString();
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(":memory:"); QVERIFY(database.open());
        RadioStore store(database); QString error;
        QVERIFY(store.save("alice", "saved", &error));
        QVERIFY(!store.save("alice", QString(501, QLatin1Char('x')), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(store.preference("alice"), QStringLiteral("saved"));
        QVERIFY(!store.save({}, "must not be written", &error));
        QVERIFY(!error.isEmpty());
        database.close();
        QVERIFY(!store.save("alice", "cannot write closed DB", &error));
        QVERIFY(!error.isEmpty());
    }
    QSqlDatabase::removeDatabase(connection);
}

void RadioClientTest::assistantRadioUsesUnifiedPlanningAndExistingSelection()
{
    Fixture fixture; QVERIFY(fixture.login());
    RadioClient client; bool done = false; QString error; RadioResult result;
    client.assist(QStringLiteral("我现在在喝酒，推荐适合慢慢喝的歌曲"),
                  {QStringLiteral("刚才想听华语")}, QStringLiteral("喜欢舒缓人声"), {13}, &fixture.context,
                  [&](const RadioResult &value, const QString &failure) { result = value; error = failure; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(result.kind, QStringLiteral("radio"));
    QCOMPARE(result.tracks.size(), 2);
    QCOMPARE(fixture.backend.count("/app/v1/assistant/plan"), 1);
    QCOMPARE(fixture.backend.count("/app/v1/radio/plan"), 0);
    QCOMPARE(fixture.backend.count("/app/v1/radio/select"), 1);
    QCOMPARE(fixture.music.count("/radio/search"), 2);
    QCOMPARE(fixture.music.count("/assistant/artists"), 0);
    const auto planned = fixture.backend.last("/app/v1/assistant/plan").json();
    const auto selected = fixture.backend.last("/app/v1/radio/select").json();
    QCOMPARE(planned.value("instruction"), selected.value("instruction"));
    QCOMPARE(planned.value("preference").toString(), QStringLiteral("喜欢舒缓人声"));
    QCOMPARE(selected.value("preference"), planned.value("preference"));
    QCOMPARE(selected.value("excludedIds").toArray(), QJsonArray{13});
    QCOMPARE(result.tracks.first().song.title, QStringLiteral("API 曲目 B"));
}

void RadioClientTest::assistantReleaseMatchesUniqueAliasAndKeepsDatesAndMetadata()
{
    Fixture fixture; QVERIFY(fixture.login());
    fixture.backend.overrides["/app/v1/assistant/plan"].append({assistantPlan("releases")});
    RadioClient client; bool done = false; QString error; RadioResult result;
    client.assist(QStringLiteral("最近邓紫棋有没有发新歌"), {}, {}, {}, &fixture.context,
                  [&](const RadioResult &value, const QString &failure) { result = value; error = failure; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(result.kind, QStringLiteral("releases"));
    QCOMPARE(result.tracks.size(), 2);
    QCOMPARE(result.tracks.at(0).song.id, qint64(5000001001LL));
    QCOMPARE(result.tracks.at(1).song.id, qint64(5000001002LL));
    QCOMPARE(result.tracks.at(0).song.title, QStringLiteral("曲库新歌 1001"));
    QCOMPARE(result.tracks.at(0).song.artist, QStringLiteral("G.E.M.邓紫棋"));
    QCOMPARE(result.tracks.at(0).song.coverUrl, QStringLiteral("https://p1.music.126.net/real-5000001001.jpg"));
    QCOMPARE(result.tracks.at(0).sourceName, QStringLiteral("真实专辑 1001"));
    const QString publishedDate = QDateTime::fromMSecsSinceEpoch(fixture.published(1001), Qt::UTC)
        .addSecs(8 * 3600).toString("yyyy-MM-dd");
    QVERIFY(result.tracks.at(0).reason.contains(publishedDate));
    QVERIFY(result.note.contains(QStringLiteral("查询日期")));
    QVERIFY(result.note.contains(QStringLiteral("网易云")));
    QVERIFY(result.note.contains(QStringLiteral("不代表全网")));
    QCOMPARE(fixture.music.count("/assistant/artists"), 1);
    QCOMPARE(fixture.music.last("/assistant/artists").query().queryItemValue("keywords"), QStringLiteral("邓紫棋"));
    QCOMPARE(fixture.music.last("/assistant/releases").query().queryItemValue("artistId"), QStringLiteral("7763"));
    QCOMPARE(fixture.music.last("/assistant/releases").query().queryItemValue("limit"), QStringLiteral("6"));
    QCOMPARE(fixture.music.count("/assistant/album"), 2);
    QCOMPARE(fixture.backend.count("/app/v1/radio/select"), 0);
    QCOMPARE(fixture.music.count("/radio/search"), 0);
    for (const auto &request : fixture.music.requests) {
        QVERIFY(!request.headers.contains("app-session-only"));
        QVERIFY(!request.headers.contains("fake-provider-secret"));
        if (request.path() == "/assistant/album")
            QCOMPARE(request.query().queryItemValue("limit"), QStringLiteral("20"));
    }
}

void RadioClientTest::assistantArtistAmbiguityRequiresExplicitSelection_data()
{
    QTest::addColumn<QJsonArray>("artists");
    QTest::addColumn<int>("expectedChoices");
    QTest::newRow("same-name-multiple-identities")
        << QJsonArray{artistRow(), artistRow(8888, QStringLiteral("邓紫棋"))} << 2;
    auto approximate = artistRow(9999, QStringLiteral("邓紫棋作品翻唱"));
    approximate.insert("aliases", QJsonArray{});
    QTest::newRow("one-result-without-exact-name-or-alias") << QJsonArray{approximate} << 1;
    QTest::newRow("no-matching-artists") << QJsonArray{} << 0;
}

void RadioClientTest::assistantArtistAmbiguityRequiresExplicitSelection()
{
    QFETCH(QJsonArray, artists); QFETCH(int, expectedChoices);
    Fixture fixture; QVERIFY(fixture.login());
    fixture.backend.overrides["/app/v1/assistant/plan"].append({assistantPlan("releases")});
    fixture.music.overrides["/assistant/artists"].append({{{"artists", artists}, {"total", artists.size()}}});
    RadioClient client; bool done = false; QString error; RadioResult result;
    client.assist(QStringLiteral("查一下邓紫棋的新歌"), {}, {}, {}, &fixture.context,
                  [&](const RadioResult &value, const QString &failure) { result = value; error = failure; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(result.kind, QStringLiteral("artists"));
    QCOMPARE(result.artists.size(), expectedChoices);
    QVERIFY(result.tracks.isEmpty());
    QCOMPARE(fixture.music.count("/assistant/releases"), 0);
    QCOMPARE(fixture.music.count("/assistant/album"), 0);
    QCOMPARE(fixture.backend.count("/app/v1/radio/select"), 0);
    if (!result.artists.isEmpty()) {
        bool selected = false;
        client.releasesForArtist(result.artists.first(), &fixture.context,
                                 [&](const RadioResult &value, const QString &failure) { result = value; error = failure; selected = true; });
        QTRY_VERIFY_WITH_TIMEOUT(selected, 2000);
        QCOMPARE(fixture.music.count("/assistant/releases"), 1);
        QCOMPARE(fixture.backend.count("/app/v1/assistant/plan"), 1);
    }
}

void RadioClientTest::assistantNonMusicActionsNeverCallMusicOrSelection_data()
{
    QTest::addColumn<QString>("kind");
    QTest::newRow("explicit-remember") << QStringLiteral("remember");
    QTest::newRow("explicit-forget") << QStringLiteral("forget");
    QTest::newRow("ask-for-clarification") << QStringLiteral("clarify");
}

void RadioClientTest::assistantNonMusicActionsNeverCallMusicOrSelection()
{
    QFETCH(QString, kind);
    Fixture fixture; QVERIFY(fixture.login());
    fixture.backend.overrides["/app/v1/assistant/plan"].append({assistantPlan(kind)});
    RadioClient client; bool done = false; QString error; RadioResult result;
    client.assist(QStringLiteral("记住我喜欢舒缓华语，少一些现场版"), {}, QStringLiteral("旧口味"), {}, &fixture.context,
                  [&](const RadioResult &value, const QString &failure) { result = value; error = failure; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(result.kind, kind);
    QVERIFY(!result.note.isEmpty());
    if (kind == QStringLiteral("remember")) QCOMPARE(result.preference, QStringLiteral("喜欢舒缓华语，少一些现场版"));
    else QVERIFY(result.preference.isEmpty());
    QVERIFY(result.tracks.isEmpty());
    QVERIFY(fixture.music.requests.isEmpty());
    QCOMPARE(fixture.backend.count("/app/v1/radio/select"), 0);
}

void RadioClientTest::assistantUtf8HistoryAndPreferenceFitPlanningLimit()
{
    Fixture fixture; QVERIFY(fixture.login());
    fixture.backend.overrides["/app/v1/assistant/plan"].append({assistantPlan("remember")});
    const QString emoji = QString::fromUtf8("🎵");
    const QString instruction = QStringLiteral("中文").append(emoji).repeated(250);
    const QString preference = QStringLiteral("轻快").append(emoji).repeated(125);
    QStringList history;
    for (int i = 0; i < 7; ++i) history.append(QStringLiteral("历史%1").arg(i) + QString(947, QChar(0x6b4c)) + emoji.repeated(25));
    RadioClient client; bool done = false; QString error;
    client.assist(instruction, history, preference, {}, &fixture.context,
                  [&](const RadioResult &, const QString &failure) { error = failure; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto request = fixture.backend.last("/app/v1/assistant/plan");
    QVERIFY(request.body.size() <= 16 * 1024);
    QCOMPARE(request.json().value("instruction").toString(), instruction);
    QCOMPARE(request.json().value("preference").toString(), preference);
    const auto recent = request.json().value("history").toArray();
    QVERIFY(!recent.isEmpty()); QVERIFY(recent.size() < 4);
    for (int i = 0; i < recent.size(); ++i) {
        const auto retained = recent.at(i).toString();
        QCOMPARE(retained, history.at(history.size() - recent.size() + i));
        QCOMPARE(QString::fromUtf8(retained.toUtf8()), retained);
    }
    QVERIFY(fixture.music.requests.isEmpty());
}

void RadioClientTest::directReleaseRejectsInvalidArtist_data()
{
    QTest::addColumn<qint64>("id"); QTest::addColumn<QString>("name");
    QTest::newRow("zero") << qint64(0) << QStringLiteral("歌手");
    QTest::newRow("negative") << qint64(-1) << QStringLiteral("歌手");
    QTest::newRow("unsafe-integer") << qint64(9007199254740992LL) << QStringLiteral("歌手");
    QTest::newRow("blank-name") << qint64(7763) << QStringLiteral(" ");
    QTest::newRow("oversized-name") << qint64(7763) << QString(121, QChar(0x6b4c));
}

void RadioClientTest::directReleaseRejectsInvalidArtist()
{
    QFETCH(qint64, id); QFETCH(QString, name);
    Fixture fixture; QVERIFY(fixture.login());
    AssistantArtist artist = selectedArtist(id); artist.name = name;
    RadioClient client; bool done = false; QString error;
    client.releasesForArtist(artist, &fixture.context,
                             [&](const RadioResult &, const QString &failure) { error = failure; done = true; });
    QVERIFY(done); QVERIFY(!error.isEmpty());
    QVERIFY(fixture.music.requests.isEmpty());
    QCOMPARE(fixture.backend.count("/app/v1/assistant/plan"), 0);
}

void RadioClientTest::releaseIdentityAndQueryDateAreValidated_data()
{
    QTest::addColumn<int>("invalidCase");
    QTest::newRow("wrong-artist") << 0;
    QTest::newRow("string-artist-id") << 1;
    QTest::newRow("missing-checked-at") << 2;
    QTest::newRow("checked-before-year-2000") << 3;
    QTest::newRow("checked-in-future") << 4;
    QTest::newRow("stale-checked-at") << 5;
}

void RadioClientTest::releaseIdentityAndQueryDateAreValidated()
{
    QFETCH(int, invalidCase);
    Fixture fixture; QVERIFY(fixture.login());
    auto response = fixture.releaseResponse();
    if (invalidCase == 0) response.insert("artist", QJsonObject{{"id", 9999}, {"name", "Wrong"}});
    if (invalidCase == 1) response.insert("artist", QJsonObject{{"id", "7763"}, {"name", "Wrong"}});
    if (invalidCase == 2) response.remove("checkedAt");
    if (invalidCase == 3) response.insert("checkedAt", 946684799999LL);
    if (invalidCase == 4) response.insert("checkedAt", fixture.releaseCheckedAt + 600000LL);
    if (invalidCase == 5) response.insert("checkedAt", fixture.releaseCheckedAt - 360000LL);
    fixture.music.overrides["/assistant/releases"].append({response});
    RadioClient client; bool done = false; QString error; RadioResult result;
    client.releasesForArtist(selectedArtist(), &fixture.context,
                             [&](const RadioResult &value, const QString &failure) { result = value; error = failure; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
    QVERIFY(!error.isEmpty()); QVERIFY(result.tracks.isEmpty());
    QCOMPARE(fixture.music.count("/assistant/album"), 0);
}

void RadioClientTest::releasesRejectFutureAndInvalidDatesWithoutClaimingNoNewMusic()
{
    Fixture fixture; QVERIFY(fixture.login());
    auto response = fixture.releaseResponse();
    auto stringDate = albumRow(2003, fixture.published(1001)); stringDate.insert("publishTime", "1767225600000");
    response.insert("albums", QJsonArray{albumRow(2001, fixture.releaseCheckedAt + 86400000LL),
                                       albumRow(2002, 0), stringDate});
    fixture.music.overrides["/assistant/releases"].append({response});
    RadioClient client; bool done = false; QString error; RadioResult result;
    client.releasesForArtist(selectedArtist(), &fixture.context,
                             [&](const RadioResult &value, const QString &failure) { result = value; error = failure; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(result.tracks.isEmpty());
    QCOMPARE(fixture.music.count("/assistant/album"), 0);
    QVERIFY(result.note.contains(QStringLiteral("暂时无法确认")));
    QVERIFY(result.note.contains(QStringLiteral("不代表全网")));
}

void RadioClientTest::albumIdentityOrDateMismatchNeverProvidesTracks_data()
{
    QTest::addColumn<QString>("field");
    QTest::addColumn<QJsonValue>("wrongValue");
    QTest::newRow("wrong-album-id") << QStringLiteral("id") << QJsonValue(9009);
    QTest::newRow("wrong-main-artist") << QStringLiteral("artistId") << QJsonValue(8888);
    QTest::newRow("changed-publish-date") << QStringLiteral("publishTime") << QJsonValue(946684800000LL);
    QTest::newRow("future-publish-date") << QStringLiteral("publishTime") << QJsonValue(QDateTime::currentMSecsSinceEpoch() + 86400000LL);
    QTest::newRow("string-id") << QStringLiteral("id") << QJsonValue(QStringLiteral("1001"));
}

void RadioClientTest::albumIdentityOrDateMismatchNeverProvidesTracks()
{
    QFETCH(QString, field); QFETCH(QJsonValue, wrongValue);
    Fixture fixture; QVERIFY(fixture.login());
    auto releases = fixture.releaseResponse(); releases.insert("albums", QJsonArray{albumRow(1001, fixture.published(1001))});
    fixture.music.overrides["/assistant/releases"].append({releases});
    auto detail = fixture.albumResponse(1001); auto metadata = detail.value("album").toObject();
    metadata.insert(field, wrongValue); detail.insert("album", metadata);
    fixture.music.overrides["/assistant/album"].append({detail});
    RadioClient client; bool done = false; RadioResult result;
    client.releasesForArtist(selectedArtist(), &fixture.context,
                             [&](const RadioResult &value, const QString &) { result = value; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
    QVERIFY(result.tracks.isEmpty());
    QCOMPARE(fixture.music.count("/assistant/album"), 1);
    QCOMPARE(fixture.backend.count("/app/v1/radio/select"), 0);
}

void RadioClientTest::releaseRequestsAndTrackCountAreBounded_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("stop-after-two-successful-albums") << 0;
    QTest::newRow("at-most-three-album-attempts") << 1;
    QTest::newRow("at-most-twelve-real-tracks") << 2;
}

void RadioClientTest::releaseRequestsAndTrackCountAreBounded()
{
    QFETCH(int, scenario);
    Fixture fixture; QVERIFY(fixture.login());
    auto releases = fixture.releaseResponse();
    auto albums = releases.value("albums").toArray();
    albums.append(albumRow(1004, fixture.published(1003) - 86400000LL));
    releases.insert("albums", albums);
    fixture.music.overrides["/assistant/releases"].append({releases});
    if (scenario == 1) {
        fixture.music.overrides["/assistant/album"].append({{{"error", QJsonObject{{"message", "temporary metadata failure"}}}}, 502});
    } else if (scenario == 2) {
        auto detail = fixture.albumResponse(1001); QJsonArray songs;
        for (int i = 0; i < 40; ++i) songs.append(song(6000000000LL + i, QStringLiteral("真实曲目 %1").arg(i), QStringLiteral("G.E.M.邓紫棋")));
        detail.insert("songs", songs); fixture.music.overrides["/assistant/album"].append({detail});
    }
    RadioClient client; bool done = false; QString error; RadioResult result;
    client.releasesForArtist(selectedArtist(), &fixture.context,
                             [&](const RadioResult &value, const QString &failure) { result = value; error = failure; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(fixture.music.count("/assistant/album"), scenario == 0 ? 2 : scenario == 1 ? 3 : 1);
    QCOMPARE(result.tracks.size(), scenario == 2 ? 12 : 2);
    QCOMPARE(fixture.backend.count("/app/v1/assistant/plan"), 0);
    QCOMPARE(fixture.backend.count("/app/v1/radio/select"), 0);
}

void RadioClientTest::releaseLifecycleSuppressesStaleResponses_data()
{
    QTest::addColumn<QString>("route"); QTest::addColumn<int>("action");
    for (const auto &stage : QStringList{QStringLiteral("releases"), QStringLiteral("album")}) {
        QTest::newRow(qPrintable(stage + "-cancel")) << "/assistant/" + stage << 0;
        QTest::newRow(qPrintable(stage + "-context-destroyed")) << "/assistant/" + stage << 1;
        QTest::newRow(qPrintable(stage + "-account-switched")) << "/assistant/" + stage << 2;
    }
}

void RadioClientTest::releaseLifecycleSuppressesStaleResponses()
{
    QFETCH(QString, route); QFETCH(int, action);
    Fixture fixture; QVERIFY(fixture.login());
    fixture.music.overrides[route].append({route.endsWith("releases") ? fixture.releaseResponse() : fixture.albumResponse(1001), 200, 180});
    RadioClient client; int callbacks = 0; auto *context = new QObject;
    client.releasesForArtist(selectedArtist(), context,
                             [&](const RadioResult &, const QString &) { ++callbacks; });
    QTRY_COMPARE_WITH_TIMEOUT(fixture.music.count(route), 1, 2000);
    const int albumRequestsBefore = fixture.music.count("/assistant/album");
    if (action == 0) client.cancel();
    else if (action == 1) { delete context; context = nullptr; }
    else QVERIFY(fixture.login("bob"));
    QTest::qWait(260);
    QCOMPARE(callbacks, 0);
    QCOMPARE(fixture.music.count("/assistant/album"), albumRequestsBefore);
    QCOMPARE(fixture.backend.count("/app/v1/radio/select"), 0);
    delete context;
}

QTEST_GUILESS_MAIN(RadioClientTest)
#include "radioclient_test.moc"
