#include "appbackendclient.h"
#include "focusdialog.h"
#include "focussession.h"
#include "historysong.h"
#include "likesong.h"
#include "neteasedialogs.h"
#include "neteaseclient.h"
#include "playerwindowview.h"
#include <QLabel>
#include <QStackedWidget>
#include <QTableView>
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
    QHash<QString, int> delays;
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
                    const QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(delays.value(request.path), socket, [guard, payload] {
                        if (!guard || guard->state() != QAbstractSocket::ConnectedState) return;
                        guard->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                     + QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
                        guard->disconnectFromHost();
                    });
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
            if (request.path == "/v1/focus/parse")
                return {{"request", QJsonObject{{"minutes", 1}, {"scene", "rest"}, {"source", "queue"},
                    {"excludeLive", false}, {"instrumentalOnly", false}, {"excludedArtists", QJsonArray{}}}}};
            if (request.path == "/v1/radio/select")
                return {{"title", "慢一点的音乐"}, {"note", "参考歌单主题与曲目信息"},
                        {"tracks", QJsonArray{QJsonObject{{"id", 42}, {"reason", "来自微醺主题歌单"}}}}};
            return {{"error", "unexpected owned-backend route"}};
        };
        music.handler = [this](const FakeServer::Request &request) -> QJsonObject {
            if (request.path == "/health") return {{"app", "quiet-netease"}, {"version", 1}, {"catalogVersion", 2}, {"profileIsolation", 1}};
            if (!request.headers.contains("X-MusicPlayer-Token: " + token))
                return {{"error", QJsonObject{{"message", "test bridge token missing"}}}};
            if (request.path == "/search")
                return {{"songs", QJsonArray{QJsonObject{{"id", 84}, {"title", "在线搜索歌曲"},
                    {"artist", "搜索歌手"}, {"album", "搜索专辑"}, {"durationMs", 180000}, {"fee", 0}}}}, {"total", 1}};
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

class NavigationTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void widgetContextDestructionDoesNotDeliverAbortedStatus();
    void widgetContextDestructionAbortsMusicRequestWithoutCallback();
    void commonPagesStayEmbeddedAndAreReused();
    void miniPlayerOpensDetailsAndNavigationPreservesPlayback();
    void leavingAssistantCancelsPendingWorkAndKeepsSuccessfulResults();
    void leavingFocusCancelsPendingParseAndKeepsActiveSession();
    void searchNavigationRejectsAnOlderResponse();
    void accountSwitchClearsEveryCachedPrivatePage();
    void cleanup();
};

