#include "appbackendclient.h"
#include "signin.h"
#include "signup.h"
#include <QDateTime>
#include <QDir>
#include <QAction>
#include <QToolButton>
#include <QTemporaryDir>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTcpServer>
#include <QTcpSocket>
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
        for (auto value = saved.crbegin(); value != saved.crend(); ++value)
        {
            if (value->existed)
                qputenv(value->name.constData(), value->value);
            else
                qunsetenv(value->name.constData());
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

class FakeBackend : public QObject
{
  public:
    QTcpServer server;
    QStringList routes;
    QVector<QJsonObject> bodies;
    QVector<QByteArray> requestHeaders;
    QString registrationMode = QStringLiteral("open");
    int passwordMinimum = 15;
    bool malformedConfig = false;
    bool failedConfig = false;
    int responseDelay = 0;
    bool deny = false;
    FakeBackend()
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
                                if (line.toLower().startsWith("content-length:"))
                                    contentLength =
                                        line.mid(line.indexOf(':') + 1).trimmed().toInt();
                            if (bytes->size() < headerEnd + 4 + contentLength)
                                return;
                            *handled = true;
                            const QString route = QString::fromLatin1(
                                headers.split('\n').first().split(' ').value(1));
                            const QJsonObject body =
                                QJsonDocument::fromJson(bytes->mid(headerEnd + 4, contentLength))
                                    .object();
                            routes.append(route);
                            bodies.append(body);
                            requestHeaders.append(headers);
                            QPointer<QTcpSocket> guard(socket);
                            const bool configRequest = route == QStringLiteral("/v1/auth/config");
                            const bool rejected = configRequest ? failedConfig : deny;
                            QJsonObject response =
                                rejected
                                    ? QJsonObject{{"error",
                                                   QJsonObject{{"code", "INVALID_CREDENTIALS"},
                                                               {"message", "用户名或密码错误。"}}}}
                                    : QJsonObject{
                                          {"token", "fake-owned-session-token"},
                                          {"expiresAt",
                                           QDateTime::currentDateTimeUtc().addSecs(3600).toString(
                                               Qt::ISODateWithMs)},
                                          {"user",
                                           QJsonObject{
                                               {"id", "owned-user-123"},
                                               {"username",
                                                body.value("username").toString("test-user")}}}};
                            if (configRequest && !failedConfig) {
                                response = QJsonObject{{"version", 1}, {"registrationMode", registrationMode},
                                    {"inviteRequired", registrationMode == QStringLiteral("invite_only")},
                                    {"passwordMin", passwordMinimum}, {"passwordMax", 128},
                                    {"usernameMin", 2}, {"usernameMax", 32}};
                                if (malformedConfig) response.insert("inviteRequired", "untrusted-string");
                            } else if (route == QStringLiteral("/v1/account/usage") && !rejected) {
                                response = QJsonObject{{"daily", QJsonObject{{"limit", 20}, {"used", 3},
                                    {"remaining", 17}, {"resetAt", "2030-01-02T00:00:00.000Z"}}},
                                    {"total", QJsonObject{{"limit", 100}, {"used", 8}, {"remaining", 92}}},
                                    {"disabled", false}};
                            }
                            QTimer::singleShot(
                                responseDelay, this,
                                [guard, response, rejected]
                                {
                                    if (!guard || guard->state() != QAbstractSocket::ConnectedState)
                                        return;
                                    const QByteArray payload =
                                        QJsonDocument(response).toJson(QJsonDocument::Compact);
                                    guard->write(
                                        "HTTP/1.1 " +
                                        QByteArray(rejected ? "401 Unauthorized" : "200 OK") +
                                        "\r\nContent-Type: application/json\r\nConnection: "
                                        "close\r\nContent-Length: " +
                                        QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
                                    guard->disconnectFromHost();
                                });
                        });
                }
            });
    }
    QByteArray url() const { return "http://127.0.0.1:" + QByteArray::number(server.serverPort()); }
};

void credentials(QDialog &dialog, const QString &password = QStringLiteral("test-password-123"))
{
    dialog.findChild<QLineEdit *>("lineEdit_Id")->setText(QStringLiteral("test-user"));
    dialog.findChild<QLineEdit *>("lineEdit_Password")->setText(password);
    if (auto *confirm = dialog.findChild<QLineEdit *>("lineEdit_ConfirmPassword")) confirm->setText(password);
}
} // namespace

