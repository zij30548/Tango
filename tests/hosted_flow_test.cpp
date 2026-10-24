#include "appbackendclient.h"
#include "focusdialog.h"
#include "focusmodelclient.h"
#include "focussession.h"
#include "mainwindow.h"
#include "signin.h"
#include "python_backend_fixture.h"
#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
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

class OwnedBackend
{
  public:
    QProcess process;
    ~OwnedBackend()
    {
        if (process.state() == QProcess::NotRunning)
            return;
        process.terminate();
        if (!process.waitForFinished(3000))
        {
            process.kill();
            process.waitForFinished(3000);
        }
    }
};

QJsonObject parsedRequest()
{
    return {{"minutes", 1},
            {"scene", "work"},
            {"source", "queue"},
            {"excludeLive", true},
            {"instrumentalOnly", false},
            {"excludedArtists", QJsonArray{}},
            {"unsupported", QJsonArray{}}};
}

class MockQwen : public QObject
{
  public:
    struct Request
    {
        QByteArray headers;
        QJsonObject body;
    };
    QVector<Request> requests;
    int delay = 0;
    QTcpServer server;
    MockQwen()
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
                            int length = 0;
                            for (const auto &line : headers.split('\n'))
                                if (line.toLower().startsWith("content-length:"))
                                    length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                            if (bytes->size() < headerEnd + 4 + length)
                                return;
                            *handled = true;
                            requests.append(
                                {headers, QJsonDocument::fromJson(bytes->mid(headerEnd + 4, length))
                                              .object()});
                            const QByteArray payload =
                                QJsonDocument(
                                    QJsonObject{
                                        {"choices",
                                         QJsonArray{QJsonObject{
                                             {"finish_reason", "stop"},
                                             {"message",
                                              QJsonObject{
                                                  {"role", "assistant"},
                                                  {"content",
                                                   QString::fromUtf8(
                                                       QJsonDocument(parsedRequest())
                                                           .toJson(QJsonDocument::Compact))}}}}}}})
                                    .toJson(QJsonDocument::Compact);
                            QPointer<QTcpSocket> guard(socket);
                            QTimer::singleShot(
                                delay, this,
                                [guard, payload]
                                {
                                    if (!guard || guard->state() != QAbstractSocket::ConnectedState)
                                        return;
                                    guard->write("HTTP/1.1 200 OK\r\nContent-Type: "
                                                 "application/json\r\nConnection: "
                                                 "close\r\nContent-Length: " +
                                                 QByteArray::number(payload.size()) + "\r\n\r\n" +
                                                 payload);
                                    guard->disconnectFromHost();
                                });
                        });
                }
            });
    }
    QByteArray base() const
    {
        return "http://127.0.0.1:" + QByteArray::number(server.serverPort()) + "/v1";
    }
};

// 测试自己的透明中继记录客户端与后台通信，不接触后台连接模型服务的流量。
class BackendWire : public QObject
{
  public:
    QTcpServer server;
    QByteArray requests, responses;
    explicit BackendWire(quint16 backendPort)
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this,
                [this, backendPort]
                {
                    while (server.hasPendingConnections())
                    {
                        auto *desktop = server.nextPendingConnection();
                        auto *upstream = new QTcpSocket(desktop);
                        connect(desktop, &QTcpSocket::readyRead, desktop,
                                [this, desktop, upstream]
                                {
                                    const auto bytes = desktop->readAll();
                                    requests.append(bytes);
                                    upstream->write(bytes);
                                });
                        connect(upstream, &QTcpSocket::readyRead, desktop,
                                [this, desktop, upstream]
                                {
                                    const auto bytes = upstream->readAll();
                                    responses.append(bytes);
                                    desktop->write(bytes);
                                });
                        connect(upstream, &QTcpSocket::disconnected, desktop,
                                &QTcpSocket::disconnectFromHost);
                        connect(desktop, &QTcpSocket::disconnected, desktop, &QObject::deleteLater);
                        upstream->connectToHost(QHostAddress::LocalHost, backendPort);
                    }
                });
    }
    QByteArray base() const
    {
        return "http://127.0.0.1:" + QByteArray::number(server.serverPort());
    }
};

QByteArray wave()
{
    constexpr quint32 rate = 22050, size = rate * 12 * sizeof(qint16);
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
} // namespace

