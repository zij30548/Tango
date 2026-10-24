#include "appbackendclient.h"
#include "development_session_fixtures.h"
#include <QtTest>
#include <QElapsedTimer>
#include <QNetworkProxy>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace DevelopmentFixtures;
class DevelopmentSessionClientTest : public QObject {
    Q_OBJECT
private slots:
    void successfulLocalBootstrapIsOneSecretRequestOnly() {
        Fixture fixture; AppBackendClient client(nullptr, 500); QObject context;
        bool done = false; QString error;
        client.bootstrapDevelopmentSession(&context, [&](const QString &value) { error=value; done=true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 2000); QVERIFY2(error.isEmpty(), qPrintable(error)); QVERIFY(client.authenticated());
        QCOMPARE(client.userId(), QString("development-fixture-user")); QCOMPARE(fixture.backend.requests.size(), 1);
        const auto first=fixture.backend.requests.first(); QCOMPARE(first.route, QString("/v1/dev/session"));
        QVERIFY(first.body.isEmpty()); QVERIFY(first.headers.contains("X-Tango-Development-Token: " + fixture.backend.capability));
        done=false; client.refreshAgentStatus(&context, [&](const QString &value){error=value;done=true;});
        QTRY_VERIFY_WITH_TIMEOUT(done,2000); QVERIFY(error.isEmpty()); QVERIFY(client.agentReady());
        done=false; client.accountUsage(&context,[&](const QJsonObject &,const QString &value){error=value;done=true;});
        QTRY_VERIFY_WITH_TIMEOUT(done,2000); QVERIFY(error.isEmpty());
        for(int index=1;index<fixture.backend.requests.size();++index) {
            QVERIFY(!fixture.backend.requests.at(index).headers.contains("X-Tango-Development-Token"));
            QVERIFY(!fixture.backend.requests.at(index).headers.contains(fixture.backend.capability));
        }
        done=false; client.bootstrapDevelopmentSession(&context,[&](const QString &value){error=value;done=true;});
        QVERIFY(done); QVERIFY(error.isEmpty()); QCOMPARE(fixture.backend.count("/v1/dev/session"),1);
    }
    void developmentModeCannotBorrowRealAccountOnFailure() {
        Fixture fixture; fixture.environment.set("MUSICPLAYER_DEVELOPMENT_MODE", "");
        AppBackendClient client(nullptr,500); QObject context; bool done=false; QString error;
        client.login("owner-fixture", "owned-fixture-password", &context,[&](const QString &value){error=value;done=true;});
        QTRY_VERIFY_WITH_TIMEOUT(done,2000); QVERIFY(error.isEmpty()); QCOMPARE(client.userId(),QString("owner-fixture-user"));
        fixture.environment.set("MUSICPLAYER_DEVELOPMENT_MODE", "1"); QVERIFY(!client.authenticated());
        fixture.backend.rejectDevelopment=true; done=false;
        client.bootstrapDevelopmentSession(&context,[&](const QString &value){error=value;done=true;});
        QTRY_VERIFY_WITH_TIMEOUT(done,2000); QVERIFY(!error.isEmpty()); QVERIFY(!client.authenticated()); QVERIFY(client.userId().isEmpty());
        QVERIFY(!error.contains(QString::fromLatin1(fixture.backend.capability))); QCOMPARE(fixture.backend.count("/v1/auth/logout"),0);
        done=false; client.login("owner-fixture","owned-fixture-password",&context,[&](const QString &value){error=value;done=true;});
        QVERIFY(done); QVERIFY(!error.isEmpty()); QCOMPARE(fixture.backend.count("/v1/auth/login"),1);
    }
    void bootstrapRequiresExplicitLiteralLoopback_data() {
        QTest::addColumn<QByteArray>("mode"); QTest::addColumn<QByteArray>("url");
        QTest::newRow("flag-off") << QByteArray() << QByteArray("fixture");
        QTest::newRow("nonexact-flag") << QByteArray("true") << QByteArray("fixture");
        QTest::newRow("localhost-not-literal") << QByteArray("1") << QByteArray("http://localhost:9");
        QTest::newRow("remote-https") << QByteArray("1") << QByteArray("https://example.invalid");
        QTest::newRow("path-prefix") << QByteArray("1") << QByteArray("fixture/prefix");
        QTest::newRow("missing-backend") << QByteArray("1") << QByteArray();
    }
    void bootstrapRequiresExplicitLiteralLoopback() {
        QFETCH(QByteArray,mode); QFETCH(QByteArray,url); Fixture fixture;
        fixture.environment.set("MUSICPLAYER_DEVELOPMENT_MODE",mode);
        if(url.startsWith("fixture"))url.replace("fixture",fixture.backend.url());
        fixture.environment.set("MUSICPLAYER_BACKEND_URL",url); AppBackendClient client(nullptr,300); QObject context;
        bool done=false; QString error; client.bootstrapDevelopmentSession(&context,[&](const QString &value){error=value;done=true;});
        QVERIFY(done); QVERIFY(!error.isEmpty()); QVERIFY(!client.authenticated()); QCOMPARE(fixture.backend.requests.size(),0);
    }
    void invalidTokenFilesNeverReachBackend_data() {
        QTest::addColumn<QString>("kind");
        for(const auto &kind:{"relative","missing","symlink","public-read","group-write","short","uppercase","extra-data","directory","hardlink","fifo"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void invalidTokenFilesNeverReachBackend() {
        QFETCH(QString,kind); Fixture fixture;
        if(kind=="relative")fixture.environment.set("MUSICPLAYER_DEVELOPMENT_TOKEN_FILE","relative-token");
        else if(kind=="missing")fixture.environment.set("MUSICPLAYER_DEVELOPMENT_TOKEN_FILE",fixture.directory.filePath("absent").toUtf8());
        else if(kind=="public-read")QVERIFY(QFile::setPermissions(fixture.tokenPath,QFile::ReadOwner|QFile::WriteOwner|QFile::ReadOther));
        else if(kind=="group-write")QVERIFY(QFile::setPermissions(fixture.tokenPath,QFile::ReadOwner|QFile::WriteOwner|QFile::WriteGroup));
        else if(kind=="short")QVERIFY(fixture.writeToken(QByteArray(32,'a')));
        else if(kind=="uppercase")QVERIFY(fixture.writeToken(QByteArray(64,'A')));
        else if(kind=="extra-data")QVERIFY(fixture.writeToken(QByteArray(64,'a')+"\n\n"));
        else if(kind=="directory")fixture.environment.set("MUSICPLAYER_DEVELOPMENT_TOKEN_FILE",fixture.directory.path().toUtf8());
#ifdef Q_OS_UNIX
        else if(kind=="symlink"||kind=="hardlink") {
            const auto link=fixture.directory.filePath("linked-token");
            const auto source=QFile::encodeName(fixture.tokenPath), target=QFile::encodeName(link);
            QCOMPARE(kind=="symlink" ? ::symlink(source.constData(),target.constData()) : ::link(source.constData(),target.constData()),0);
            fixture.environment.set("MUSICPLAYER_DEVELOPMENT_TOKEN_FILE",link.toUtf8());
        } else if(kind=="fifo") {
            QVERIFY(QFile::remove(fixture.tokenPath)); QCOMPARE(::mkfifo(QFile::encodeName(fixture.tokenPath).constData(),0600),0);
        }
#endif
        AppBackendClient client(nullptr,300); QObject context; bool done=false; QString error; QElapsedTimer timer; timer.start();
        client.bootstrapDevelopmentSession(&context,[&](const QString &value){error=value;done=true;});
        QVERIFY(done); QVERIFY(timer.elapsed()<500); QVERIFY(!error.isEmpty()); QVERIFY(!client.authenticated()); QCOMPARE(fixture.backend.requests.size(),0);
    }
    void responseMustProveDevelopmentIdentity_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("missing-flag")<<0;QTest::newRow("false-flag")<<1;QTest::newRow("string-flag")<<2;QTest::newRow("expired")<<3;
    }
    void responseMustProveDevelopmentIdentity() {
        QFETCH(int,kind); Fixture fixture;
        if(kind==0)fixture.backend.omitDevelopment=true;else if(kind==1)fixture.backend.developmentValue=false;
        else if(kind==2)fixture.backend.developmentValue="true";else fixture.backend.expired=true;
        AppBackendClient client(nullptr,500); QObject context; bool done=false; QString error;
        client.bootstrapDevelopmentSession(&context,[&](const QString &value){error=value;done=true;});
        QTRY_VERIFY_WITH_TIMEOUT(done,2000); QVERIFY(!error.isEmpty()); QVERIFY(!client.authenticated()); QCOMPARE(fixture.backend.requests.size(),1);
    }
    void redirectDoesNotForwardCapability() {
        Fixture fixture; Backend collector; fixture.backend.redirectedTo=collector.url()+"/collect";
        AppBackendClient client(nullptr,500); QObject context; bool done=false; QString error;
        client.bootstrapDevelopmentSession(&context,[&](const QString &value){error=value;done=true;});
        QTRY_VERIFY_WITH_TIMEOUT(done,2000); QVERIFY(!error.isEmpty()); QVERIFY(!client.authenticated()); QCOMPARE(collector.requests.size(),0);
    }
    void localCapabilityBypassesApplicationProxy() {
        Fixture fixture; Backend proxy;
        struct Restore {QNetworkProxy old=QNetworkProxy::applicationProxy();~Restore(){QNetworkProxy::setApplicationProxy(old);}} restore;
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy,"127.0.0.1",proxy.server.serverPort()));
        AppBackendClient client(nullptr,500); QObject context; bool done=false; QString error;
        client.bootstrapDevelopmentSession(&context,[&](const QString &value){error=value;done=true;});
        QTRY_VERIFY_WITH_TIMEOUT(done,2000); QVERIFY(error.isEmpty()); QVERIFY(client.authenticated()); QCOMPARE(proxy.requests.size(),0);
    }
    void destroyedContextPreventsLateIdentity() {
        Fixture fixture; fixture.backend.delay=100; AppBackendClient client(nullptr,500); auto *context=new QObject; bool callback=false;
        client.bootstrapDevelopmentSession(context,[&](const QString &){callback=true;});
        QTRY_COMPARE_WITH_TIMEOUT(fixture.backend.requests.size(),1,2000); delete context; QTest::qWait(150);
        QVERIFY(!callback); QVERIFY(!client.authenticated());
    }
    void leavingDevelopmentModeRevokesLocalAccess() {
        Fixture fixture; AppBackendClient client(nullptr,500); QObject context; bool done=false;
        client.bootstrapDevelopmentSession(&context,[&](const QString &){done=true;});QTRY_VERIFY_WITH_TIMEOUT(done,2000);QVERIFY(client.authenticated());
        fixture.environment.set("MUSICPLAYER_DEVELOPMENT_MODE",""); QVERIFY(!client.authenticated());
        done=false; QString error; client.me(&context,[&](const QString &value){error=value;done=true;});
        QVERIFY(done);QVERIFY(!error.isEmpty());QCOMPARE(fixture.backend.requests.size(),1);
    }
};
QTEST_GUILESS_MAIN(DevelopmentSessionClientTest)
#include "development_session_client_test.moc"
