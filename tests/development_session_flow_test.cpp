#include "appbackendclient.h"
#include "mainwindow.h"
#include "radiodialog.h"
#include "signin.h"
#include "development_session_fixtures.h"
#include <QtTest>
#include <QAction>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSqlDatabase>
#include <QStackedWidget>
using namespace DevelopmentFixtures;
class DevelopmentSessionFlowTest : public QObject {
    Q_OBJECT
private slots:
    void cleanup() {
        // This suite creates and closes only its own isolated profile database.
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        if(QSqlDatabase::contains()) {QSqlDatabase::database(QSqlDatabase::defaultConnection,false).close();QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);}
        AppBackendClient::instance();
    }
    void startupEntersDevelopmentIdentityAndAgentInputWithoutLoginOrModel() {
        Fixture fixture; MainWindow window; window.show();
        auto *client=AppBackendClient::instance(); QTRY_VERIFY_WITH_TIMEOUT(client->authenticated()&&client->agentReady(),3000);
        QVERIFY(window.accountProfile().startsWith("server:")); QVERIFY(window.accountProfile().endsWith("development-fixture-user"));
        auto *label=window.findChild<QLabel*>("label_DisplayUser");QVERIFY(label);QCOMPARE(label->text(),QString("本机开发测试"));
        QVERIFY(window.findChild<QPushButton*>("pushButton_SignIn")->isHidden());
        QVERIFY(!window.findChild<QAction*>("actionLocalLogout")->isVisible());
        QVERIFY(!window.findChild<SignIn*>());
        QVERIFY(QMetaObject::invokeMethod(&window,"openRadio",Qt::DirectConnection));
        auto *radio=window.findChild<RadioDialog*>();QVERIFY(radio);QVERIFY(radio->isVisible());
        auto *input=radio->findChild<QPlainTextEdit*>("edit_RadioInstruction");QVERIFY(input&&input->isEnabled());
        input->setPlainText(QStringLiteral("阅读时听安静的歌，先给我预览"));
        QVERIFY(radio->findChild<QPushButton*>("button_RadioGenerate")->isEnabled());
        QCOMPARE(fixture.backend.count("/v1/dev/session"),1);
        QCOMPARE(fixture.backend.count("/v1/auth/login"),0);QCOMPARE(fixture.backend.count("/v1/auth/register"),0);
        for(const auto &request:fixture.backend.requests) {
            if(request.headers.startsWith("POST"))QCOMPARE(request.route,QString("/v1/dev/session"));
            if(request.route!="/v1/dev/session")QVERIFY(!request.headers.contains(fixture.backend.capability));
        }
    }
    void failureRetainsDevelopmentBoundaryAndCanBeRetried() {
        Fixture fixture;fixture.backend.rejectDevelopment=true;
        MainWindow window;window.show();auto *retry=window.findChild<QPushButton*>("button_DevelopmentRetry");QVERIFY(retry);
        QTRY_VERIFY_WITH_TIMEOUT(!retry->isHidden()&&retry->isEnabled(),3000);QVERIFY(!AppBackendClient::instance()->authenticated());
        QVERIFY(window.findChild<QPushButton*>("pushButton_SignIn")->isHidden());
        QVERIFY(!window.findChild<SignIn*>());
        const auto labels = window.findChildren<QLabel*>(); QVERIFY(!labels.isEmpty());
        for (const auto *label : labels)
            QVERIFY(!label->text().contains(QString::fromLatin1(fixture.backend.capability)));
        fixture.backend.rejectDevelopment=false;retry->click();
        QTRY_VERIFY_WITH_TIMEOUT(AppBackendClient::instance()->authenticated()&&AppBackendClient::instance()->agentReady(),3000);
        QVERIFY(retry->isHidden());QCOMPARE(fixture.backend.count("/v1/dev/session"),2);QCOMPARE(fixture.backend.count("/v1/auth/login"),0);
        // The shared login slot is also guarded; it never opens a real-account dialog.
        QVERIFY(QMetaObject::invokeMethod(&window,"on_pushButton_SignIn_clicked",Qt::DirectConnection));
        QVERIFY(!window.findChild<SignIn*>());QCOMPARE(fixture.backend.count("/v1/auth/login"),0);
    }
    void windowDestructionDiscardsPendingIdentity() {
        Fixture fixture;fixture.backend.delay=120;
        auto *window=new MainWindow;window->show();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.backend.count("/v1/dev/session"),1,3000);delete window;QTest::qWait(180);
        QVERIFY(!AppBackendClient::instance()->authenticated());
    }
    void productionModeDoesNotBootstrapOrHideLogin() {
        Fixture fixture;fixture.environment.set("MUSICPLAYER_DEVELOPMENT_MODE","");
        MainWindow window;window.show();QTest::qWait(40);
        QVERIFY(!AppBackendClient::instance()->authenticated());
        QVERIFY(!window.findChild<QPushButton*>("pushButton_SignIn")->isHidden());
        QCOMPARE(fixture.backend.count("/v1/dev/session"),0);QVERIFY(!window.findChild<QPushButton*>("button_DevelopmentRetry"));
    }
};
QTEST_MAIN(DevelopmentSessionFlowTest)
#include "development_session_flow_test.moc"
