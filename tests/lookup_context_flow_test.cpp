#include "routing_fixtures.h"
#include "mainwindow.h"
#include "radiodialog.h"
#include "radiostore.h"
#include "lookupcontext.h"
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTableView>
using namespace RoutingFixtures;

class LookupContextFlowTest : public QObject {
    Q_OBJECT
    QJsonObject song(int id, const QString &artist, const QString &version = {}) {
        return {{"id", id}, {"title", "晚风来信"}, {"artist", artist}, {"version", version},
            {"album", "候选测试专辑"}, {"durationMs", 180000}, {"coverUrl", "not-a-network-url"}};
    }
    void catalog(FakeServer &music, Fixture &fixture, const QJsonArray &songs, int delay = 0) {
        fixture.environment.set("MUSICPLAYER_NETEASE_PORT", QByteArray::number(music.server.serverPort()));
        QDir().mkpath(fixture.directory.filePath("netease"));
        QFile token(fixture.directory.filePath("netease/token"));
        if (token.open(QIODevice::WriteOnly)) token.write("owned-offline-fixture-token");
        token.close();
        // 夹具和正式曲库一样提供身份 ID；版本测试不能绕过歌手核验。
        QJsonArray creditedSongs, artists;
        QHash<QString, int> identities;
        for (const auto &value : songs) {
            auto row = value.toObject();
            const QString name = row.value("artist").toString();
            if (!identities.contains(name)) {
                const int id = 1001 + identities.size();
                identities.insert(name, id);
                artists.append(QJsonObject{{"id", id}, {"name", name}});
            }
            row.insert("artists", QJsonArray{QJsonObject{{"id", identities.value(name)}, {"name", name}}});
            creditedSongs.append(row);
        }
        music.handler = [creditedSongs, artists, delay](const Request &request) -> Response {
            if (request.path == "/health") return {{{"app", "quiet-netease"}, {"version", 1}, {"catalogVersion", 2}, {"profileIsolation", 1}}};
            if (request.path == "/assistant/artists") return {{{"artists", artists}, {"total", artists.size()}}};
            if (request.path == "/search") return {{{"songs", creditedSongs}, {"total", creditedSongs.size()}}, 200, delay};
            if (request.path == "/song/url") return {{{"message", "固定验收不提供音乐流"}}};
            return {{{"loggedIn", false}}};
        };
    }
    bool open(Fixture &fixture, MainWindow &window) {
        window.show();
        if (!fixture.login() || !QMetaObject::invokeMethod(&window, "openRadio", Qt::DirectConnection)) return false;
        auto *client = window.findChild<ConversationClient *>();
        return QTest::qWaitFor([&] { return !client->busy(); }, 1500);
    }
    bool send(Fixture &fixture, MainWindow &window, const QString &text) {
        auto *client = window.findChild<ConversationClient *>();
        if (!QTest::qWaitFor([&] { return !client->busy() && !client->snapshot().value("journalPending").toBool(); }, 2000)) return false;
        auto *dialog = window.findChild<RadioDialog *>();
        auto *button = dialog->findChild<QPushButton *>("button_RadioGenerate");
        dialog->findChild<QPlainTextEdit *>("edit_RadioInstruction")->setPlainText(text);
        if (!button->isEnabled()) return false;
        const auto oldId = fixture.messageId; button->click();
        return QTest::qWaitFor([&] { return fixture.messageId != oldId && !client->busy() && !client->snapshot().value("journalPending").toBool(); }, 2000);
    }
    void lookup(Fixture &fixture) {
        fixture.makePlan = [](const QString &id, const QString &text) {
            return Fixture::plan(id, {"song_lookup"}, {Fixture::action("lookup-source", "music.lookup", text,
                {{"artist", ""}, {"title", "晚风来信"}, {"play", false}})});
        };
    }
    QJsonObject context(MainWindow &window) { return window.property("tangoLookupContext").toJsonObject(); }
    QString transcript(MainWindow &window) { return window.findChild<QPlainTextEdit *>("text_RadioConversation")->toPlainText(); }
    void refine(Fixture &fixture, const QJsonObject &selection) {
        fixture.makePlan = [&fixture, selection](const QString &id, const QString &text) {
            QJsonObject context;
            for (const auto &request : fixture.server.requests)
                if (request.method == "POST" && request.path.endsWith("/" + id + "/route")) context = request.body.value("lookupContext").toObject();
            QJsonObject parameters = selection;
            for (const auto &key : QStringList{"candidateSetId", "originMessageId", "originActionId"}) parameters.insert(key, context.value(key));
            return Fixture::plan(id, {"song_lookup"}, {Fixture::action("refine-current", "music.refine", text, parameters)});
        };
    }
    void noWrites(Fixture &fixture, FakeServer &music, MainWindow &window) {
        QCOMPARE(music.count("GET", "/search"), 1); QCOMPARE(music.count("GET", "/song/url"), 0);
        QCOMPARE(fixture.server.count("POST", "/v1/tasks"), 0);
        QCOMPARE(window.findChild<QTableView *>("queueView")->model()->rowCount(), 0);
        QVERIFY(RadioStore(QSqlDatabase::database()).preference(window.accountProfile()).isEmpty());
    }
private slots:
    void cleanup() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (QSqlDatabase::contains()) { QSqlDatabase::database(QSqlDatabase::defaultConnection, false).close(); QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection); }
    }
    void lookupPublishesOnlyBoundedMetadata() {
        Fixture f(true); FakeServer music; catalog(music, f, {song(101,"甲","live"), song(102,"乙","studio")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("找一下《晚风来信》，别放，先让我看看")));
        QTRY_VERIFY(LookupContext::valid(context(window)));
        QCOMPARE(context(window).value("originMessageId").toString(),f.messageId);
        QCOMPARE(context(window).value("visibleIds").toArray(),QJsonArray({101,102}));
        for (const auto &value : context(window).value("rows").toArray()) { QVERIFY(!value.toObject().contains("coverUrl")); QVERIFY(!value.toObject().contains("url")); }
        noWrites(f,music,window);
    }
    void newConversationCannotPlayOldCandidate() {
        Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","studio")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        auto *dialog=window.findChild<RadioDialog*>(); QTRY_COMPARE(dialog->recommendations().size(),1);
        auto *client=window.findChild<ConversationClient*>();
        QTRY_VERIFY(!client->busy() && !client->snapshot().value("journalPending").toBool());
        client->startNew();
        QTRY_VERIFY(dialog->recommendations().isEmpty());
        QVERIFY(context(window).isEmpty()); QVERIFY(send(f,window,QStringLiteral("播放第1首")));
        QCOMPARE(music.count("GET","/song/url"),0);
    }
    void continuousIndexVersionAndOtherArtistUseOriginalPool() {
        Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","live"),song(102,"乙","studio"),song(103,"丙","studio")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_VERIFY(LookupContext::valid(context(window))); const auto original=context(window); const auto rootRows=original.value("rows");
        refine(f,{{"operation","select"},{"index",2}}); QVERIFY(send(f,window,QStringLiteral("我要第二首的那个")));
        QVERIFY2(QTest::qWaitFor([&]{return context(window).value("visibleIds").toArray()==QJsonArray({102});},1500),qPrintable(transcript(window)+QString::fromUtf8(QJsonDocument(context(window)).toJson(QJsonDocument::Compact))));
        QVERIFY(context(window).value("candidateSetId")!=original.value("candidateSetId")); QCOMPARE(context(window).value("rows"),rootRows);
        refine(f,{{"operation","other_artist"}}); QVERIFY(send(f,window,QStringLiteral("换成另一个歌手")));
        QTRY_COMPARE(context(window).value("visibleIds").toArray(),QJsonArray({101,103}));
        refine(f,{{"operation","version"},{"version","studio"}}); QVERIFY(send(f,window,QStringLiteral("我要录音室版的那个")));
        QTRY_COMPARE(context(window).value("visibleIds").toArray(),QJsonArray({102,103}));
        QCOMPARE(context(window).value("rows"),rootRows); QVERIFY(transcript(window).contains(QStringLiteral("按本轮条件重新筛选")));
        noWrites(f,music,window);
    }
    void indexUsesVisibleOrderAndCorrectionCanReturnExcludedVersion() {
        Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","live"),song(102,"乙","studio"),song(103,"丙","studio")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_VERIFY(LookupContext::valid(context(window)));
        refine(f,{{"operation","exclude_version"},{"version","live"}}); QVERIFY(send(f,window,QStringLiteral("不要现场版，先别放")));
        QTRY_COMPARE(context(window).value("visibleIds").toArray(),QJsonArray({102,103}));
        refine(f,{{"operation","select"},{"index",2}}); QVERIFY(send(f,window,QStringLiteral("第二首")));
        QTRY_COMPARE(context(window).value("visibleIds").toArray(),QJsonArray({103}));
        refine(f,{{"operation","version"},{"version","live"}}); QVERIFY(send(f,window,QStringLiteral("换成现场版")));
        QTRY_COMPARE(context(window).value("visibleIds").toArray(),QJsonArray({101})); noWrites(f,music,window);
    }
    void unknownVersionNeverBecomesStudioAndEmptySelectionCanBeCorrected() {
        Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲"),song(102,"乙","live")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_VERIFY(LookupContext::valid(context(window)));
        refine(f,{{"operation","exclude_version"},{"version","live"}}); QVERIFY(send(f,window,QStringLiteral("排除现场版")));
        QTRY_VERIFY(context(window).value("visibleIds").toArray().isEmpty()); QCOMPARE(context(window).value("rows").toArray().first().toObject().value("version").toString(),QString("unknown"));
        QVERIFY(transcript(window).contains(QStringLiteral("版本未知不会当作录音室版")));
        refine(f,{{"operation","version"},{"version","live"}}); QVERIFY(send(f,window,QStringLiteral("要现场版那个")));
        QTRY_COMPARE(context(window).value("visibleIds").toArray(),QJsonArray({102})); noWrites(f,music,window);
    }
    void clarificationPreservesPoolWithoutPartiallyApplyingGender() {
        Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","live"),song(102,"乙","studio")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_VERIFY(LookupContext::valid(context(window))); const auto before=context(window);
        f.makePlan=[](const QString &id,const QString &text){return Fixture::plan(id,{"song_lookup"},{Fixture::action("reply","assistant.reply",text,
            {{"kind","clarify"},{"text","这批候选没有可靠的声线资料，不能判断女声；请说歌手或序号。"}})});};
        QVERIFY(send(f,window,QStringLiteral("女声的那个，不要现场版"))); QCOMPARE(context(window),before);
        QCOMPARE(window.findChild<RadioDialog*>()->recommendations().size(),2);
        refine(f,{{"operation","artist"},{"artist","乙"}}); QVERIFY(send(f,window,QStringLiteral("我要乙唱的那个")));
        QTRY_COMPARE(context(window).value("visibleIds").toArray(),QJsonArray({102})); noWrites(f,music,window);
    }
    void clarificationCanBeAnsweredWithTheCompleteCandidateArtistName() {
        Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","live"),song(102,"许栀","studio")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_VERIFY(LookupContext::valid(context(window))); const auto before=context(window);
        f.makePlan=[](const QString &id,const QString &text){return Fixture::plan(id,{"song_lookup"},{Fixture::action("clarify-artist","assistant.reply",text,
            {{"kind","clarify"},{"text","这批候选没有可靠的声线资料，请说完整歌手名或序号。"}})});};
        QVERIFY(send(f,window,QStringLiteral("女声那个"))); QCOMPARE(context(window),before);
        refine(f,{{"operation","artist"},{"artist","许栀"}}); QVERIFY(send(f,window,QStringLiteral("许栀")));
        QVERIFY2(QTest::qWaitFor([&]{return context(window).value("visibleIds").toArray()==QJsonArray({102});},1500),qPrintable(transcript(window)));
        noWrites(f,music,window);
    }
    void unrelatedTopicClearsContextAndVisibleSongs() {
        Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","studio"),song(102,"乙","live")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_VERIFY(LookupContext::valid(context(window)));
        f.makePlan=[](const QString &id,const QString &text){return Fixture::plan(id,{"help"},{Fixture::action("reply","assistant.reply",text,{{"kind","help"},{"text","功能说明"}})});};
        QVERIFY(send(f,window,QStringLiteral("你能做什么"))); QVERIFY(context(window).isEmpty()); QVERIFY(window.findChild<RadioDialog*>()->recommendations().isEmpty());
        noWrites(f,music,window);
    }
    void forgedSelectorsCannotChangeTheVisibleCandidates_data() {
        QTest::addColumn<QJsonObject>("extra");
        QTest::newRow("play-field")<<QJsonObject{{"play",false}};
        QTest::newRow("wrong-index")<<QJsonObject{{"index",1}};
        QTest::newRow("wrong-set")<<QJsonObject{{"candidateSetId","a-different-set"}};
        QTest::newRow("wrong-origin")<<QJsonObject{{"originMessageId","some-other-turn"}};
        QTest::newRow("url-field")<<QJsonObject{{"url","not-an-executable-source"}};
    }
    void forgedSelectorsCannotChangeTheVisibleCandidates() {
        QFETCH(QJsonObject,extra); Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","studio"),song(102,"乙","live")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_VERIFY(LookupContext::valid(context(window))); const auto before=context(window);
        f.makePlan=[before,extra](const QString &id,const QString &text){
            QJsonObject parameters{{"candidateSetId",before.value("candidateSetId")},{"originMessageId",before.value("originMessageId")},
                {"originActionId",before.value("originActionId")},{"operation","select"},{"index",2}};
            for(auto it=extra.begin();it!=extra.end();++it)parameters.insert(it.key(),it.value());
            return Fixture::plan(id,{"song_lookup"},{Fixture::action("refine","music.refine",text,parameters)});
        };
        QVERIFY(send(f,window,QStringLiteral("第二首"))); QVERIFY(context(window).isEmpty());
        QVERIFY(window.findChild<RadioDialog*>()->recommendations().isEmpty()); QVERIFY(transcript(window).contains(QStringLiteral("未通过类型或授权范围校验")));
        noWrites(f,music,window);
    }
    void changedAccountHasNoUsableCandidateContext() {
        Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","studio")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_VERIFY(LookupContext::valid(context(window))); QVERIFY(f.login("other-account"));
        QVERIFY(context(window).isEmpty()); QVERIFY(window.findChild<RadioDialog*>()->recommendations().isEmpty());
        QCOMPARE(music.count("GET","/song/url"),0);
    }
    void oversizedIdentityDoesNotEnableContext() {
        Fixture f(true); FakeServer music; auto row=song(101,QString(201,QLatin1Char('a')),"studio"); catalog(music,f,{row});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_COMPARE(window.findChild<RadioDialog*>()->recommendations().size(),1); QVERIFY(context(window).isEmpty());
        QTRY_VERIFY(transcript(window).contains(QStringLiteral("未启用跨轮指代"))); noWrites(f,music,window);
    }
    void expiredLookupCannotUseDirectPlaybackAndRefinementDoesNotExtendDeadline() {
        Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","studio"),song(102,"乙","live")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_VERIFY(LookupContext::valid(context(window))); const auto deadline=window.radioLookupContextDeadline.deadline();
        refine(f,{{"operation","select"},{"index",2}}); QVERIFY(send(f,window,QStringLiteral("第二首")));
        QTRY_COMPARE(context(window).value("visibleIds").toArray(),QJsonArray({102})); QCOMPARE(window.radioLookupContextDeadline.deadline(),deadline);
        // 仅测试友元把同一私有截止时间设过期，真实输入仍走完整本机播放守卫。
        window.radioLookupContextDeadline.setRemainingTime(0,Qt::PreciseTimer);
        QVERIFY(send(f,window,QStringLiteral("播放第1首"))); QVERIFY(context(window).isEmpty());
        QVERIFY(window.findChild<RadioDialog*>()->recommendations().isEmpty()); noWrites(f,music,window);
    }
    void localPreferenceTopicInvalidatesDirectLookupPlayback() {
        Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","studio")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_VERIFY(LookupContext::valid(context(window))); QVERIFY(send(f,window,QStringLiteral("查看我的口味")));
        QVERIFY(context(window).isEmpty()); QVERIFY(window.findChild<RadioDialog*>()->recommendations().isEmpty());
        QVERIFY(send(f,window,QStringLiteral("播放第1首"))); noWrites(f,music,window);
    }
    void delayedLookupResponseCannotRebuildCandidatesAfterTopicChange() {
        Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","studio")},300);
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_COMPARE(music.count("GET","/search"),1);
        f.makePlan=[](const QString &id,const QString &text){return Fixture::plan(id,{"help"},{Fixture::action("reply","assistant.reply",text,{{"kind","help"},{"text","新的话题"}})});};
        QVERIFY(send(f,window,QStringLiteral("你能做什么"))); QTest::qWait(400);
        QVERIFY(context(window).isEmpty()); QVERIFY(window.findChild<RadioDialog*>()->recommendations().isEmpty()); noWrites(f,music,window);
    }
    void lateOlderLookupAndRouteCannotReplaceNewCandidateSet() {
        Fixture f(true); FakeServer music; catalog(music,f,{}); int searches=0; const auto fallback=music.handler;
        music.handler=[&](const Request &request){
            if(request.path=="/search")return Response{{{"songs",QJsonArray{song(++searches==1?101:202,"甲","studio")}}},200,searches==1?300:0};
            return fallback(request);
        };
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_COMPARE(music.count("GET","/search"),1); const auto older=f.messageId; const auto olderPlan=f.routePlans.value(older);
        QVERIFY(send(f,window,QStringLiteral("再搜索晚风来信"))); QTRY_COMPARE(music.count("GET","/search"),2);
        QTRY_COMPARE(context(window).value("visibleIds").toArray(),QJsonArray({202})); const auto newer=context(window);
        // 模拟尚未投递的旧路由回调，不能在动作校验前清掉较新的上下文。
        window.routedRadioTurns.remove(older); window.applyRadioRoute(older,olderPlan); QTest::qWait(400);
        QCOMPARE(context(window),newer); QCOMPARE(window.findChild<RadioDialog*>()->recommendations().first().song.id,qint64(202));
        QCOMPARE(music.count("GET","/song/url"),0); QCOMPARE(f.server.count("POST","/v1/tasks"),0);
    }
    void conflictingVersionCannotBeReinferredOrAutomaticallyPlayed_data() {
        QTest::addColumn<QString>("explicitVersion"); QTest::addColumn<QString>("title"); QTest::addColumn<QString>("requestedVersion");
        QTest::newRow("explicit-studio-title-live")<<QString("studio")<<QString("晚风来信 (Live)")<<QString("live");
        QTest::newRow("explicit-live-title-studio")<<QString("live")<<QString("晚风来信 (Studio)")<<QString("studio");
    }
    void conflictingVersionCannotBeReinferredOrAutomaticallyPlayed() {
        QFETCH(QString,explicitVersion); QFETCH(QString,title); QFETCH(QString,requestedVersion);
        Fixture f(true); FakeServer music; auto row=song(101,"甲",explicitVersion); row.insert("title",title); catalog(music,f,{row});
        MainWindow window; QVERIFY(open(f,window));
        f.makePlan=[requestedVersion](const QString &id,const QString &text){return Fixture::plan(id,{"song_lookup"},{Fixture::action("lookup-source","music.lookup",text,
            {{"artist","甲"},{"title","晚风来信"},{"play",true},{"version",requestedVersion}})});};
        QVERIFY(send(f,window,requestedVersion=="live"?QStringLiteral("播放甲的晚风来信，用现场版"):QStringLiteral("播放甲的晚风来信，用录音室版")));
        QTRY_VERIFY(LookupContext::valid(context(window))); QCOMPARE(context(window).value("rows").toArray().first().toObject().value("version").toString(),QString("unknown"));
        noWrites(f,music,window);
    }
    void knownBackendContextRejectionClearsPreviewWithoutReplay() {
        Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","studio")});
        MainWindow window; QVERIFY(open(f,window)); lookup(f); QVERIFY(send(f,window,QStringLiteral("搜索晚风来信")));
        QTRY_VERIFY(LookupContext::valid(context(window))); const auto fallback=f.server.handler;
        f.server.handler=[fallback](const Request &request){
            if(request.method=="POST"&&request.path.endsWith("/route"))return Response{{{"error",QJsonObject{{"code","LOOKUP_CONTEXT_STALE"},{"message","candidate expired"}}}},409};
            return fallback(request);
        };
        QVERIFY(send(f,window,QStringLiteral("第二首"))); QVERIFY(context(window).isEmpty()); QVERIFY(window.findChild<RadioDialog*>()->recommendations().isEmpty());
        QVERIFY(transcript(window).contains(QStringLiteral("后台已拒绝本次候选筛选"))); QCOMPARE(f.server.count("GET",f.routePath()),0);
        QVERIFY(send(f,window,QStringLiteral("播放第1首"))); noWrites(f,music,window);
    }
    void recoveredRefinementAndLegacyLookupNeverReexecute_data() {
        QTest::addColumn<bool>("refined"); QTest::newRow("refinement-dispatched")<<true; QTest::newRow("legacy-lookup-unknown")<<false;
    }
    void recoveredRefinementAndLegacyLookupNeverReexecute() {
        QFETCH(bool,refined); Fixture f(true); FakeServer music; catalog(music,f,{song(101,"甲","studio"),song(102,"乙","live")});
        auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); lookup(f); QVERIFY(send(f,*window,QStringLiteral("搜索晚风来信")));
        QTRY_VERIFY(LookupContext::valid(context(*window)));
        if(refined){refine(f,{{"operation","select"},{"index",2}});QVERIFY(send(f,*window,QStringLiteral("第二首")));QTRY_COMPARE(context(*window).value("visibleIds").toArray(),QJsonArray({102}));}
        auto *client=window->findChild<ConversationClient*>(); QTRY_VERIFY(!client->busy()&&!client->snapshot().value("journalPending").toBool());
        const auto turn=f.messageId,profile=window->accountProfile(),device=client->deviceId(); const auto action=refined?QString("refine-current"):QString("lookup-source");
        QSqlQuery query;query.prepare("UPDATE radio_route_journal SET actions_json=?,terminal=0 WHERE profile=? AND device_id=? AND turn_id=?");
        query.addBindValue(QString::fromUtf8(QJsonDocument(QJsonObject{{action,refined?"dispatched":"unknown"}}).toJson(QJsonDocument::Compact)));query.addBindValue(profile);query.addBindValue(device);query.addBindValue(turn);QVERIFY(query.exec());query.finish();
        window.reset(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        if(QSqlDatabase::contains()){QSqlDatabase::database(QSqlDatabase::defaultConnection,false).close();QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);}
        window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); QTRY_VERIFY(f.server.count("GET",f.messagesPath()+"/"+turn+"/route")>0);
        QTest::qWait(100); QVERIFY(context(*window).isEmpty()); QVERIFY(window->findChild<RadioDialog*>()->recommendations().isEmpty());
        QCOMPARE(f.server.count("POST",f.messagesPath()+"/"+turn+"/route"),1); QCOMPARE(music.count("GET","/search"),1); QCOMPARE(music.count("GET","/song/url"),0);
        if(refined)QTRY_VERIFY(transcript(*window).contains(QStringLiteral("原候选未跨重启保存")));
    }
    void contextContractRejectsUnknownKeysAndLargePayload() {
        QJsonObject context{{"candidateSetId","set-123456"},{"originMessageId","message-123456"},{"originActionId","lookup-source"},
            {"rows",QJsonArray{QJsonObject{{"id",1},{"title","title"},{"artist","artist"},{"album","album"},{"version","unknown"},{"durationMs",180000}}}},
            {"visibleIds",QJsonArray{1}}};
        QVERIFY(LookupContext::valid(context)); auto injected=context; injected.insert("play",false); QVERIFY(!LookupContext::valid(injected));
        auto duplicate=context; duplicate.insert("visibleIds",QJsonArray{1,1}); QVERIFY(!LookupContext::valid(duplicate));
        auto foreign=context; foreign.insert("visibleIds",QJsonArray{2}); QVERIFY(!LookupContext::valid(foreign));
        QJsonArray rows,ids; for(int i=1;i<=12;++i){rows.append(QJsonObject{{"id",i},{"title",QString(200,QChar(0x4e00))},{"artist",QString(200,QChar(0x4e01))},
            {"album",QString(200,QChar(0x4e02))},{"version","unknown"},{"durationMs",180000}});ids.append(i);}
        context.insert("rows",rows);context.insert("visibleIds",ids);QVERIFY(!LookupContext::valid(context));
    }
};
QTEST_MAIN(LookupContextFlowTest)
#include "lookup_context_flow_test.moc"