namespace {
QPushButton *nav(MainWindow &window, const char *name)
{
    return window.findChild<QPushButton *>(QString::fromLatin1(name));
}
QStackedWidget *pages(MainWindow &window)
{
    return window.findChild<QStackedWidget *>(QStringLiteral("pageStack"));
}
int visibleTopLevelWindows()
{
    int count = 0;
    for (const auto *widget : QApplication::topLevelWidgets())
        if (widget->isVisible() && widget->windowType() != Qt::ToolTip) ++count;
    return count;
}
QString createPlayingFixture(Fixture &fixture)
{
    const QString path = fixture.data.filePath(QStringLiteral("owned-playing-fixture.wav"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    const auto bytes = wave();
    return file.write(bytes) == bytes.size() ? path : QString();
}
bool seedPrivateRows(const QString &name)
{
    for (const QString &table : {QStringLiteral("favorite_songs"), QStringLiteral("play_history")}) {
        QSqlQuery insert;
        insert.prepare(QStringLiteral("INSERT INTO %1 (song_id, song_name, artist_name, media_url) VALUES (?, ?, ?, ?)").arg(table));
        insert.addBindValue(-42);
        insert.addBindValue(name);
        insert.addBindValue(QStringLiteral("私人歌手"));
        insert.addBindValue(QUrl::fromLocalFile(QStringLiteral("/owned-fixture/%1.wav").arg(name)).toString());
        if (!insert.exec()) return false;
    }
    return true;
}
}

void NavigationTest::widgetContextDestructionDoesNotDeliverAbortedStatus()
{
    Fixture fixture;
    MainWindow window;
    QVERIFY(fixture.login(window));
    auto *backend = AppBackendClient::instance();
    const int before = fixture.backend.count(QStringLiteral("/v1/agent/status"));
    fixture.backend.delays.insert(QStringLiteral("/v1/agent/status"), 600);
    auto *context = new QWidget;
    QPointer<QLabel> child = new QLabel(QStringLiteral("view child"), context);
    int callbacks = 0;
    bool callbackSawDestroyedChild = false;
    backend->refreshAgentStatus(context, [&](const QString &) {
        ++callbacks;
        callbackSawDestroyedChild = !child;
    });
    QTRY_COMPARE(fixture.backend.count(QStringLiteral("/v1/agent/status")), before + 1);
    delete context;
    QVERIFY(child.isNull());
    QCOMPARE(callbacks, 0);
    QVERIFY(!callbackSawDestroyedChild);
    QTest::qWait(700);
    QCOMPARE(callbacks, 0);
    QVERIFY(backend->agentReady());
}

void NavigationTest::widgetContextDestructionAbortsMusicRequestWithoutCallback()
{
    Fixture fixture;
    NeteaseClient client;
    fixture.music.delays.insert(QStringLiteral("/search"), 600);
    auto *context = new QWidget;
    QPointer<QLabel> child = new QLabel(QStringLiteral("view child"), context);
    int callbacks = 0;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("keywords"), QStringLiteral("owned fixture"));
    client.get(QStringLiteral("/search"), query, context,
               [&](const QJsonObject &, const QString &) { ++callbacks; });
    QTRY_COMPARE(fixture.music.count(QStringLiteral("/search")), 1);
    QPointer<QTcpSocket> pendingSocket;
    for (auto *socket : fixture.music.server.findChildren<QTcpSocket *>())
        if (socket->state() == QAbstractSocket::ConnectedState) pendingSocket = socket;
    QVERIFY(pendingSocket);
    delete context;
    QVERIFY(child.isNull());
    QCOMPARE(callbacks, 0);
    QTRY_VERIFY_WITH_TIMEOUT(!pendingSocket || pendingSocket->state() == QAbstractSocket::UnconnectedState, 200);
    QTest::qWait(650);
    QCOMPARE(callbacks, 0);
}

void NavigationTest::commonPagesStayEmbeddedAndAreReused()
{
    Fixture fixture;
    MainWindow window;
    QVERIFY(fixture.login(window));
    window.show();
    auto *stack = pages(window);
    QVERIFY(stack);
    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("libraryPage"));
    QCOMPARE(visibleTopLevelWindows(), 1);
    QVector<QWidget *> instances;
    const QVector<QPair<QByteArray, QByteArray>> destinations{
        {"radioButton", "radioDialog"}, {"focusButton", "focusDialog"},
        {"pushButton_HistorySong", "historyPage"}, {"pushButton_LikeSong", "favoritesPage"}};
    for (const auto &destination : destinations) {
        auto *button = nav(window, destination.first.constData());
        QVERIFY(button);
        button->click();
        auto *page = window.findChild<QWidget *>(QString::fromLatin1(destination.second));
        QVERIFY(page);
        QCOMPARE(stack->currentWidget(), page);
        QVERIFY(!page->isWindow());
        QCOMPARE(page->parentWidget(), stack);
        QVERIFY(button->isChecked());
        QCOMPARE(visibleTopLevelWindows(), 1);
        instances.append(page);
    }
    auto *query = window.findChild<QLineEdit *>(QStringLiteral("lineEdit_InputSong"));
    QVERIFY(query);
    query->setText(QStringLiteral("测试歌曲"));
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_SearchSong_clicked", Qt::DirectConnection));
    auto *search = window.findChild<OnlineSearchDialog *>();
    QVERIFY(search);
    QTRY_COMPARE(search->findChild<QTableWidget *>("table_OnlineSearchResults")->rowCount(), 1);
    QCOMPARE(stack->currentWidget(), search);
    QVERIFY(!search->isWindow());
    QCOMPARE(visibleTopLevelWindows(), 1);
    const int pageCount = stack->count();
    for (int i = 0; i < destinations.size(); ++i) {
        nav(window, destinations.at(i).first.constData())->click();
        QCOMPARE(stack->currentWidget(), instances.at(i));
        QCOMPARE(stack->count(), pageCount);
    }
    nav(window, "focusButton")->click();
    auto *focus = qobject_cast<FocusDialog *>(stack->currentWidget());
    QVERIFY(focus);
    focus->findChild<QPushButton *>("button_FocusClose")->click();
    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("libraryPage"));
    nav(window, "radioButton")->click();
    QTest::keyClick(stack->currentWidget(), Qt::Key_Escape);
    QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("libraryPage"));
    QCOMPARE(visibleTopLevelWindows(), 1);
}