class HostedAuthDialogsTest : public QObject
{
    Q_OBJECT
  private slots:
    void init();
    void cleanup();
    void loginCreatesAppSessionAndClearsPassword();
    void wrongPasswordNeverFallsBackToRedis();
    void registrationCompletesParentSignIn();
    void closePendingPreventsLateLogin();
    void closePendingRegistrationPreventsLateSession();
    void invalidConfigurationNeverFallsBackToRedis();
    void passwordVisibilityIsAccessibleAndConcealedWhenHidden_data();
    void passwordVisibilityIsAccessibleAndConcealedWhenHidden();
    void mismatchedConfirmationDoesNotSubmit();
    void inviteRegistrationRequiresCodeAndSendsItOnce();
    void unavailableRegistrationPolicyFailsClosed_data();
    void unavailableRegistrationPolicyFailsClosed();
    void failedPolicyCanBeRetried();
    void advertisedPasswordMinimumControlsRegistration_data();
    void advertisedPasswordMinimumControlsRegistration();
    void closingPendingPolicyPreventsLateEnable();
    void accountUsageUsesAuthenticatedSession();
    void backendChangeRequiresNewPolicyBeforeRegistration();

  private:
    std::unique_ptr<ScopedEnvironment> environment;
    void configure(FakeBackend &backend);
};

void HostedAuthDialogsTest::init()
{
    environment.reset(new ScopedEnvironment);
    environment->set("MUSICPLAYER_BACKEND_URL", {});
    // Any accidental fallback fails locally without contacting real Redis.
    environment->set("MUSICPLAYER_REDIS_PORT", "invalid-test-port");
}

void HostedAuthDialogsTest::cleanup() { environment.reset(); }

void HostedAuthDialogsTest::configure(FakeBackend &backend)
{
    QVERIFY(backend.server.isListening());
    environment->set("MUSICPLAYER_BACKEND_URL", backend.url());
    QVERIFY(AppBackendClient::enabled());
    QVERIFY(AppBackendClient::configurationError().isEmpty());
    QVERIFY(!AppBackendClient::instance()->authenticated());
}

void HostedAuthDialogsTest::loginCreatesAppSessionAndClearsPassword()
{
    FakeBackend backend;
    backend.responseDelay = 50;
    configure(backend);
    SignIn dialog;
    credentials(dialog);
    QSignalSpy loggedIn(&dialog, &SignIn::userLoggedIn);
    auto *submit = dialog.findChild<QPushButton *>("pushButton_SignIn");
    submit->click();
    QVERIFY(!submit->isEnabled());
    QVERIFY(dialog.findChild<QLineEdit *>("lineEdit_Password")->text().isEmpty());
    submit->click();
    QTRY_COMPARE_WITH_TIMEOUT(loggedIn.count(), 1, 3000);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(dialog.isSignIn);
    QCOMPARE(loggedIn.first().first().toString(), QStringLiteral("test-user"));
    QCOMPARE(AppBackendClient::instance()->userId(), QStringLiteral("owned-user-123"));
    QCOMPARE(backend.routes.count(), 1);
    QCOMPARE(backend.routes.first(), QStringLiteral("/v1/auth/login"));
    QCOMPARE(backend.bodies.first().value("username").toString(), QStringLiteral("test-user"));
    QCOMPARE(backend.bodies.first().value("password").toString(),
             QStringLiteral("test-password-123"));
}

void HostedAuthDialogsTest::wrongPasswordNeverFallsBackToRedis()
{
    FakeBackend backend;
    backend.deny = true;
    configure(backend);
    SignIn dialog;
    credentials(dialog, QStringLiteral("wrong-password"));
    QSignalSpy loggedIn(&dialog, &SignIn::userLoggedIn);
    dialog.findChild<QPushButton *>("pushButton_SignIn")->click();
    auto *status = dialog.findChild<QLabel *>("label_AuthStatus");
    QTRY_VERIFY_WITH_TIMEOUT(!status->text().isEmpty(), 3000);
    QVERIFY(!status->text().contains(QStringLiteral("Redis")));
    QVERIFY(!AppBackendClient::instance()->authenticated());
    QCOMPARE(loggedIn.count(), 0);
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QVERIFY(dialog.findChild<QPushButton *>("pushButton_SignIn")->isEnabled());
    QVERIFY(dialog.findChild<QLineEdit *>("lineEdit_Password")->text().isEmpty());
}

