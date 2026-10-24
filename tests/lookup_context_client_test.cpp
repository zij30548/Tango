#include "routing_fixtures.h"
#include "lookupcontext.h"
using namespace RoutingFixtures;

class LookupContextClientTest : public QObject {
    Q_OBJECT
    QJsonObject context() {
        return {{"candidateSetId","candidate-123456"},{"originMessageId","message-123456"},{"originActionId","lookup-source"},
            {"rows",QJsonArray{QJsonObject{{"id",1},{"title","晚风来信"},{"artist","乙"},{"album","候选专辑"},{"version","studio"},{"durationMs",180000}}}},
            {"visibleIds",QJsonArray{1}}};
    }
    bool submit(Fixture &fixture) {
        QSignalSpy accepted(fixture.client,&ConversationClient::messageAccepted);fixture.client->submit(QStringLiteral("第一首"));
        return QTest::qWaitFor([&]{return accepted.count()==1;},1500);
    }
private slots:
    void originalPostOnlyRecognizesCompletePrecommitContextErrors_data() {
        QTest::addColumn<int>("status");QTest::addColumn<QString>("code");QTest::addColumn<bool>("terminal");QTest::addColumn<bool>("getKnownError");
        QTest::newRow("invalid")<<400<<QString("INVALID_LOOKUP_CONTEXT")<<true<<false;
        QTest::newRow("stale")<<409<<QString("LOOKUP_CONTEXT_STALE")<<true<<false;
        QTest::newRow("idempotency-conflict")<<409<<QString("IDEMPOTENCY_CONFLICT")<<false<<false;
        QTest::newRow("missing-code")<<409<<QString()<<false<<false;
        QTest::newRow("server-error")<<500<<QString("LOOKUP_CONTEXT_STALE")<<false<<false;
        QTest::newRow("wrong-status")<<400<<QString("LOOKUP_CONTEXT_STALE")<<false<<false;
        QTest::newRow("get-cannot-prove-original-post")<<503<<QString("TEMPORARY_ERROR")<<false<<true;
    }
    void originalPostOnlyRecognizesCompletePrecommitContextErrors() {
        QFETCH(int,status);QFETCH(QString,code);QFETCH(bool,terminal);QFETCH(bool,getKnownError);
        Fixture f;QVERIFY(f.login());QVERIFY(submit(f));const auto fallback=f.server.handler;
        f.server.handler=[&](const Request &request){
            if(request.path.endsWith("/route")){
                const bool post=request.method=="POST";
                QJsonObject error{{"message","owned protocol failure"}};
                const auto selected=post?code:getKnownError?QString("LOOKUP_CONTEXT_STALE"):QString("NOT_FOUND");
                if(!selected.isEmpty())error.insert("code",selected);
                return Response{{{"error",error}},post?status:getKnownError?409:404};
            }
            return fallback(request);
        };
        QSignalSpy failed(f.client,&ConversationClient::routeFailed),ready(f.client,&ConversationClient::routeReady);
        f.client->route(f.messageId,{},context()); QTRY_COMPARE_WITH_TIMEOUT(failed.count(),1,4000);
        QCOMPARE(ready.count(),0);QCOMPARE(f.client->routeNeedsObservation(f.messageId),!terminal);
        QCOMPARE(f.server.count("POST",f.routePath()),1); QCOMPARE(f.server.count("GET",f.routePath()),terminal?0:2);
        QCOMPARE(failed.first().at(1).toString().contains(QStringLiteral("后台已拒绝本次候选筛选")),terminal);
        if(terminal){f.client->route(f.messageId,{},context());f.client->observeRoute(f.messageId);QTest::qWait(50);QCOMPARE(f.server.count("POST",f.routePath()),1);QCOMPARE(f.server.count("GET",f.routePath()),0);}
    }
    void ordinaryConversationDoesNotTreatContextCodeAsKnownFailure() {
        Fixture f;QVERIFY(f.login());QVERIFY(submit(f));const auto fallback=f.server.handler;
        f.server.handler=[&](const Request &request){
            if(request.path.endsWith("/route"))return Response{{{"error",QJsonObject{{"code","LOOKUP_CONTEXT_STALE"},{"message","unexpected context code"}}}},409};
            return fallback(request);
        };
        QSignalSpy failed(f.client,&ConversationClient::routeFailed);f.client->route(f.messageId,{});
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(),1,4000);QVERIFY(f.client->routeNeedsObservation(f.messageId));QCOMPARE(f.server.count("POST",f.routePath()),1);
    }
    void invalidLocalContextNeverLeavesDevice() {
        Fixture f;QVERIFY(f.login());QVERIFY(submit(f));auto value=context();value.insert("url","not-uploaded");
        QSignalSpy failed(f.client,&ConversationClient::routeFailed);f.client->route(f.messageId,{},value);
        QCOMPARE(failed.count(),1);QCOMPARE(f.server.count("POST",f.routePath()),0);
    }
    void parserMatchesBackendReadOnlyVocabulary_data() {
        QTest::addColumn<QString>("instruction");QTest::addColumn<QJsonObject>("expected");
        for(const auto &text:QStringList{"第二首","我要第２首的那个","请你帮我选择第二首","看看第二首这个"})QTest::newRow(text.toUtf8())<<text<<QJsonObject{{"operation","select"},{"index",2}};
        for(const auto &text:QStringList{"我要录音室版的那个","只要录音室版","还是录音室版","看看录音室版那首"})QTest::newRow(text.toUtf8())<<text<<QJsonObject{{"operation","version"},{"version","studio"}};
        QTest::newRow("readonly-tail")<<QString("第二首，先别放，先让我看看")<<QJsonObject{{"operation","select"},{"index",2}};
        QTest::newRow("artist")<<QString("我说的是乙那个")<<QJsonObject{{"operation","artist"},{"artist","乙"}};
        QTest::newRow("complete-artist-only")<<QString("乙")<<QJsonObject{{"operation","artist"},{"artist","乙"}};
        QTest::newRow("complete-artist-whitespace")<<QString(" 乙 ")<<QJsonObject{{"operation","artist"},{"artist","乙"}};
        QTest::newRow("other-correction")<<QString("不是这个歌手，换另一个歌手")<<QJsonObject{{"operation","other_artist"}};
        for(const auto &text:QStringList{"女声的那个，不要现场版","如果我同意，第二首","他说选择第二首","第二首，然后播放","播放第二首","第二首，先别放，先让我看看，给我资料","乙，先别放","乙。","如果是乙","他说乙","“乙”"})
            QTest::newRow(text.toUtf8())<<text<<QJsonObject{};
    }
    void parserMatchesBackendReadOnlyVocabulary() {
        QFETCH(QString,instruction);QFETCH(QJsonObject,expected);QCOMPARE(LookupContext::requestedSelection(instruction,context()),expected);
    }
    void completeArtistWithoutCandidateContextCannotSelect() {
        QVERIFY(LookupContext::requestedSelection(QStringLiteral("乙")).isEmpty());
    }
};
QTEST_GUILESS_MAIN(LookupContextClientTest)
#include "lookup_context_client_test.moc"