void NavigationTest::miniPlayerOpensDetailsAndNavigationPreservesPlayback()
{
    Fixture fixture;
    MainWindow window;
    QVERIFY(fixture.login(window));
    window.p_PlayerObject->setMuted(true);
    window.show();
    const QString file = createPlayingFixture(fixture);
    QVERIFY(!file.isEmpty());
    window.addLocalFiles({file});
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    const QUrl media = window.p_PlayerObject->currentMedia().request().url();
    const int history = historyCount();
    auto *mini = nav(window, "miniPlayerButton");
    QVERIFY(mini);
    QCOMPARE(window.findChild<QLabel *>("miniTitleLabel")->text(), QStringLiteral("owned-playing-fixture"));
    QVERIFY(window.findChild<QLabel *>("miniArtistLabel")->text().contains(QStringLiteral("本地音乐")));
    bool hasArtwork = false;
    for (auto *child : mini->findChildren<QWidget *>())
        if (dynamic_cast<ArtworkWidget *>(child)) hasArtwork = true;
    QVERIFY(hasArtwork);
    for (const char *destination : {"pushButton_HistorySong", "pushButton_LikeSong", "radioButton", "focusButton", "libraryButton"}) {
        nav(window, destination)->click();
        QVERIFY(mini->isVisible());
        QCOMPARE(window.p_PlayerObject->state(), QMediaPlayer::PlayingState);
        QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), media);
        QCOMPARE(window.p_PlayerList->mediaCount(), 1);
        QCOMPARE(historyCount(), history);
        mini->click();
        QCOMPARE(pages(window)->currentWidget()->objectName(), QStringLiteral("nowPlayingPage"));
        QCOMPARE(window.p_PlayerObject->state(), QMediaPlayer::PlayingState);
        QCOMPARE(visibleTopLevelWindows(), 1);
    }
    nav(window, "queueButton")->click();
    QCOMPARE(pages(window)->currentWidget()->objectName(), QStringLiteral("libraryPage"));
    window.p_PlayerObject->pause();
    nav(window, "radioButton")->click();
    mini->click();
    QCOMPARE(window.p_PlayerObject->state(), QMediaPlayer::PausedState);
}

void NavigationTest::leavingAssistantCancelsPendingWorkAndKeepsSuccessfulResults()
{
    Fixture fixture;
    MainWindow window;
    QVERIFY(fixture.login(window));
    window.show();
    nav(window, "radioButton")->click();
    auto *page = window.findChild<RadioDialog *>();
    QVERIFY(page);
    QVERIFY(submit(page, QStringLiteral("微醺时刻听歌")));
    QTRY_COMPARE_WITH_TIMEOUT(page->recommendations().size(), 1, 3000);
    const auto previous = page->recommendations().first().song.id;
    fixture.backend.delays.insert(QStringLiteral("/v1/assistant/plan"), 600);
    QSignalSpy cancelled(page, &RadioDialog::cancelRequested);
    QVERIFY(submit(page, QStringLiteral("换一批")));
    QTRY_COMPARE(fixture.backend.count(QStringLiteral("/v1/assistant/plan")), 2);
    nav(window, "libraryButton")->click();
    QCOMPARE(cancelled.size(), 1);
    nav(window, "radioButton")->click();
    QCOMPARE(window.findChild<RadioDialog *>(), page);
    QCOMPARE(page->recommendations().size(), 1);
    QCOMPARE(page->recommendations().first().song.id, previous);
    QVERIFY(!page->findChild<QPushButton *>("button_RadioCancel")->isEnabled());
    auto *draft = page->findChild<QPlainTextEdit *>("edit_RadioInstruction"); QVERIFY(draft);
    QCOMPARE(draft->toPlainText(), QStringLiteral("换一批"));
    QVERIFY(page->findChild<QPushButton *>("button_RadioGenerate")->isEnabled());
    QTest::qWait(700);
    QCOMPARE(fixture.backend.count(QStringLiteral("/v1/radio/select")), 1);
    QCOMPARE(page->recommendations().first().song.id, previous);
    QCOMPARE(draft->toPlainText(), QStringLiteral("换一批"));
    fixture.backend.delays.remove(QStringLiteral("/v1/assistant/plan"));
    // A new batch needs a new source and candidate: source123 and track42
    // were consumed by the first preview and are deliberately excluded.
    const auto originalCatalog = fixture.music.handler;
    fixture.music.handler = [originalCatalog](const FakeServer::Request &request) {
        auto response = originalCatalog(request);
        if (request.path == QStringLiteral("/radio/search")) {
            auto rows = response.value("playlists").toArray();
            auto playlist = rows.first().toObject(); playlist.insert("id", 124); rows[0] = playlist;
            response.insert("playlists", rows);
        }
        if (request.path == QStringLiteral("/radio/playlist")) {
            auto playlist = response.value("playlist").toObject(); playlist.insert("id", 124);
            response.insert("playlist", playlist);
            auto rows = response.value("songs").toArray();
            auto song = rows.first().toObject(); song.insert("id", 43); rows[0] = song;
            response.insert("songs", rows);
        }
        return response;
    };
    const auto originalBackend = fixture.backend.handler;
    fixture.backend.handler = [originalBackend](const FakeServer::Request &request) {
        auto response = originalBackend(request);
        if (request.path == QStringLiteral("/v1/radio/select")) {
            auto rows = response.value("tracks").toArray();
            auto track = rows.first().toObject(); track.insert("id", 43); rows[0] = track;
            response.insert("tracks", rows);
        }
        return response;
    };
    QVERIFY(submit(page, QStringLiteral("换一批")));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.backend.count(QStringLiteral("/v1/assistant/plan")), 3, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.backend.count(QStringLiteral("/v1/radio/select")), 2, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(draft->toPlainText().isEmpty(), 3000);
    QCOMPARE(page->recommendations().size(), 1);
    QCOMPARE(visibleTopLevelWindows(), 1);
}