void HostedAuthDialogsTest::registrationCompletesParentSignIn()
{
    FakeBackend backend;
    configure(backend);
    SignIn dialog;
    QSignalSpy loggedIn(&dialog, &SignIn::userLoggedIn);
    bool registrationStarted = false;
    bool genderHidden = false;
    QTimer registrar;
    connect(&registrar, &QTimer::timeout, &dialog,
            [&]
            {
                if (registrationStarted)
                    return;
                auto *signup = dialog.findChild<SignUp *>();
                if (!signup || !signup->isVisible() ||
                    !signup->findChild<QPushButton *>("pushButton_SignUp")->isEnabled())
                    return;
                registrationStarted = true;
                genderHidden = signup->findChild<QRadioButton *>("radioButton_man")->isHidden() &&
                               signup->findChild<QRadioButton *>("radioButton_woman")->isHidden() &&
                               signup->findChild<QLabel *>("label_4")->isHidden();
                credentials(*signup);
                signup->findChild<QPushButton *>("pushButton_SignUp")->click();
            });
    registrar.start(10);
    // Prevent a broken success path from hanging the test in the modal sign-up.
    QTimer::singleShot(3000, &dialog,
                       [&]
                       {
                           if (auto *signup = dialog.findChild<SignUp *>())
                               signup->reject();
                       });
    dialog.findChild<QPushButton *>("pushButton_SignUp")->click();
    QVERIFY(registrationStarted);
    QVERIFY(genderHidden);
    QCOMPARE(loggedIn.count(), 1);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(AppBackendClient::instance()->authenticated());
    QCOMPARE(backend.routes.count(), 2);
    QCOMPARE(backend.routes.first(), QStringLiteral("/v1/auth/config"));
    QCOMPARE(backend.routes.last(), QStringLiteral("/v1/auth/register"));
    QVERIFY(!backend.bodies.last().contains(QStringLiteral("gender")));
    QVERIFY(!backend.bodies.last().contains(QStringLiteral("inviteCode")));
}

void HostedAuthDialogsTest::closePendingPreventsLateLogin()
{
    FakeBackend backend;
    backend.responseDelay = 200;
    configure(backend);
    SignIn dialog;
    credentials(dialog);
    QSignalSpy loggedIn(&dialog, &SignIn::userLoggedIn);
    dialog.findChild<QPushButton *>("pushButton_SignIn")->click();
    QTRY_COMPARE_WITH_TIMEOUT(backend.routes.count(), 1, 3000);
    dialog.reject();
    QTest::qWait(250);
    QVERIFY(!AppBackendClient::instance()->authenticated());
    QCOMPARE(loggedIn.count(), 0);
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
}

void HostedAuthDialogsTest::closePendingRegistrationPreventsLateSession()
{
    FakeBackend backend;
    backend.responseDelay = 200;
    configure(backend);
    auto *dialog = new SignUp;
    QTRY_VERIFY_WITH_TIMEOUT(dialog->findChild<QPushButton *>("pushButton_SignUp")->isEnabled(), 3000);
    credentials(*dialog);
    dialog->findChild<QPushButton *>("pushButton_SignUp")->click();
    QTRY_COMPARE_WITH_TIMEOUT(backend.routes.count(QStringLiteral("/v1/auth/register")), 1, 3000);
    dialog->reject();
    delete dialog;
    QTest::qWait(250);
    QVERIFY(!AppBackendClient::instance()->authenticated());
}

void HostedAuthDialogsTest::invalidConfigurationNeverFallsBackToRedis()
{
    environment->set("MUSICPLAYER_BACKEND_URL", "http://example.com");
    QVERIFY(!AppBackendClient::configurationError().isEmpty());
    SignIn dialog;
    credentials(dialog);
    dialog.findChild<QPushButton *>("pushButton_SignIn")->click();
    auto *status = dialog.findChild<QLabel *>("label_AuthStatus");
    QVERIFY(!status->text().isEmpty());
    QVERIFY(!status->text().contains(QStringLiteral("Redis")));
    QVERIFY(!AppBackendClient::instance()->authenticated());
}

void HostedAuthDialogsTest::passwordVisibilityIsAccessibleAndConcealedWhenHidden_data()
{
    QTest::addColumn<bool>("signup");
    QTest::newRow("login") << false;
    QTest::newRow("registration") << true;
}

