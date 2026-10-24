#include "neteaseclient.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
class ProfileTest : public QObject {
 Q_OBJECT
private slots:
 void catalogEpochFollowsAuthorization() {
  QTemporaryDir dir; QVERIFY(dir.isValid()); QDir(dir.path()).mkpath("netease");
  QFile token(dir.path()+"/netease/token"); QVERIFY(token.open(QIODevice::WriteOnly)); token.write(QByteArray(64,'a')); token.close();
  qputenv("MUSICPLAYER_DATA_DIR",dir.path().toUtf8());
  QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,0));
  qputenv("MUSICPLAYER_NETEASE_PORT",QByteArray::number(server.serverPort()));
  int qrCode=802;
  connect(&server,&QTcpServer::newConnection,this,[&]{
   while(server.hasPendingConnections()) {
    auto *socket=server.nextPendingConnection();
    connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
    connect(socket,&QTcpSocket::readyRead,socket,[&,socket]{
     if(socket->property("handled").toBool())return;
     auto bytes=socket->property("data").toByteArray()+socket->readAll();socket->setProperty("data",bytes);
     const int end=bytes.indexOf("\r\n\r\n");if(end<0)return;
     int length=0;for(const auto &line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(line.indexOf(':')+1).trimmed().toInt();
     if(bytes.size()<end+4+length)return;socket->setProperty("handled",true);
     QJsonObject object;
     if(bytes.startsWith("GET /health "))object={{"app","quiet-netease"},{"version",1},{"catalogVersion",2},{"profileIsolation",1}};
     else if(bytes.startsWith("POST /login/qr/check "))object={{"code",qrCode}};
     else object={{"loggedIn",false}};
     const auto body=QJsonDocument(object).toJson(QJsonDocument::Compact);
     socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();
    });
   }
  });
  NeteaseClient::setAccountProfile("epoch-fixture-alice");
  const auto initial=NeteaseClient::catalogCacheContext();
  QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f-]{36}$")).match(initial).hasMatch());
  NeteaseClient::setAccountProfile("epoch-fixture-alice");QCOMPARE(NeteaseClient::catalogCacheContext(),initial);
  NeteaseClient client;QObject context;bool done=false;QString failure;
  const auto callback=[&](const QJsonObject &,const QString &error){failure=error;done=true;};
  client.post("/login/qr/check",{},&context,callback);QTRY_VERIFY(done);QVERIFY(failure.isEmpty());
  QCOMPARE(NeteaseClient::catalogCacheContext(),initial);
  qrCode=803;done=false;client.post("/login/qr/check",{},&context,callback);QTRY_VERIFY(done);QVERIFY(failure.isEmpty());
  const auto authorized=NeteaseClient::catalogCacheContext();QVERIFY(authorized!=initial);
  done=false;client.post("/logout",{},&context,callback);QTRY_VERIFY(done);QVERIFY(failure.isEmpty());
  const auto loggedOut=NeteaseClient::catalogCacheContext();QVERIFY(loggedOut!=authorized);
  NeteaseClient::setAccountProfile("epoch-fixture-bob");QVERIFY(NeteaseClient::catalogCacheContext()!=loggedOut);
  NeteaseClient::setAccountProfile("guest");
 }
 void headerAndGuest() { run(false, false); }
 void switchedResponseDiscarded() { run(true, false); }
 void switchedBeforeReadyDiscarded() { run(false, true); }
 void oldServiceCannotReceiveAccountRequest() { run(false, false, true); }
 void outdatedCatalogCannotReceiveMetadataRequest() { run(false, false, false, true); }
private:
 void run(bool delayApi, bool delayHealth, bool oldService = false, bool oldCatalog = false) {
  QTemporaryDir dir; QVERIFY(dir.isValid()); QDir(dir.path()).mkpath("netease");
  QFile token(dir.path()+"/netease/token"); QVERIFY(token.open(QIODevice::WriteOnly)); token.write(QByteArray(64,'a')); token.close();
  qputenv("MUSICPLAYER_DATA_DIR", dir.path().toUtf8());
  QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,0));
  qputenv("MUSICPLAYER_NETEASE_PORT",QByteArray::number(server.serverPort()));
  QList<QByteArray> requests;
  connect(&server,&QTcpServer::newConnection,this,[&]{
   while(server.hasPendingConnections()) {
    auto *socket=server.nextPendingConnection();
    connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
    connect(socket,&QTcpSocket::readyRead,this,[&,socket]{
     if(socket->property("handled").toBool())return;
     auto data=socket->property("data").toByteArray()+socket->readAll();socket->setProperty("data",data);
     if(!data.contains("\r\n\r\n"))return; socket->setProperty("handled",true);
     bool health=data.startsWith("GET /health "); if(!health)requests.append(data);
     auto reply=[socket,health,oldService,oldCatalog]{ if(socket->state()==QAbstractSocket::UnconnectedState)return;
      QByteArray body=health?(oldService?"{\"app\":\"quiet-netease\",\"version\":1,\"catalogVersion\":2}":"{\"app\":\"quiet-netease\",\"version\":1,\"catalogVersion\":2,\"profileIsolation\":1}"):"{\"ok\":true}";
      if (health && oldCatalog) body="{\"app\":\"quiet-netease\",\"version\":1,\"profileIsolation\":1}";
      socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();};
     if((health&&delayHealth)||(!health&&delayApi))QTimer::singleShot(80,socket,reply);else reply();
    });
   }
  });
  NeteaseClient::setAccountProfile("server:fixture:alice");
  const auto hash=QCryptographicHash::hash(QByteArray("server:fixture:alice"),QCryptographicHash::Sha256).toHex();
  QCOMPARE(NeteaseClient::accountProfile().toLatin1(),hash);
  NeteaseClient client; QObject context; bool called=false; QString failure;
  client.get("/search",{},&context,[&](const QJsonObject &value,const QString &error){failure=error; if(!oldService&&!oldCatalog){QVERIFY(error.isEmpty());QVERIFY(value["ok"].toBool());}called=true;});
  if(oldService||oldCatalog) {QTRY_VERIFY(called);QVERIFY(!failure.isEmpty());if(oldCatalog)QVERIFY(failure.contains(QStringLiteral("旧版")));QCOMPARE(requests.size(),0);}
  else if(delayHealth) {
   NeteaseClient::setAccountProfile("server:fixture:bob");QTest::qWait(150);QVERIFY(!called);QCOMPARE(requests.size(),0);
  } else {
   QTRY_COMPARE(requests.size(),1); QVERIFY(requests.first().contains("X-MusicPlayer-Profile: "+hash));QVERIFY(!requests.first().contains("server:fixture:alice"));
   if(delayApi) {NeteaseClient::setAccountProfile("server:fixture:bob");NeteaseClient::setAccountProfile("server:fixture:alice");QTest::qWait(150);QVERIFY(!called);}
   else {QTRY_VERIFY(called);NeteaseClient::setAccountProfile("guest");called=false;client.get("/search",{},&context,[&](const QJsonObject &,const QString &error){QVERIFY(error.isEmpty());called=true;});QTRY_VERIFY(called);QCOMPARE(requests.size(),2);QVERIFY(!requests.last().contains("X-MusicPlayer-Profile:"));}
  }
  NeteaseClient::setAccountProfile("guest");
 }
};
QTEST_GUILESS_MAIN(ProfileTest)
#include "netease_profile_test.moc"
