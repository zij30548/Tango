#include "appbackendclient.h"
#include "mainwindow.h"
#include "radiodialog.h"
#include "radiostore.h"
#include "songlistdelegate.h"

#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <functional>
#include <memory>

namespace {
class ScopedEnvironment {
public:
    ~ScopedEnvironment() {
        for (auto value = saved.crbegin(); value != saved.crend(); ++value)
            if (value->existed) qputenv(value->name.constData(), value->value);
            else qunsetenv(value->name.constData());
    }
    void set(const QByteArray &name, const QByteArray &value) {
        saved.append({name, qgetenv(name.constData()), qEnvironmentVariableIsSet(name.constData())});
        qputenv(name.constData(), value);
    }
private:
    struct Value { QByteArray name, value; bool existed; };
    QVector<Value> saved;
};

// Both servers are owned loopback fixtures. No model provider, public music
// service, real user account, cookie or credentials are contacted by this suite.
class FakeServer : public QObject {
public:
    struct Request {
        QString path;
        QByteArray headers, bytes;
        QJsonObject json() const { return QJsonDocument::fromJson(bytes).object(); }
    };
    QTcpServer server;
    QVector<Request> requests;
    std::function<QJsonObject(const Request &)> handler;
    FakeServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                auto buffer = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, handled] {
                    buffer->append(socket->readAll());
                    if (*handled) return;
                    const int end = buffer->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray headers = buffer->left(end);
                    int length = 0;
                    for (const auto &line : headers.split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    if (buffer->size() < end + 4 + length) return;
                    *handled = true;
                    const auto target = headers.split(' ').value(1);
                    const Request request{QUrl::fromEncoded(target).path(), headers,
                                          buffer->mid(end + 4, length)};
                    requests.append(request);
                    const QByteArray payload = QJsonDocument(handler ? handler(request) : QJsonObject{})
                                                   .toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                  + QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QByteArray base() const { return "http://127.0.0.1:" + QByteArray::number(server.serverPort()); }
    int count(const QString &path) const {
        int count = 0;
        for (const auto &request : requests) if (request.path == path) ++count;
        return count;
    }
};

struct Fixture {
    ScopedEnvironment environment;
    QTemporaryDir data;
    FakeServer backend, music;
    QObject context;
    const QByteArray token = "owned-integration-bridge-token";
    const QString proposal = QStringLiteral("偏爱华语 R&B，少推荐摇滚");
    Fixture() {
        Q_ASSERT(data.isValid());
        QDir(data.path()).mkpath("netease");
        QFile tokenFile(data.filePath("netease/token"));
        if (!tokenFile.open(QIODevice::WriteOnly)) qFatal("Cannot create isolated test token");
        tokenFile.write(token);
        tokenFile.close();
        environment.set("MUSICPLAYER_DATA_DIR", data.path().toUtf8());
        environment.set("MUSICPLAYER_BACKEND_URL", backend.base());
        environment.set("MUSICPLAYER_NETEASE_PORT", QByteArray::number(music.server.serverPort()));
        environment.set("MUSICPLAYER_REDIS_PORT", "invalid-test-port");
        environment.set("QWEN_API_KEY", "fake-provider-secret-not-for-desktop");
        backend.handler = [this](const FakeServer::Request &request) -> QJsonObject {
            if (request.path == "/v1/auth/login") {
                const QString name = request.json().value("username").toString();
                return {{"token", "owned-session-for-" + name + "-123456789"},
                        {"user", QJsonObject{{"id", "owned-id-" + name}, {"username", name}}},
                        {"expiresAt", QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODate)}};
            }
            if (request.path == "/v1/auth/logout") return {{"ok", true}};
            if (request.path == "/v1/agent/status") return {{"configured", true}, {"model", "owned-fixture"}};
            if (request.path == "/v1/assistant/plan") {
                const QString instruction = request.json().value("instruction").toString();
                if (instruction.contains(QStringLiteral("清除")))
                    return {{"kind", "forget"}, {"note", "review required"}};
                if (instruction.contains(QStringLiteral("记住")))
                    return {{"kind", "remember"}, {"note", "review required"}, {"preference", proposal}};
                return {{"kind", "radio"}, {"title", "微醺时刻"}, {"note", "search actual playlists"},
                        {"queries", QJsonArray{QStringLiteral("微醺 华语 R&B")}}, {"excludedArtists", QJsonArray{}}};
            }
            if (request.path == "/v1/radio/select")
                return {{"title", "慢一点的音乐"}, {"note", "参考歌单主题与曲目信息"},
                        {"tracks", QJsonArray{QJsonObject{{"id", 42}, {"reason", "来自微醺主题歌单"}}}}};
            return {{"error", "unexpected owned-backend route"}};
        };
        music.handler = [this](const FakeServer::Request &request) -> QJsonObject {
            if (request.path == "/health") return {{"app", "quiet-netease"}, {"version", 1}, {"catalogVersion", 2}, {"profileIsolation", 1}};
            if (!request.headers.contains("X-MusicPlayer-Token: " + token))
                return {{"error", QJsonObject{{"message", "test bridge token missing"}}}};
            if (request.path == "/radio/search")
                return {{"playlists", QJsonArray{QJsonObject{{"id", 123}, {"name", "微醺华语"}}}}};
            if (request.path == "/radio/playlist")
                return {{"playlist", QJsonObject{{"id", 123}, {"name", "微醺华语"}, {"tags", QJsonArray{"华语"}}}},
                        {"songs", QJsonArray{QJsonObject{{"id", 42}, {"title", "曲库真实标题"},
                            {"artist", "曲库歌手"}, {"album", "曲库专辑"}, {"coverUrl", ""},
                            {"durationMs", 200000}, {"fee", 0}}}}};
            return {{"error", QJsonObject{{"message", "unexpected owned-music route"}}}};
        };
    }
    ~Fixture() { AppBackendClient::instance()->logout(&context); }
    bool login(MainWindow &window, const QString &name = QStringLiteral("alice")) {
        bool done = false;
        QString error;
        auto *client = AppBackendClient::instance();
        client->login(name, "owned-fixture-password", &window,
                      [&](const QString &value) { error = value; done = true; });
        QElapsedTimer timer;
        timer.start();
        while (!done && timer.elapsed() < 2000) QTest::qWait(5);
        if (!done || !error.isEmpty()) return false;
        done = false;
        client->refreshAgentStatus(&window, [&](const QString &value) { error = value; done = true; });
        timer.restart();
        while (!done && timer.elapsed() < 2000) QTest::qWait(5);
        return done && error.isEmpty() && client->authenticated() && client->agentReady();
    }
};

bool openAssistant(MainWindow &window, const std::function<void(RadioDialog *)> &drive)
{
    QPointer<RadioDialog> dialog;
    QTimer timer, timeout;
    bool expired = false;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, &window, [&] {
        if (!dialog) dialog = window.findChild<RadioDialog *>();
        if (dialog) drive(dialog);
    });
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &window, [&] {
        expired = true;
        for (auto *editor : window.findChildren<QInputDialog *>()) editor->reject();
        if (dialog) dialog->reject();
    });
    window.show();
    timer.start();
    timeout.start(7000);
    const bool invoked = QMetaObject::invokeMethod(&window, "openRadio", Qt::DirectConnection);
    dialog = window.findChild<RadioDialog *>();
    // The assistant is a normal center page now; keep driving its asynchronous
    // workflow until the test explicitly returns to the library.
    while (invoked && dialog && dialog->isVisible() && !expired) QTest::qWait(5);
    timer.stop();
    timeout.stop();
    return invoked && !expired;
}