void HostedAuthDialogsTest::passwordVisibilityIsAccessibleAndConcealedWhenHidden()
{
    QFETCH(bool, signup);
    FakeBackend backend;
    if (signup) backend.registrationMode = QStringLiteral("invite_only");
    configure(backend);
    std::unique_ptr<QDialog> dialog(signup ? static_cast<QDialog *>(new SignUp) : static_cast<QDialog *>(new SignIn));
    dialog->show();
    if (signup) QTRY_VERIFY_WITH_TIMEOUT(dialog->findChild<QPushButton *>("pushButton_SignUp")->isEnabled(), 3000);
    dialog->resize(460, dialog->minimumSizeHint().height());
    QCoreApplication::processEvents();
    for (const char *name : {"lineEdit_Id", "lineEdit_Password"}) {
        auto *field = dialog->findChild<QLineEdit *>(name); QVERIFY(field);
        QVERIFY(dialog->rect().contains(QRect(field->mapTo(dialog.get(), QPoint()), field->size())));
    }
    if (signup) {
        for (const char *name : {"lineEdit_ConfirmPassword", "lineEdit_InviteCode"}) {
            auto *field = dialog->findChild<QLineEdit *>(name); QVERIFY(field && !field->isHidden());
            QVERIFY(dialog->rect().contains(QRect(field->mapTo(dialog.get(), QPoint()), field->size())));
        }
    }
    const QString screenshotDirectory = qEnvironmentVariable("TANGO_AUTH_UI_SCREENSHOT_DIR");
    if (!screenshotDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(screenshotDirectory));
        QVERIFY(dialog->grab().save(QDir(screenshotDirectory).filePath(signup ? QStringLiteral("signup-invite.png") : QStringLiteral("signin.png"))));
    }
    auto *password = dialog->findChild<QLineEdit *>("lineEdit_Password");
    auto *eye = dialog->findChild<QAction *>("action_PasswordVisibility");
    QVERIFY(password && eye); QVERIFY(eye->isCheckable());
    password->setText(QStringLiteral("fixture-password-123"));
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    eye->trigger(); QCOMPARE(password->echoMode(), QLineEdit::Normal);
    QVERIFY(password->inputMethodHints().testFlag(Qt::ImhSensitiveData));
    QVERIFY(password->inputMethodHints().testFlag(Qt::ImhNoPredictiveText));
    auto *eyeButton = password->findChild<QToolButton *>("action_PasswordVisibilityButton");
    QVERIFY(eyeButton); QVERIFY(eyeButton->accessibleName().contains(QStringLiteral("隐藏密码")));
    QVERIFY(!eyeButton->icon().isNull());
    eyeButton->click(); QCOMPARE(password->echoMode(), QLineEdit::Password);
    eyeButton->setFocus(); QTest::keyClick(eyeButton, Qt::Key_Space);
    QCOMPARE(password->echoMode(), QLineEdit::Normal);
    if (signup) {
        auto *confirm = dialog->findChild<QLineEdit *>("lineEdit_ConfirmPassword");
        auto *confirmEye = dialog->findChild<QAction *>("action_ConfirmPasswordVisibility");
        QVERIFY(confirm && confirmEye); confirmEye->trigger();
        QCOMPARE(confirm->echoMode(), QLineEdit::Normal);
    }
    dialog->hide(); QCOMPARE(password->echoMode(), QLineEdit::Password); QVERIFY(!eye->isChecked());
    if (signup) QCOMPARE(dialog->findChild<QLineEdit *>("lineEdit_ConfirmPassword")->echoMode(), QLineEdit::Password);
    dialog->show(); QCOMPARE(password->echoMode(), QLineEdit::Password);
    eye->trigger(); dialog->reject();
    QCOMPARE(password->echoMode(), QLineEdit::Password); QVERIFY(password->text().isEmpty());
}

void HostedAuthDialogsTest::mismatchedConfirmationDoesNotSubmit()
{
    FakeBackend backend; configure(backend);
    SignUp dialog;
    auto *submit = dialog.findChild<QPushButton *>("pushButton_SignUp");
    QTRY_VERIFY_WITH_TIMEOUT(submit->isEnabled(), 3000);
    credentials(dialog);
    dialog.findChild<QLineEdit *>("lineEdit_ConfirmPassword")->setText(QStringLiteral("different-fixture-password"));
    submit->click();
    QVERIFY(dialog.findChild<QLabel *>("label_AuthStatus")->text().contains(QStringLiteral("不一致")));
    QCOMPARE(backend.routes.count(QStringLiteral("/v1/auth/register")), 0);
    QVERIFY(!AppBackendClient::instance()->authenticated());
}

