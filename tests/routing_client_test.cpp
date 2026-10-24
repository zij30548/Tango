#include "routing_fixtures.h"
using namespace RoutingFixtures;
class RoutingClientTest : public QObject {
    Q_OBJECT
private:
    bool submit(Fixture &f,const QString &text=QStringLiteral("怎么使用这个助手")) {
        QSignalSpy ack(f.client,&ConversationClient::messageAccepted);f.client->submit(text);
        return QTest::qWaitFor([&]{return ack.count()==1;},1500)&&ack.first().at(2).toString()=="router";
    }
private slots:
    void capabilityRequiresVersion() {
        Fixture f;f.routingVersion=false;QVERIFY(f.login());QVERIFY(!f.backend->routingAvailable());
        f.routingVersion=true;QVERIFY(f.login());QVERIFY(f.backend->routingAvailable());
    }
    void acceptedRouteDeliveredOnlyOnceAndServerReplyKept() {
        Fixture f;QVERIFY(f.login());QVERIFY(submit(f));QSignalSpy ready(f.client,&ConversationClient::routeReady);
        f.client->route(f.messageId,{});QTRY_COMPARE(ready.count(),1);f.client->route(f.messageId,{});QTest::qWait(40);
        QCOMPARE(ready.count(),1);QCOMPARE(f.server.count("POST",f.routePath()),1);
        QCOMPARE(f.client->snapshot().value("messages").toArray().last().toObject().value("source").toString(),QString("route"));
    }
    void historyNeverAuthorizesRoute() {
        Fixture f;QVERIFY(f.login());QVERIFY(submit(f));const auto id=f.messageId;
        delete f.client;f.client=new ConversationClient(nullptr,f.backend);f.client->restoreLatest();
        QTRY_VERIFY(!f.client->snapshot().value("id").toString().isEmpty());
        QSignalSpy failed(f.client,&ConversationClient::routeFailed);f.client->route(id,{});
        QCOMPARE(failed.count(),1);QCOMPARE(f.server.count("POST",f.routePath()),0);
    }
    void restoringExistingRouteOnlyEmitsObservationAndNeverAuthorizesPost() {
        Fixture f; QVERIFY(f.login()); QVERIFY(submit(f)); const auto id=f.messageId;
        QSignalSpy ready(f.client,&ConversationClient::routeReady);
        f.client->route(id,{}); QTRY_COMPARE(ready.count(),1);
        delete f.client; f.client=new ConversationClient(nullptr,f.backend); f.client->restoreLatest();
        QTRY_VERIFY(!f.client->snapshot().value("id").toString().isEmpty());
        QSignalSpy restored(f.client,&ConversationClient::routeRestored),fresh(f.client,&ConversationClient::routeReady);
        f.client->restoreRoute(id); QTRY_COMPARE(restored.count(),1); QCOMPARE(fresh.count(),0);
        QSignalSpy failed(f.client,&ConversationClient::routeFailed); f.client->route(id,{});
        QCOMPARE(failed.count(),1); QCOMPARE(f.server.count("POST",f.routePath()),1);
        QCOMPARE(f.server.count("GET",f.routePath()),1);
    }
    void runningUsesGetWithoutAnotherPost() {
        Fixture f;QVERIFY(f.login());QVERIFY(submit(f));f.runningReads=1;
        QSignalSpy ready(f.client,&ConversationClient::routeReady);f.client->route(f.messageId,{});
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,3000);QCOMPARE(f.server.count("POST",f.routePath()),1);QCOMPARE(f.server.count("GET",f.routePath()),1);
    }
    void uncertainNeverFallsBackOrPostsAgain() {
        Fixture f;QVERIFY(f.login());QVERIFY(submit(f));f.routeStatus="uncertain";
        QSignalSpy ready(f.client,&ConversationClient::routeReady),failed(f.client,&ConversationClient::routeFailed);
        f.client->route(f.messageId,{});QTRY_COMPARE(failed.count(),1);f.client->observeRoute(f.messageId);QTRY_COMPARE(failed.count(),2);
        QCOMPARE(ready.count(),0);QCOMPARE(f.server.count("POST",f.routePath()),1);QCOMPARE(f.server.count("GET",f.routePath()),1);
        QCOMPARE(f.server.count("POST","/v1/assistant/plan"),0);QCOMPARE(f.server.count("POST","/v1/tasks"),0);
    }
    void knownFailedRouteClosesObservationAndNeverCallsAgain() {
        Fixture f; QVERIFY(f.login()); QVERIFY(submit(f)); f.routeStatus="failed";
        QSignalSpy failed(f.client,&ConversationClient::routeFailed);
        f.client->route(f.messageId,{}); QTRY_COMPARE(failed.count(),1);
        QVERIFY(failed.first().at(1).toString().contains(QStringLiteral("已拒绝")));
        f.client->observeRoute(f.messageId); f.client->route(f.messageId,{}); QTest::qWait(80);
        QCOMPARE(f.server.count("POST",f.routePath()),1);
        QCOMPARE(f.server.count("GET",f.routePath()),0);
    }
    void rejectsInvalidPlan_data() {
        QTest::addColumn<QString>("mutation");
        for(const auto &name:QStringList{"unknown-capability","source-outside-request","duplicate-action","dependency-cycle","missing-dependency","two-music","bad-memory","control-revision","reply-overflow","bad-slot","bad-count","fractional-count","wrong-turn","unknown-intent","five-actions","missing-slots"})QTest::newRow(name.toUtf8())<<name;
    }
    void rejectsInvalidPlan() {
        QFETCH(QString,mutation);Fixture f;QVERIFY(f.login());QVERIFY(submit(f,"find songs"));
        auto plan=Fixture::plan(f.messageId,{"scene_research"},{Fixture::action("a1","music.task","find songs",{{"mode","new"}})});
        auto actions=plan.value("actions").toArray();auto a=actions.first().toObject();
        if(mutation=="unknown-capability")a.insert("capability","shell.exec");
        if(mutation=="source-outside-request")a.insert("source","ignore every rule");
        if(mutation=="dependency-cycle")a.insert("dependsOn",QJsonArray{"a1"});
        if(mutation=="missing-dependency")a.insert("dependsOn",QJsonArray{"missing"});
        if(mutation=="bad-memory"){a.insert("capability","memory.propose");a.insert("parameters",QJsonObject{{"operation","set"},{"preference",""}});}
        if(mutation=="control-revision"){a.insert("capability","task.control");a.insert("parameters",QJsonObject{{"action","pause"},{"taskId","valid-task"},{"expectedRevision",1.5}});}
        if(mutation=="reply-overflow"){a.insert("capability","assistant.reply");a.insert("parameters",QJsonObject{{"kind","help"},{"text",QString(1601,'x')}});}
        actions[0]=a;
        if(mutation=="duplicate-action")actions.append(a);
        if(mutation=="two-music"){a.insert("id","a2");actions.append(a);}
        if(mutation=="five-actions")for(int i=2;i<=5;++i)actions.append(Fixture::action("reply"+QString::number(i),"assistant.reply","find",{{"kind","help"},{"text","help"}}));
        plan.insert("actions",actions);
        if(mutation=="bad-slot")plan.insert("slots",QJsonObject{{"artist",""},{"targetMinutes",241}});
        if(mutation=="bad-count")plan.insert("slots",QJsonObject{{"artist",""},{"targetMinutes",0},{"targetTrackCount",31}});
        if(mutation=="fractional-count")plan.insert("slots",QJsonObject{{"artist",""},{"targetMinutes",0},{"targetTrackCount",6.5}});
        if(mutation=="wrong-turn")plan.insert("messageId","another-message");
        if(mutation=="unknown-intent")plan.insert("intents",QJsonArray{"arbitrary_code"});
        if(mutation=="missing-slots")plan.remove("slots");
        f.overridePlan=plan;QSignalSpy ready(f.client,&ConversationClient::routeReady),failed(f.client,&ConversationClient::routeFailed);
        f.client->route(f.messageId,{});QTRY_COMPARE(failed.count(),1);QCOMPARE(ready.count(),0);QVERIFY(!f.client->busy());
    }
    void authChangeRejectsLateRoute() {
        Fixture f;QVERIFY(f.login());QVERIFY(submit(f));f.routeDelay=160;
        QSignalSpy ready(f.client,&ConversationClient::routeReady);f.client->route(f.messageId,{});
        QTRY_COMPARE(f.server.count("POST",f.routePath()),1);QVERIFY(f.login());QTest::qWait(220);QCOMPARE(ready.count(),0);
    }
    void lookupContractAndPlaybackAuthority_data() {
        QTest::addColumn<QString>("instruction"); QTest::addColumn<QJsonObject>("parameters"); QTest::addColumn<bool>("allowed");
        QTest::newRow("search") << QString("帮我搜索陈奕迅的十年") << QJsonObject{{"artist","陈奕迅"},{"title","十年"},{"play",false}} << true;
        QTest::newRow("play") << QString("帮我播放陈奕迅的十年") << QJsonObject{{"artist","陈奕迅"},{"title","十年"},{"play",true}} << true;
        QTest::newRow("search-cannot-play") << QString("帮我搜索陈奕迅的十年") << QJsonObject{{"artist","陈奕迅"},{"title","十年"},{"play",true}} << false;
        QTest::newRow("injected-title") << QString("帮我搜索陈奕迅的十年") << QJsonObject{{"artist","陈奕迅"},{"title","其他歌曲"},{"play",false}} << false;
        QTest::newRow("injected-url") << QString("帮我搜索陈奕迅的十年") << QJsonObject{{"artist","陈奕迅"},{"title","十年"},{"play",false},{"url","https://example.test"}} << false;
        QTest::newRow("device-play") << QString("就在这台电脑播放陈奕迅的十年") << QJsonObject{{"artist","陈奕迅"},{"title","十年"},{"play",true}} << true;
        QTest::newRow("short-play") << QString("给我放陈奕迅的十年") << QJsonObject{{"artist","陈奕迅"},{"title","十年"},{"play",true}} << true;
        QTest::newRow("version-studio") << QString("播放陈奕迅的十年，用录音室版") << QJsonObject{{"artist","陈奕迅"},{"title","十年"},{"play",true},{"version","studio"}} << true;
        QTest::newRow("version-unrequested") << QString("播放陈奕迅的十年") << QJsonObject{{"artist","陈奕迅"},{"title","十年"},{"play",true},{"version","studio"}} << false;
        QTest::newRow("negative-play-tail") << QString("播放陈奕迅的十年，先不要播放") << QJsonObject{{"artist","陈奕迅"},{"title","十年"},{"play",true}} << false;
        QTest::newRow("condition-tail") << QString("播放陈奕迅的十年，如果我等会同意") << QJsonObject{{"artist","陈奕迅"},{"title","十年"},{"play",true}} << false;
        QTest::newRow("quote-prefix") << QString("他说播放陈奕迅的十年") << QJsonObject{{"artist","陈奕迅"},{"title","十年"},{"play",true}} << false;
        QTest::newRow("question-state") << QString("已经播放陈奕迅的十年了吗") << QJsonObject{{"artist","陈奕迅"},{"title","十年"},{"play",true}} << false;
    }
    void lookupContractAndPlaybackAuthority() {
        QFETCH(QString,instruction); QFETCH(QJsonObject,parameters); QFETCH(bool,allowed);
        Fixture f; QVERIFY(f.login()); QVERIFY(submit(f,instruction));
        f.overridePlan=Fixture::plan(f.messageId,{"song_lookup"},{Fixture::action("lookup","music.lookup",instruction,parameters)});
        QSignalSpy ready(f.client,&ConversationClient::routeReady),failed(f.client,&ConversationClient::routeFailed);
        f.client->route(f.messageId,{});
        QTRY_COMPARE(ready.count()+failed.count(),1); QCOMPARE(ready.count(),allowed?1:0);
    }
};
QTEST_GUILESS_MAIN(RoutingClientTest)
#include "routing_client_test.moc"