void NavigationTest::leavingFocusCancelsPendingParseAndKeepsActiveSession()
{
    Fixture fixture;
    MainWindow window;
    QVERIFY(fixture.login(window));
    window.p_PlayerObject->setMuted(true);
    window.show();
    const QString file = createPlayingFixture(fixture);
    QVERIFY(!file.isEmpty());
    window.addLocalFiles({file});
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    nav(window, "focusButton")->click();
    auto *page = window.findChild<FocusDialog *>();
    QVERIFY(page);
    auto *input = page->findChild<QLineEdit *>("edit_FocusInstruction");
    input->setText(QStringLiteral("工作一分钟"));
    fixture.backend.delays.insert(QStringLiteral("/v1/focus/parse"), 600);
    QSignalSpy cancelled(page, &FocusDialog::cancelRequested);
    page->findChild<QPushButton *>("button_FocusParse")->click();
    QTRY_COMPARE(fixture.backend.count(QStringLiteral("/v1/focus/parse")), 1);
    const quint64 pendingRevision = page->generation();
    nav(window, "libraryButton")->click();
    QCOMPARE(cancelled.size(), 1);
    QVERIFY(!page->isCurrentGeneration(pendingRevision));
    nav(window, "focusButton")->click();
    QCOMPARE(page->request().originalText, QStringLiteral("工作一分钟"));
    QTest::qWait(700);
    QCOMPARE(page->findChild<QTableWidget *>("table_FocusPlan")->rowCount(), 0);
    QVERIFY(page->findChild<QPushButton *>("button_FocusParse")->isEnabled());
    page->findChild<QPushButton *>("button_FocusPreview")->click();
    QCOMPARE(page->findChild<QTableWidget *>("table_FocusPlan")->rowCount(), 1);
    page->findChild<QPushButton *>("button_FocusStart")->click();
    auto *session = window.findChild<FocusSession *>("focusSession");
    QVERIFY(session);
    QVERIFY(session->isActive());
    nav(window, "radioButton")->click();
    QVERIFY(session->isActive());
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 3000);
    nav(window, "focusButton")->click();
    QVERIFY(page->findChild<QPushButton *>("button_FocusPause")->isEnabled());
    QVERIFY(session->isActive());
}