void HostedAuthDialogsTest::inviteRegistrationRequiresCodeAndSendsItOnce()
{
    FakeBackend backend; backend.registrationMode = QStringLiteral("invite_only"); backend.responseDelay = 30;
    configure(backend); SignUp dialog;
    auto *submit = dialog.findChild<QPushButton *>("pushButton_SignUp");
    QTRY_VERIFY_WITH_TIMEOUT(submit->isEnabled(), 3000);
    credentials(dialog);
    auto *invite = dialog.findChild<QLineEdit *>("lineEdit_InviteCode"); QVERIFY(invite && !invite->isHidden());
    submit->click(); QCOMPARE(backend.routes.count(QStringLiteral("/v1/auth/register")), 0);
    invite->setText(QStringLiteral("not-an-invite")); submit->click();
    QCOMPARE(backend.routes.count(QStringLiteral("/v1/auth/register")), 0);
    const QString code = QStringLiteral("TANGO-") + QString(32, QLatin1Char('a'));
    invite->setText(code);
    auto *eye = dialog.findChild<QAction *>("action_PasswordVisibility"); eye->trigger();
    submit->click(); submit->click();
    QVERIFY(invite->text().isEmpty());
    QVERIFY(dialog.findChild<QLineEdit *>("lineEdit_Password")->text().isEmpty());
    QVERIFY(dialog.findChild<QLineEdit *>("lineEdit_ConfirmPassword")->text().isEmpty());
    QCOMPARE(dialog.findChild<QLineEdit *>("lineEdit_Password")->echoMode(), QLineEdit::Password);
    QTRY_VERIFY_WITH_TIMEOUT(AppBackendClient::instance()->authenticated(), 3000);
    QCOMPARE(backend.routes.count(QStringLiteral("/v1/auth/register")), 1);
    const auto registration = backend.bodies.at(backend.routes.indexOf(QStringLiteral("/v1/auth/register")));
    QCOMPARE(registration.value("inviteCode").toString(), code);
    QCOMPARE(registration.keys().size(), 3);
    QVERIFY(!backend.requestHeaders.first().contains("Authorization:"));
}

void HostedAuthDialogsTest::unavailableRegistrationPolicyFailsClosed_data()
{
    QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("malformed"); QTest::addColumn<bool>("networkFailure");
    QTest::newRow("closed") << QString("closed") << false << false;
    QTest::newRow("unknown-mode") << QString("untrusted-mode") << false << false;
    QTest::newRow("malformed") << QString("open") << true << false;
    QTest::newRow("server-error") << QString("open") << false << true;
}

void HostedAuthDialogsTest::unavailableRegistrationPolicyFailsClosed()
{
    QFETCH(QString, mode); QFETCH(bool, malformed); QFETCH(bool, networkFailure);
    FakeBackend backend; backend.registrationMode = mode; backend.malformedConfig = malformed; backend.failedConfig = networkFailure;
    configure(backend); SignUp dialog;
    auto *submit = dialog.findChild<QPushButton *>("pushButton_SignUp");
    QTRY_COMPARE_WITH_TIMEOUT(backend.routes.count(QStringLiteral("/v1/auth/config")), 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!submit->text().contains(QStringLiteral("读取")), 3000);
    QVERIFY(!submit->isEnabled()); credentials(dialog); submit->click();
    QCOMPARE(backend.routes.count(QStringLiteral("/v1/auth/register")), 0);
    QVERIFY(dialog.findChild<QLineEdit *>("lineEdit_InviteCode")->isHidden());
    QVERIFY(!AppBackendClient::instance()->authenticated());
    if (mode == QStringLiteral("closed")) QVERIFY(submit->text().contains(QStringLiteral("未开放")));
    else QVERIFY(!dialog.findChild<QPushButton *>("pushButton_RetryRegistrationPolicy")->isHidden());
}

void HostedAuthDialogsTest::failedPolicyCanBeRetried()
{
    FakeBackend backend; backend.failedConfig = true; configure(backend); SignUp dialog;
    auto *retry = dialog.findChild<QPushButton *>("pushButton_RetryRegistrationPolicy");
    QTRY_VERIFY_WITH_TIMEOUT(!retry->isHidden(), 3000);
    QVERIFY(!dialog.findChild<QPushButton *>("pushButton_SignUp")->isEnabled());
    backend.failedConfig = false; retry->click();
    QTRY_VERIFY_WITH_TIMEOUT(dialog.findChild<QPushButton *>("pushButton_SignUp")->isEnabled(), 3000);
    QVERIFY(retry->isHidden());
    QCOMPARE(backend.routes.count(QStringLiteral("/v1/auth/config")), 2);
}