bool submit(RadioDialog *dialog, const QString &request)
{
    auto *input = dialog->findChild<QPlainTextEdit *>("edit_RadioInstruction");
    if (!input || !input->isEnabled()) return false;
    input->setPlainText(request);
    dialog->findChild<QPushButton *>("button_RadioGenerate")->click();
    return true;
}

QByteArray wave()
{
    constexpr quint32 rate = 22050, size = rate * 20 * sizeof(qint16);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + size);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << rate << quint32(rate * sizeof(qint16))
           << quint16(sizeof(qint16)) << quint16(16);
    stream.writeRawData("data", 4);
    stream << size;
    bytes.append(QByteArray(size, '\0'));
    return bytes;
}
int historyCount()
{
    QSqlQuery query;
    return query.exec("SELECT COUNT(*) FROM play_history") && query.next() ? query.value(0).toInt() : -1;
}
}

class AssistantFlowTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void memoryProposalRequiresReviewBeforeSavingAndCanBeCleared();
    void changingAccountWhileReviewOpenCannotSaveOldProposal();
    void scenePreviewPreservesPlaybackQueueHistoryAndSavedTaste();
    void cleanup();
};

void AssistantFlowTest::memoryProposalRequiresReviewBeforeSavingAndCanBeCleared()
{
    Fixture fixture;
    MainWindow window;
    QVERIFY(fixture.login(window));
    const QString profile = window.accountProfile();
    const QString old = QStringLiteral("原来喜欢的轻音乐");
    const QString approved = QStringLiteral("确认喜欢华语 R&B，少推荐摇滚");
    QVERIFY(RadioStore(QSqlDatabase::database()).save(profile, old));
    int phase = 0;
    QStringList errors;
    auto expect = [&](bool value, const QString &message) { if (!value) errors.append(message); };
    QVERIFY(openAssistant(window, [&](RadioDialog *dialog) {
        auto *editor = dialog->findChild<QInputDialog *>("dialog_RadioPreferenceEditor");
        const auto saved = [&] { return RadioStore(QSqlDatabase::database()).preference(profile); };
        if (phase == 0 && submit(dialog, QStringLiteral("记住我喜欢华语 R&B"))) { phase = 1; return; }
        if (phase == 1 && editor && editor->isVisible()) {
            expect(saved() == old, "model proposal was saved before review");
            expect(editor->textValue() == fixture.proposal, "model proposal not shown for review");
            editor->reject(); phase = 2; return;
        }
        if (phase == 2 && !editor) {
            expect(saved() == old, "cancelling review changed saved preference");
            if (submit(dialog, QStringLiteral("记住我喜欢华语 R&B"))) phase = 3;
            return;
        }
        if (phase == 3 && editor && editor->isVisible()) {
            expect(saved() == old, "second proposal was saved before review");
            editor->setTextValue(approved);
            editor->accept(); phase = 4; return;
        }
        if (phase == 4 && !editor) {
            expect(saved() == approved, "confirmed edited preference did not persist");
            if (submit(dialog, QStringLiteral("清除记住的音乐口味"))) phase = 5;
            return;
        }
        if (phase == 5 && editor && editor->isVisible()) {
            expect(saved() == approved, "forget request cleared preference before confirmation");
            expect(editor->textValue().isEmpty(), "forget review was not empty");
            editor->accept(); phase = 6; return;
        }
        if (phase == 6 && !editor) {
            expect(saved().isEmpty(), "confirmed empty review did not clear preference");
            phase = 7;
            dialog->accept();
        }
    }));
    QCOMPARE(phase, 7);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    QCOMPARE(fixture.backend.count("/v1/assistant/plan"), 3);
    QCOMPARE(fixture.backend.count("/v1/radio/select"), 0);
    QCOMPARE(fixture.music.requests.size(), 0);
}