void NavigationTest::searchNavigationRejectsAnOlderResponse()
{
    Fixture fixture;
    MainWindow window;
    window.show();
    const auto original = fixture.music.handler;
    fixture.music.handler = [&](const FakeServer::Request &request) -> QJsonObject {
        if (request.path != QStringLiteral("/search")) return original(request);
        return {{"songs", QJsonArray{QJsonObject{{"id", 84},
            {"title", fixture.music.count(QStringLiteral("/search")) == 1 ? "stale" : "fresh"},
            {"artist", "artist"}, {"durationMs", 180000}}}}, {"total", 1}};
    };
    fixture.music.delays.insert(QStringLiteral("/search"), 600);
    auto *query = window.findChild<QLineEdit *>("lineEdit_InputSong");
    query->setText(QStringLiteral("old query"));
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_SearchSong_clicked", Qt::DirectConnection));
    auto *page = window.findChild<OnlineSearchDialog *>();
    QVERIFY(page);
    QTRY_COMPARE(fixture.music.count(QStringLiteral("/search")), 1);
    nav(window, "libraryButton")->click();
    fixture.music.delays.insert(QStringLiteral("/search"), 0);
    query->setText(QStringLiteral("new query"));
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_SearchSong_clicked", Qt::DirectConnection));
    QCOMPARE(window.findChild<OnlineSearchDialog *>(), page);
    auto *table = page->findChild<QTableWidget *>("table_OnlineSearchResults");
    QTRY_COMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("fresh"));
    QTest::qWait(700);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("fresh"));
}

void NavigationTest::accountSwitchClearsEveryCachedPrivatePage()
{
    Fixture fixture;
    MainWindow window;
    QVERIFY(fixture.login(window));
    window.show();
    const QString aliceProfile = window.accountProfile();
    QVERIFY(seedPrivateRows(QStringLiteral("Alice私藏")));
    nav(window, "pushButton_HistorySong")->click();
    auto *history = window.findChild<HistorySong *>();
    auto *historyTable = history->findChild<QTableWidget *>("table_HistorySong");
    QCOMPARE(historyTable->rowCount(), 1);
    nav(window, "pushButton_LikeSong")->click();
    auto *favorites = window.findChild<LikeSong *>();
    auto *favoritesTable = favorites->findChild<QTableWidget *>("tableWidget_LikeSong");
    QCOMPARE(favoritesTable->rowCount(), 1);
    nav(window, "focusButton")->click();
    auto *focus = window.findChild<FocusDialog *>();
    focus->findChild<QLineEdit *>("edit_FocusInstruction")->setText(QStringLiteral("Alice的私人安排"));
    nav(window, "radioButton")->click();
    auto *assistant = window.findChild<RadioDialog *>();
    QVERIFY(submit(assistant, QStringLiteral("微醺时刻听歌")));
    QTRY_COMPARE_WITH_TIMEOUT(assistant->recommendations().size(), 1, 3000);
    auto *query = window.findChild<QLineEdit *>("lineEdit_InputSong");
    query->setText(QStringLiteral("Alice搜索"));
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_SearchSong_clicked", Qt::DirectConnection));
    auto *search = window.findChild<OnlineSearchDialog *>();
    auto *searchTable = search->findChild<QTableWidget *>("table_OnlineSearchResults");
    QTRY_COMPARE(searchTable->rowCount(), 1);
    QVERIFY(fixture.login(window, QStringLiteral("bob")));
    QVERIFY(window.accountProfile() != aliceProfile);
    QCOMPARE(historyTable->rowCount(), 0);
    QCOMPARE(favoritesTable->rowCount(), 0);
    QCOMPARE(searchTable->rowCount(), 0);
    QVERIFY(query->text().isEmpty());
    QVERIFY(assistant->recommendations().isEmpty());
    QVERIFY(assistant->findChild<QPlainTextEdit *>("text_RadioConversation")->toPlainText().isEmpty());
    QVERIFY(focus->request().originalText.isEmpty());
    QCOMPARE(visibleTopLevelWindows(), 1);
    QVERIFY(fixture.login(window, QStringLiteral("alice")));
    QCOMPARE(window.accountProfile(), aliceProfile);
    QCOMPARE(historyTable->rowCount(), 1);
    QCOMPARE(favoritesTable->rowCount(), 1);
    QCOMPARE(historyTable->item(0, 0)->text(), QStringLiteral("Alice私藏"));
    QCOMPARE(window.findChild<RadioDialog *>(), assistant);
    QCOMPARE(window.findChild<HistorySong *>(), history);
    QCOMPARE(window.findChild<LikeSong *>(), favorites);
}

void NavigationTest::cleanup()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (QSqlDatabase::contains()) {
        QSqlDatabase::database(QSqlDatabase::defaultConnection, false).close();
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }
}

QTEST_MAIN(NavigationTest)
#include "navigation_test.moc"