class HostedFlowTest : public QObject
{
    Q_OBJECT
  private slots:
    void realBackendAccountsModelPreviewPlaybackAndLogout();
    void logoutWithUnavailableGuestUsesIsolatedTemporarySpace_data();
    void logoutWithUnavailableGuestUsesIsolatedTemporarySpace();
    void cleanup();
};

void HostedFlowTest::realBackendAccountsModelPreviewPlaybackAndLogout()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    ScopedEnvironment environment;
    environment.set("MUSICPLAYER_DATA_DIR", data.path().toUtf8());
    environment.set("MUSICPLAYER_NETEASE_PORT", "9");
    environment.set("MUSICPLAYER_REDIS_PORT", "invalid-test-port");
    environment.set("MUSICPLAYER_AGENT_API_KEY", {});
    environment.set("DASHSCOPE_API_KEY", {});
    MockQwen qwen;
    QVERIFY(qwen.server.isListening());
    QTcpServer reservation;
    QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const quint16 backendPort = reservation.serverPort();
    reservation.close();
    const QString serverPath = QFINDTESTDATA("../backend_py/tango_backend/server.py");
    QVERIFY(!serverPath.isEmpty());
    const QString backendDirectory = QDir::cleanPath(QFileInfo(serverPath).absolutePath() + "/..");
    const QString python = QDir(backendDirectory).filePath(".venv/bin/python");
    QVERIFY(QFileInfo(python).isExecutable());
    OwnedBackend backend;
    QProcessEnvironment childEnvironment;
    childEnvironment.insert("QUIET_REGISTRATION_MODE", "open");
    childEnvironment.insert("PATH", "/usr/bin:/bin");
    childEnvironment.insert("QUIET_BACKEND_HOST", "127.0.0.1");
    childEnvironment.insert("QUIET_BACKEND_PORT", QString::number(backendPort));
    childEnvironment.insert("QUIET_DB_PATH", data.filePath("owned-backend.sqlite"));
    childEnvironment.insert("QUIET_STORAGE_DRIVER", "sqlite");
    childEnvironment.insert("QWEN_API_KEY", "fake-test-only");
    childEnvironment.insert("QWEN_BASE_URL", QString::fromLatin1(qwen.base()));
    childEnvironment.insert("QWEN_MODEL", "qwen-integration-fixture");
    backend.process.setProcessEnvironment(childEnvironment);
    backend.process.setProcessChannelMode(QProcess::MergedChannels);
    backend.process.setWorkingDirectory(backendDirectory);
    backend.process.start(python, {"-m", "tango_backend.server", "--no-env"});
    QVERIFY(backend.process.waitForStarted(3000));
    QTRY_VERIFY_WITH_TIMEOUT(ownedPythonBackendReady(backend.process, backendPort), PythonBackendFixtureStartupTimeoutMs);
    QByteArray startup;
    startup += backend.process.readAll();
    QVERIFY(!startup.contains("fake-test-only"));
    // 发送隔离测试凭据前，再明确校验后台归当前测试所有。
    QNetworkAccessManager healthNetwork;
    auto *health = healthNetwork.get(
        QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/health").arg(backendPort))));
    QTRY_VERIFY_WITH_TIMEOUT(health->isFinished(), 3000);
    const auto healthObject = QJsonDocument::fromJson(health->readAll()).object();
    QCOMPARE(healthObject.value("app").toString(), QStringLiteral("quiet-app-backend"));
    QCOMPARE(healthObject.value("pid").toVariant().toLongLong(), backend.process.processId());
    health->deleteLater();
    BackendWire wire(backendPort);
    QVERIFY(wire.server.isListening());
    environment.set("MUSICPLAYER_BACKEND_URL", wire.base());
    auto *client = AppBackendClient::instance();
    QVERIFY(!client->authenticated());
    MainWindow window;
    window.p_PlayerObject->setMuted(true);
    QCOMPARE(window.accountProfile(), QStringLiteral("guest"));
    FocusModelClient parser(&window);
    bool anonymousDone = false;
    QString anonymousError;
    parser.parse(QStringLiteral("工作1分钟不要现场版"), FocusRequest{}, &window,
                 [&](const FocusRequest &, const QString &error)
                 {
                     anonymousError = error;
                     anonymousDone = true;
                 });
    QVERIFY(anonymousDone);
    QVERIFY(!anonymousError.isEmpty());
    QCOMPARE(qwen.requests.count(), 0);

    bool registered = false;
    QString authError;
    client->registerUser(QStringLiteral("owned-flow-user"),
                         QStringLiteral("owned-fixture-password"), &window,
                         [&](const QString &error)
                         {
                             authError = error;
                             registered = true;
                         });
    QTRY_VERIFY_WITH_TIMEOUT(registered, 5000);
    QVERIFY2(authError.isEmpty(), qPrintable(authError));
    QVERIFY(client->authenticated());
    const QString firstUserId = client->userId();
    const QString profile = "server:" + client->serviceIdentity() + ":" + firstUserId;
    QCOMPARE(window.accountProfile(), profile);
    QVERIFY(QFileInfo::exists(data.filePath("owned-backend.sqlite")));
    QVERIFY(FocusStore(QSqlDatabase::database()).addRule(
        profile, "work", "exclude_artist", "private-fixture-artist") > 0);
    bool loggedOut = false;
    client->logout(&window, [&](const QString &) { loggedOut = true; });
    QVERIFY(!client->authenticated());
    QCOMPARE(window.accountProfile(), QStringLiteral("guest"));
    QTRY_VERIFY_WITH_TIMEOUT(loggedOut, 3000);
    QVERIFY(FocusStore(QSqlDatabase::database()).rules("guest").isEmpty());
    bool wrongDone = false;
    client->login("owned-flow-user", "wrong-password", &window,
                  [&](const QString &error)
                  {
                      authError = error;
                      wrongDone = true;
                  });
    QTRY_VERIFY_WITH_TIMEOUT(wrongDone, 5000);
    QVERIFY(!authError.isEmpty());
    QVERIFY(!client->authenticated());
    QCOMPARE(window.accountProfile(), QStringLiteral("guest"));
    bool loginDone = false;
    client->login("owned-flow-user", "owned-fixture-password", &window,
                  [&](const QString &error)
                  {
                      authError = error;
                      loginDone = true;
                  });
    QTRY_VERIFY_WITH_TIMEOUT(loginDone, 5000);
    QVERIFY2(authError.isEmpty(), qPrintable(authError));
    QCOMPARE(client->userId(), firstUserId);
    QCOMPARE(window.accountProfile(), profile);
    QCOMPARE(FocusStore(QSqlDatabase::database()).rules(profile).size(), 1);
    bool readyDone = false;
    client->refreshAgentStatus(&window,
                               [&](const QString &error)
                               {
                                   authError = error;
                                   readyDone = true;
                               });
    QTRY_VERIFY_WITH_TIMEOUT(readyDone, 3000);
    QVERIFY2(authError.isEmpty(), qPrintable(authError));
    QVERIFY(client->agentReady());

    QStringList music;
    for (const QString &name :
         {QStringLiteral("Studio fixture.wav"), QStringLiteral("Live fixture.wav")})
    {
        QFile file(data.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto bytes = wave();
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        music.append(file.fileName());
    }
    window.addLocalFiles(music);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    window.p_PlayerObject->pause();
    auto *session = window.findChild<FocusSession *>("focusSession");
    QVERIFY(session);
    bool dialogVisited = false, previewed = false, started = false;
    QPointer<FocusDialog> activeDialog;
    QTimer::singleShot(0, &window,
                       [&]
                       {
                           activeDialog = window.findChild<FocusDialog *>();
                           if (!activeDialog)
                               return;
                           dialogVisited = true;
                           activeDialog->findChild<QLineEdit *>("edit_FocusInstruction")
                               ->setText(QStringLiteral("工作1分钟不要现场版"));
                           activeDialog->findChild<QPushButton *>("button_FocusParse")->click();
                           auto *table = activeDialog->findChild<QTableWidget *>("table_FocusPlan");
                           QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 5000);
                           previewed = activeDialog->request().minutes == 1 &&
                                       activeDialog->request().scene == "work" &&
                                       activeDialog->request().excludeLive &&
                                       table->item(0, 0)->text().contains("Studio");
                           activeDialog->findChild<QPushButton *>("button_FocusStart")->click();
                           QTRY_VERIFY_WITH_TIMEOUT(session->isRunning(), 7000);
                           started = true;
                           activeDialog->accept();
                       });
    QTimer::singleShot(12000, &window,
                       [&]
                       {
                           if (activeDialog)
                               activeDialog->reject();
                       });
    QVERIFY(QMetaObject::invokeMethod(&window, "openFocus", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(dialogVisited && started, 12000);
    QVERIFY(dialogVisited);
    QVERIFY(previewed);
    QVERIFY(started);
    QVERIFY(session->isActive());
    QCOMPARE(session->plan().tracks.size(), 1);
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(),
             QUrl::fromLocalFile(music.first()));
    QCOMPARE(qwen.requests.count(), 1);
    QVERIFY(qwen.requests.first().headers.contains("Bearer fake-test-only"));
    QVERIFY(qwen.requests.first().headers.startsWith("POST /v1/chat/completions HTTP/1.1"));
    const auto messages = qwen.requests.first().body.value("messages").toArray();
    QCOMPARE(messages.size(), 2);
    QVERIFY(messages.at(1).toObject().value("content").toString().contains(
        QStringLiteral("工作1分钟不要现场版")));
    QVERIFY(!wire.requests.contains("fake-test-only"));
    QVERIFY(!wire.responses.contains("fake-test-only"));
    QVERIFY(!wire.requests.contains("Studio fixture"));
    QVERIFY(wire.requests.contains("/v1/focus/parse"));
    QVERIFY(wire.responses.contains("\"excludeLive\":true"));

    // A failed account switch must keep the current data space and active plan.
    QSignalSpy accountChanges(client, &AppBackendClient::authChanged);
    SignIn switchAccount;
    switchAccount.findChild<QLineEdit *>("lineEdit_Id")
        ->setText(QStringLiteral("another-owned-user"));
    switchAccount.findChild<QLineEdit *>("lineEdit_Password")
        ->setText(QStringLiteral("wrong-fixture-password"));
    switchAccount.findChild<QPushButton *>("pushButton_SignIn")->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        !switchAccount.findChild<QLabel *>("label_AuthStatus")->text().isEmpty(), 5000);
    QVERIFY(client->authenticated());
    QCOMPARE(client->userId(), firstUserId);
    QCOMPARE(window.accountProfile(), profile);
    QCOMPARE(FocusStore(QSqlDatabase::database()).rules(profile).size(), 1);
    QCOMPARE(accountChanges.count(), 0);
    QVERIFY(session->isActive());
    QVERIFY(session->isRunning());
    QCOMPARE(window.p_PlayerObject->state(), QMediaPlayer::PlayingState);
    switchAccount.reject();

    session->cancel();
    qwen.delay = 500;
    bool staleVisited = false, staleInvalid = false;
    activeDialog.clear();
    QTimer::singleShot(
        0, &window,
        [&]
        {
            activeDialog = window.findChild<FocusDialog *>();
            if (!activeDialog)
                return;
            staleVisited = true;
            activeDialog->findChild<QLineEdit *>("edit_FocusInstruction")
                ->setText(QStringLiteral("工作1分钟不要现场版"));
            activeDialog->findChild<QPushButton *>("button_FocusParse")->click();
            QTRY_COMPARE_WITH_TIMEOUT(qwen.requests.count(), 2, 5000);
            QVERIFY(QMetaObject::invokeMethod(&window, "logoutLocalAccount", Qt::DirectConnection));
            QVERIFY(!client->authenticated());
            QCOMPARE(window.accountProfile(), QStringLiteral("guest"));
            QTest::qWait(650);
            auto *table = activeDialog->findChild<QTableWidget *>("table_FocusPlan");
            auto *start = activeDialog->findChild<QPushButton *>("button_FocusStart");
            auto *parse = activeDialog->findChild<QPushButton *>("button_FocusParse");
            staleInvalid = table->rowCount() == 0 && !start->isEnabled() && !parse->isEnabled() &&
                           activeDialog->request().originalText.isEmpty();
            activeDialog->accept();
        });
    QTimer::singleShot(9000, &window,
                       [&]
                       {
                           if (activeDialog)
                               activeDialog->reject();
                       });
    QVERIFY(QMetaObject::invokeMethod(&window, "openFocus", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(staleVisited && staleInvalid, 9000);
    QVERIFY(staleVisited);
    QVERIFY(staleInvalid);
    QVERIFY(!client->authenticated());
    QCOMPARE(window.accountProfile(), QStringLiteral("guest"));
    QVERIFY(!session->isActive());
    QVERIFY(!wire.requests.contains("fake-test-only"));
    QVERIFY(!wire.responses.contains("fake-test-only"));
    QVERIFY(!backend.process.readAll().contains("fake-test-only"));
}

void HostedFlowTest::logoutWithUnavailableGuestUsesIsolatedTemporarySpace_data()
{
    QTest::addColumn<QString>("failure");
    QTest::newRow("corrupted guest file") << QStringLiteral("corrupt");
    QTest::newRow("read-only guest file") << QStringLiteral("readonly");
    QTest::newRow("unavailable data directory") << QStringLiteral("directory");
}

void HostedFlowTest::logoutWithUnavailableGuestUsesIsolatedTemporarySpace()
{
    QFETCH(QString, failure);
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const QString desktopData = data.filePath("desktop");
    ScopedEnvironment environment;
    environment.set("MUSICPLAYER_DATA_DIR", desktopData.toUtf8());
    environment.set("MUSICPLAYER_NETEASE_PORT", "9");
    environment.set("MUSICPLAYER_REDIS_PORT", "invalid-test-port");
    QTcpServer reservation;
    QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const quint16 port = reservation.serverPort();
    reservation.close();
    const QString serverPath = QFINDTESTDATA("../backend_py/tango_backend/server.py");
    QVERIFY(!serverPath.isEmpty());
    const QString backendDirectory = QDir::cleanPath(QFileInfo(serverPath).absolutePath() + "/..");
    const QString python = QDir(backendDirectory).filePath(".venv/bin/python");
    QVERIFY(QFileInfo(python).isExecutable());
    OwnedBackend backend;
    QProcessEnvironment childEnvironment;
    childEnvironment.insert("QUIET_REGISTRATION_MODE", "open");
    childEnvironment.insert("PATH", "/usr/bin:/bin");
    childEnvironment.insert("QUIET_BACKEND_HOST", "127.0.0.1");
    childEnvironment.insert("QUIET_BACKEND_PORT", QString::number(port));
    childEnvironment.insert("QUIET_DB_PATH", data.filePath("owned-backend.sqlite"));
    childEnvironment.insert("QUIET_STORAGE_DRIVER", "sqlite");
    backend.process.setProcessEnvironment(childEnvironment);
    backend.process.setProcessChannelMode(QProcess::MergedChannels);
    backend.process.setWorkingDirectory(backendDirectory);
    backend.process.start(python, {"-m", "tango_backend.server", "--no-env"});
    QVERIFY(backend.process.waitForStarted(3000));
    QTRY_VERIFY_WITH_TIMEOUT(ownedPythonBackendReady(backend.process, port), PythonBackendFixtureStartupTimeoutMs);
    environment.set("MUSICPLAYER_BACKEND_URL",
                    "http://127.0.0.1:" + QByteArray::number(port));
    auto *client = AppBackendClient::instance();
    QVERIFY(!client->authenticated());
    MainWindow window;
    window.p_PlayerObject->setMuted(true);
    bool registered = false;
    QString authError;
    client->registerUser("isolated-logout-user", "owned-fixture-password", &window,
                         [&](const QString &error)
                         {
                             authError = error;
                             registered = true;
                         });
    QTRY_VERIFY_WITH_TIMEOUT(registered, 5000);
    QVERIFY2(authError.isEmpty(), qPrintable(authError));
    const QString profile = window.accountProfile();
    QVERIFY(profile.startsWith("server:"));
    const QString accountPath = QSqlDatabase::database().databaseName();
    QVERIFY(FocusStore(QSqlDatabase::database()).addRule(
        profile, "work", "exclude_artist", "private-account-marker") > 0);
    QFile music(data.filePath("Logout fixture.wav"));
    QVERIFY(music.open(QIODevice::WriteOnly));
    const auto bytes = wave();
    QCOMPARE(music.write(bytes), qint64(bytes.size()));
    music.close();
    window.addLocalFiles({music.fileName()});
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_AddLike_clicked", Qt::DirectConnection));
    FocusRequest request;
    request.minutes = 1;
    const auto plan = FocusPlanner::build(request, window.focusCandidates("queue"),
                                          FocusStore(QSqlDatabase::database()).rules(profile, "work"));
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QVERIFY(window.startFocusPlan(plan));
    auto *session = window.findChild<FocusSession *>("focusSession");
    QVERIFY(session);
    QTRY_VERIFY_WITH_TIMEOUT(session->isRunning(), 7000);

    const QString guestPath = QDir(desktopData).filePath("history.db");
    QByteArray guestBytes;
    if (failure == "corrupt") {
        QFile guest(guestPath);
        QVERIFY(guest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        guestBytes = "owned deliberately corrupted guest fixture";
        QCOMPARE(guest.write(guestBytes), qint64(guestBytes.size()));
    } else if (failure == "readonly") {
        QFile guest(guestPath);
        QVERIFY(guest.open(QIODevice::ReadOnly));
        guestBytes = guest.readAll();
        guest.close();
        QVERIFY(guest.setPermissions(QFileDevice::ReadOwner));
        QVERIFY(!QFileInfo(guestPath).isWritable());
    } else {
        QFile blocker(data.filePath("not-a-directory"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        QVERIFY(blocker.write("owned path blocker") > 0);
        blocker.close();
        environment.set("MUSICPLAYER_DATA_DIR", blocker.fileName().toUtf8());
    }

    QVERIFY(QMetaObject::invokeMethod(&window, "logoutLocalAccount", Qt::DirectConnection));
    QVERIFY(!client->authenticated());
    QVERIFY(!window.isSign);
    QVERIFY(!session->isActive());
    QCOMPARE(window.p_PlayerObject->state(), QMediaPlayer::PausedState);
    QCOMPARE(window.accountProfile(), QStringLiteral("guest"));
    QVERIFY(window.findChild<QLabel *>("label_DisplayUser")->text().contains(
        QStringLiteral("临时空间")));
    QVERIFY(window.findChild<QPushButton *>("pushButton_SignIn")->text().contains(
        QStringLiteral("登录探歌")));
    {
        auto guest = QSqlDatabase::database(); // Auto-opening must never reopen accountPath.
        QVERIFY(guest.isOpen());
        QCOMPARE(guest.databaseName(), QStringLiteral(":memory:"));
        QVERIFY(guest.databaseName() != accountPath);
        QVERIFY(FocusStore(guest).rules(profile).isEmpty());
        QVERIFY(FocusStore(guest).rules("guest").isEmpty());
        QVERIFY(FocusStore(guest).addRule("guest", "work", "exclude_artist", "temporary-guest-marker") > 0);
        QSqlQuery favorites(guest);
        QVERIFY(favorites.exec("SELECT COUNT(*) FROM favorite_songs"));
        QVERIFY(favorites.next());
        QCOMPARE(favorites.value(0).toInt(), 0);
        favorites.finish();
        QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_AddLike_clicked", Qt::DirectConnection));
        QVERIFY(favorites.exec("SELECT COUNT(*) FROM favorite_songs"));
        QVERIFY(favorites.next());
        QCOMPARE(favorites.value(0).toInt(), 1);
    }
    if (failure != "directory") {
        QFile preserved(guestPath);
        QVERIFY(preserved.open(QIODevice::ReadOnly));
        QCOMPARE(preserved.readAll(), guestBytes);
    }

    // Re-enter the original account and prove temporary guest writes stayed out.
    environment.set("MUSICPLAYER_DATA_DIR", desktopData.toUtf8());
    bool loggedIn = false;
    client->login("isolated-logout-user", "owned-fixture-password", &window,
                  [&](const QString &error)
                  {
                      authError = error;
                      loggedIn = true;
                  });
    QTRY_VERIFY_WITH_TIMEOUT(loggedIn, 5000);
    QVERIFY2(authError.isEmpty(), qPrintable(authError));
    QCOMPARE(window.accountProfile(), profile);
    QCOMPARE(QSqlDatabase::database().databaseName(), accountPath);
    QCOMPARE(FocusStore(QSqlDatabase::database()).rules(profile).size(), 1);
    QVERIFY(FocusStore(QSqlDatabase::database()).rules("guest").isEmpty());
    {
        QSqlQuery favorites;
        QVERIFY(favorites.exec("SELECT COUNT(*) FROM favorite_songs"));
        QVERIFY(favorites.next());
        QCOMPARE(favorites.value(0).toInt(), 1);
    }
    client->logout(&window);
    QVERIFY(!client->authenticated());
    QCOMPARE(window.accountProfile(), QStringLiteral("guest"));
}

void HostedFlowTest::cleanup()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (QSqlDatabase::contains())
    {
        QSqlDatabase::database(QSqlDatabase::defaultConnection, false).close();
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }
}

QTEST_MAIN(HostedFlowTest)
#include "hosted_flow_test.moc"