void AssistantFlowTest::changingAccountWhileReviewOpenCannotSaveOldProposal()
{
    Fixture fixture;
    MainWindow window;
    QVERIFY(fixture.login(window));
    const QString aliceProfile = window.accountProfile();
    const QString aliceTaste = QStringLiteral("只属于 Alice 的轻音乐口味");
    QVERIFY(RadioStore(QSqlDatabase::database()).save(aliceProfile, aliceTaste));
    QString bobProfile, authError;
    bool loginDone = false;
    int phase = 0;
    QPointer<QInputDialog> oldEditor;
    std::unique_ptr<QSignalSpy> edited;
    QStringList errors;
    auto expect = [&](bool value, const QString &message) { if (!value) errors.append(message); };
    QVERIFY(openAssistant(window, [&](RadioDialog *dialog) {
        if (!edited) edited.reset(new QSignalSpy(dialog, &RadioDialog::preferenceEdited));
        if (phase == 0 && submit(dialog, QStringLiteral("记住我喜欢华语 R&B"))) { phase = 1; return; }
        if (phase == 1) {
            oldEditor = dialog->findChild<QInputDialog *>("dialog_RadioPreferenceEditor");
            if (!oldEditor || !oldEditor->isVisible()) return;
            AppBackendClient::instance()->login("bob", "owned-fixture-password", &window,
                [&](const QString &error) { authError = error; loginDone = true; });
            phase = 2; return;
        }
        if (phase == 2 && loginDone) {
            bobProfile = window.accountProfile();
            expect(authError.isEmpty(), "switch to Bob failed");
            expect(bobProfile != aliceProfile, "account profile did not change");
            expect(RadioStore(QSqlDatabase::database()).preference(bobProfile).isEmpty(), "Bob received Alice taste");
            expect(dialog->findChild<QPlainTextEdit *>("text_RadioConversation")->toPlainText().isEmpty(),
                   "old account conversation survived account reset");
            oldEditor->accept(); phase = 3; return;
        }
        if (phase == 3 && !oldEditor) {
            expect(edited->isEmpty(), "old editor emitted a save after account switch");
            expect(RadioStore(QSqlDatabase::database()).preference(bobProfile).isEmpty(), "old proposal was saved into Bob");
            loginDone = false;
            AppBackendClient::instance()->login("alice", "owned-fixture-password", &window,
                [&](const QString &error) { authError = error; loginDone = true; });
            phase = 4; return;
        }
        if (phase == 4 && loginDone) {
            expect(authError.isEmpty(), "return to Alice failed");
            expect(window.accountProfile() == aliceProfile, "Alice profile was not restored");
            expect(RadioStore(QSqlDatabase::database()).preference(aliceProfile) == aliceTaste,
                   "Alice preference changed without confirmation");
            phase = 5; dialog->accept();
        }
    }));
    QCOMPARE(phase, 5);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    QCOMPARE(fixture.backend.count("/v1/assistant/plan"), 1);
    QCOMPARE(fixture.music.requests.size(), 0);
}