void HostedAuthDialogsTest::advertisedPasswordMinimumControlsRegistration_data()
{
    QTest::addColumn<int>("minimum");
    QTest::newRow("compatibility-eight") << 8;
    QTest::newRow("production-fifteen") << 15;
    QTest::newRow("dynamic-twenty") << 20;
}

void HostedAuthDialogsTest::advertisedPasswordMinimumControlsRegistration()
{
    QFETCH(int, minimum);
    FakeBackend backend; backend.passwordMinimum = minimum; configure(backend); SignUp dialog;
    auto *submit = dialog.findChild<QPushButton *>("pushButton_SignUp");
    QTRY_VERIFY_WITH_TIMEOUT(submit->isEnabled(), 3000);
    auto *password = dialog.findChild<QLineEdit *>("lineEdit_Password");
    QVERIFY(password->placeholderText().contains(QString::number(minimum)));
    credentials(dialog, QString(minimum - 1, QLatin1Char('a'))); submit->click();
    QCOMPARE(backend.routes.count(QStringLiteral("/v1/auth/register")), 0);
    credentials(dialog, QString(minimum, QLatin1Char('a'))); submit->click();
    QTRY_COMPARE_WITH_TIMEOUT(backend.routes.count(QStringLiteral("/v1/auth/register")), 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(AppBackendClient::instance()->authenticated(), 3000);
}

void HostedAuthDialogsTest::closingPendingPolicyPreventsLateEnable()
{
    FakeBackend backend; backend.responseDelay = 150; configure(backend); SignUp dialog; dialog.show();
    QTRY_COMPARE_WITH_TIMEOUT(backend.routes.count(QStringLiteral("/v1/auth/config")), 1, 3000);
    dialog.reject(); QTest::qWait(200);
    QVERIFY(!dialog.findChild<QPushButton *>("pushButton_SignUp")->isEnabled());
    QCOMPARE(backend.routes.count(QStringLiteral("/v1/auth/register")), 0);
    QVERIFY(!AppBackendClient::instance()->authenticated());
}

void HostedAuthDialogsTest::accountUsageUsesAuthenticatedSession()
{
    FakeBackend backend; configure(backend); QObject context;
    bool loggedIn = false; QString failure;
    auto *client = AppBackendClient::instance();
    client->login(QStringLiteral("test-user"), QStringLiteral("fixture-password-123"), &context,
        [&](const QString &error) { failure = error; loggedIn = true; });
    QTRY_VERIFY_WITH_TIMEOUT(loggedIn, 3000); QVERIFY(failure.isEmpty());
    bool received = false; QJsonObject usage;
    client->accountUsage(&context, [&](const QJsonObject &data, const QString &error) { usage = data; failure = error; received = true; });
    QTRY_VERIFY_WITH_TIMEOUT(received, 3000); QVERIFY(failure.isEmpty());
    QCOMPARE(usage.value("daily").toObject().value("remaining").toInt(), 17);
    const int index = backend.routes.indexOf(QStringLiteral("/v1/account/usage")); QVERIFY(index >= 0);
    QVERIFY(backend.requestHeaders.at(index).contains("Authorization: Bearer fake-owned-session-token"));
    QVERIFY(backend.bodies.at(index).isEmpty());
}

void HostedAuthDialogsTest::backendChangeRequiresNewPolicyBeforeRegistration()
{
    FakeBackend first, second;
    second.registrationMode = QStringLiteral("invite_only"); second.responseDelay = 30;
    configure(first); SignUp dialog;
    auto *submit = dialog.findChild<QPushButton *>("pushButton_SignUp");
    QTRY_VERIFY_WITH_TIMEOUT(submit->isEnabled(), 3000);
    credentials(dialog);
    environment->set("MUSICPLAYER_BACKEND_URL", second.url());
    submit->click();
    QVERIFY(!submit->isEnabled());
    QCOMPARE(first.routes.count(QStringLiteral("/v1/auth/register")), 0);
    QCOMPARE(second.routes.count(QStringLiteral("/v1/auth/register")), 0);
    QTRY_VERIFY_WITH_TIMEOUT(submit->isEnabled(), 3000);
    auto *invite = dialog.findChild<QLineEdit *>("lineEdit_InviteCode"); QVERIFY(invite && !invite->isHidden());
    submit->click();
    QCOMPARE(second.routes.count(QStringLiteral("/v1/auth/register")), 0);
    QVERIFY(!AppBackendClient::instance()->authenticated());
}

QTEST_MAIN(HostedAuthDialogsTest)
#include "hosted_auth_dialogs_test.moc"