void AssistantFlowTest::scenePreviewPreservesPlaybackQueueHistoryAndSavedTaste()
{
    Fixture fixture;
    MainWindow window;
    window.p_PlayerObject->setMuted(true);
    QVERIFY(fixture.login(window));
    const QString profile = window.accountProfile();
    const QString preference = QStringLiteral("喜欢华语 R&B");
    QVERIFY(RadioStore(QSqlDatabase::database()).save(profile, preference));
    QFile file(fixture.data.filePath("owned-playing-fixture.wav"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray bytes = wave();
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
    file.close();
    window.addLocalFiles({file.fileName()});
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    const QUrl media = window.p_PlayerObject->currentMedia().request().url();
    const int historyBefore = historyCount();
    QVERIFY(historyBefore > 0);
    int phase = 0;
    bool retainedPlaying = false, metadataFromService = false;
    QVERIFY(openAssistant(window, [&](RadioDialog *dialog) {
        if (phase == 0 && submit(dialog, QStringLiteral("我现在在喝酒，推荐一些适合的歌"))) { phase = 1; return; }
        auto *table = dialog->findChild<QTableWidget *>("table_RadioRecommendations");
        if (phase == 1 && table->rowCount() == 1) {
            retainedPlaying = window.p_PlayerObject->state() == QMediaPlayer::PlayingState &&
                              window.p_PlayerObject->currentMedia().request().url() == media &&
                              window.p_PlayerList->mediaCount() == 1 && historyCount() == historyBefore;
            metadataFromService = table->item(0, 0)->text() == QStringLiteral("曲库真实标题") &&
                                  table->item(0, 0)->data(SongListDelegate::ArtistRole).toString() == QStringLiteral("曲库歌手");
            phase = 2; dialog->accept();
        }
    }));
    QCOMPARE(phase, 2);
    QVERIFY(retainedPlaying);
    QVERIFY(metadataFromService);
    QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile), preference);
    QCOMPARE(fixture.backend.count("/v1/assistant/plan"), 1);
    QCOMPARE(fixture.backend.count("/v1/radio/select"), 1);
    QCOMPARE(fixture.music.count("/song/url"), 0);
    for (const auto &request : fixture.backend.requests) {
        QVERIFY(!request.headers.contains("fake-provider-secret-not-for-desktop"));
        QVERIFY(!request.bytes.contains("fake-provider-secret-not-for-desktop"));
        if (request.path == "/v1/assistant/plan") QCOMPARE(request.json().value("preference").toString(), preference);
    }
    bool reopened = false;
    QVERIFY(openAssistant(window, [&](RadioDialog *dialog) {
        reopened = dialog->recommendations().size() == 1;
        dialog->accept();
    }));
    QVERIFY(reopened);
    QVERIFY(fixture.login(window, QStringLiteral("bob")));
    QVERIFY(RadioStore(QSqlDatabase::database()).preference(window.accountProfile()).isEmpty());
    bool clearedForBob = false;
    QVERIFY(openAssistant(window, [&](RadioDialog *dialog) {
        clearedForBob = dialog->recommendations().isEmpty() &&
            dialog->findChild<QPlainTextEdit *>("text_RadioConversation")->toPlainText().isEmpty();
        dialog->accept();
    }));
    QVERIFY(clearedForBob);
    QCOMPARE(historyCount(), 0);
}

void AssistantFlowTest::cleanup()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (QSqlDatabase::contains()) {
        QSqlDatabase::database(QSqlDatabase::defaultConnection, false).close();
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }
}

QTEST_MAIN(AssistantFlowTest)
#include "assistant_flow_test.moc"
