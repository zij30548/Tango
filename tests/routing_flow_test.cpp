#include "routing_fixtures.h"
#include "mainwindow.h"
#include "radiodialog.h"
#include "radiostore.h"
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableView>
#include <QSqlDatabase>
#include <QSqlQuery>
using namespace RoutingFixtures;
class RoutingFlowTest : public QObject {
    Q_OBJECT
    bool open(Fixture &fixture, MainWindow &window, const QString &username = QStringLiteral("fixture")) {
        window.show();
        if (!fixture.login(username) || !QMetaObject::invokeMethod(&window,"openRadio",Qt::DirectConnection)) return false;
        auto *conversation=window.findChild<ConversationClient*>();
        return conversation && QTest::qWaitFor([&]{return !conversation->busy();},1500);
    }
    bool send(MainWindow &window, Fixture &fixture, const QString &text) {
        auto *dialog=window.findChild<RadioDialog*>(); if(!dialog)return false;
        auto *conversation=window.findChild<ConversationClient*>();
        if(!settled(window))return false;
        const int before=fixture.routeReplies.size();
        dialog->findChild<QPlainTextEdit*>("edit_RadioInstruction")->setPlainText(text);
        auto *button=dialog->findChild<QPushButton*>("button_RadioGenerate");
        if(!QTest::qWaitFor([&]{return button->isEnabled();},2000))return false;
        button->click();
        return QTest::qWaitFor([&]{return fixture.routeReplies.size()>before&&!conversation->busy();},2000);
    }
    void music(Fixture &fixture,const QString &mode="new") {
        fixture.makePlan=[mode,&fixture](const QString &id,const QString &text){
            QJsonObject parameters{{"mode",mode}};
            if(mode=="continue"){parameters.insert("taskId",fixture.task.value("id"));parameters.insert("expectedRevision",fixture.task.value("revision"));}
            return Fixture::plan(id,{"scene_research"},{Fixture::action("music","music.task",text,parameters)});
        };
    }
    void compound(Fixture &fixture,bool depends=false) {
        fixture.makePlan=[depends](const QString &id,const QString &text){return Fixture::plan(id,{"preference_proposal","scene_research"},{
            Fixture::action("memory","memory.propose",text,{{"operation","set"},{"preference","喜欢爵士；少人声"}}),
            Fixture::action("music","music.task",text,{{"mode","new"}},depends?QJsonArray{"memory"}:QJsonArray{})});};
    }
    void destroy(std::unique_ptr<MainWindow> &window) {
        window.reset();
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        // 模拟新窗口重新打开原账号文件，避免同一条打开连接掩盖漏写磁盘的问题。
        if (QSqlDatabase::contains()) {
            QSqlDatabase::database(QSqlDatabase::defaultConnection,false).close();
            QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
        }
    }
    bool settled(MainWindow &window) {
        auto *conversation=window.findChild<ConversationClient*>();
        auto *task=window.findChild<MusicTaskClient*>();
        return conversation && task && QTest::qWaitFor([&] {
            return !conversation->busy() && !conversation->snapshot().value("journalPending").toBool() && !task->busy();
        },2000);
    }
    int preferenceRevision(const QString &profile) {
        QSqlQuery query;
        query.prepare("SELECT revision FROM radio_preference_versions WHERE profile=?"); query.addBindValue(profile);
        if (!query.exec()) return -1;
        return query.next() ? query.value(0).toInt() : 0;
    }
    bool sendLocal(MainWindow &window, const QString &text) {
        if (!settled(window)) return false;
        auto *dialog=window.findChild<RadioDialog*>();
        auto *conversation=window.findChild<ConversationClient*>();
        const int before=conversation->snapshot().value("version").toInt();
        dialog->findChild<QPlainTextEdit*>("edit_RadioInstruction")->setPlainText(text);
        auto *button=dialog->findChild<QPushButton*>("button_RadioGenerate");
        if (!button->isEnabled()) return false;
        button->click();
        return QTest::qWaitFor([&] {
            return conversation->snapshot().value("version").toInt()>before &&
                !conversation->busy() && !conversation->snapshot().value("journalPending").toBool();
        },2000);
    }
private slots:
    void cleanup() {
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        if(QSqlDatabase::contains()){QSqlDatabase::database(QSqlDatabase::defaultConnection,false).close();QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);}
        AppBackendClient::instance();
    }
    void helpKeepsPreviewAndReplyAppearsOnce() {
        Fixture f(true);MainWindow window;QVERIFY(open(f,window));music(f);QVERIFY(send(window,f,"find quiet songs"));
        auto *dialog=window.findChild<RadioDialog*>();auto *table=dialog->findChild<QTableWidget*>("table_RadioRecommendations");
        QTRY_COMPARE(table->rowCount(),1);const auto before=dialog->recommendations().first().song.id;
        f.makePlan=[](const QString &id,const QString &text){return Fixture::plan(id,{"help"},{Fixture::action("reply","assistant.reply",text,{{"kind","help"},{"text","帮助说明只显示一次"}})});};
        QVERIFY(send(window,f,"how do I use this app"));QCOMPARE(table->rowCount(),1);QCOMPARE(dialog->recommendations().first().song.id,before);
        auto *transcript=dialog->findChild<QPlainTextEdit*>("text_RadioConversation");QTRY_COMPARE(transcript->toPlainText().count("帮助说明只显示一次"),1);
        QCOMPARE(f.server.count("POST","/v1/tasks"),1);QCOMPARE(f.server.count("POST","/v1/assistant/plan"),0);
    }
    void knownRouteRejectionDoesNotOfferUnknownRecovery() {
        Fixture f(true); MainWindow window; QVERIFY(open(f,window)); f.routeStatus="failed";
        auto *dialog=window.findChild<RadioDialog*>();
        dialog->findChild<QPlainTextEdit*>("edit_RadioInstruction")->setPlainText(QStringLiteral("帮我搜索某首歌"));
        auto *button=dialog->findChild<QPushButton*>("button_RadioGenerate"); QVERIFY(button->isEnabled()); button->click();
        auto *text=dialog->findChild<QPlainTextEdit*>("text_RadioConversation");
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("已拒绝")));
        QVERIFY(!text->toPlainText().contains(QStringLiteral("原请求结果尚未确认")));
        QCOMPARE(f.server.count("POST","/v1/tasks"),0);
    }
    void missingSlotQuestionAppearsInChatAndDoesNotPlay() {
        Fixture f(true); const auto original=f.server.handler;
        f.server.handler=[&f,original](const Request &request) {
            auto response=original(request);
            if (request.path=="/v1/tasks" && request.method=="POST") {
                f.task.insert("status","needs_clarification"); f.task.insert("question",QStringLiteral("你想查询哪位歌手的新歌？"));
                f.task.insert("preview",QJsonObject()); response.body.insert("task",f.task);
            }
            return response;
        };
        MainWindow window; QVERIFY(open(f,window)); music(f); QVERIFY(send(window,f,QStringLiteral("帮我查最近的新歌")));
        auto *text=window.findChild<QPlainTextEdit*>("text_RadioConversation");
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("你想查询哪位歌手的新歌？")));
        auto *queue=window.findChild<QTableView*>("queueView"); QVERIFY(queue); QCOMPARE(queue->model()->rowCount(),0);
    }
    void naturalArtistIdentitySelectsOnlyCurrentUniqueCandidate() {
        Fixture f(true); const auto original=f.server.handler;
        f.server.handler=[&f,original](const Request &request) {
            auto response=original(request);
            if (request.path=="/v1/tasks" && request.method=="POST") {
                f.task.insert("status","needs_clarification"); f.task.insert("question",QStringLiteral("请确认歌手身份"));
                f.task.insert("providedArtists",QJsonArray{QJsonObject{{"id",301},{"name","周棠"},{"description","独立音乐人，作品《夜港》"}},
                    QJsonObject{{"id",302},{"name","周棠"},{"description","器乐演奏者"}}});
                f.task.insert("preview",QJsonObject()); response.body.insert("task",f.task);
            }
            return response;
        };
        MainWindow window; QVERIFY(open(f,window)); music(f); QVERIFY(send(window,f,QStringLiteral("查周棠的新歌")));
        auto *text=window.findChild<QPlainTextEdit*>("text_RadioConversation");
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("夜港")));
        QVERIFY(sendLocal(window,QStringLiteral("就是做《夜港》的那位，不是器乐那位")));
        QTRY_COMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),1);
        Request selected; for (const auto &request:f.server.requests) if(request.path.endsWith("/actions")) selected=request;
        QCOMPARE(selected.body.value("action").toString(),QString("select_artist"));
        QCOMPARE(selected.body.value("artistId").toInt(),301);
        QCOMPARE(f.server.count("POST",f.routePath()),0);
        QCOMPARE(window.findChild<QTableView*>("queueView")->model()->rowCount(),0);
    }
    void naturalArtistSelectionRejectsNegatedQuotedConditionalOrAmbiguousClues_data() {
        QTest::addColumn<QString>("instruction"); QTest::addColumn<bool>("allowed");
        QTest::newRow("work-with-negative-contrast")<<QStringLiteral("就是做《夜港》的那位，不是器乐那位")<<true;
        QTest::newRow("positive-identity")<<QStringLiteral("选择器乐那位")<<true;
        QTest::newRow("negative-clue")<<QStringLiteral("不是做《夜港》的那位")<<false;
        QTest::newRow("quoted-choice")<<QStringLiteral("他说就是做《夜港》的那位")<<false;
        QTest::newRow("conditional-choice")<<QStringLiteral("如果可以就是做《夜港》的那位")<<false;
        QTest::newRow("ambiguous-identity")<<QStringLiteral("就是音乐人那位")<<false;
        QTest::newRow("two-works")<<QStringLiteral("就是《夜港》或者《日落》那位")<<false;
    }
    void naturalArtistSelectionRejectsNegatedQuotedConditionalOrAmbiguousClues() {
        QFETCH(QString,instruction); QFETCH(bool,allowed);
        qRegisterMetaType<AssistantArtist>(); RadioDialog dialog; dialog.setAvailability(true,true,{}); dialog.setConversationMode(true);
        AssistantArtist first; first.id=301; first.name=QStringLiteral("周棠"); first.aliases=QStringList{QStringLiteral("独立音乐人，作品《夜港》")};
        AssistantArtist second; second.id=302; second.name=QStringLiteral("周棠"); second.aliases=QStringList{QStringLiteral("器乐音乐人")};
        dialog.setArtistChoices({first,second}); QSignalSpy selection(&dialog,&RadioDialog::artistSelected);
        QCOMPARE(dialog.executeChatAction(instruction),allowed); QCOMPARE(selection.count(),allowed?1:0);
    }
    void naturalControlChangesCurrentTaskWithoutPlayingOrCreatingAnotherTask() {
        Fixture f(true); MainWindow window; QVERIFY(open(f,window)); music(f); QVERIFY(send(window,f,QStringLiteral("推荐通勤歌曲")));
        for (const auto &pair:QVector<QPair<QString,QString>>{{"先暂停一下任务","pause"},{"继续刚才的任务","resume"},{"算了取消吧","cancel"}}) {
            f.makePlan=[&f,pair](const QString &id,const QString &text) {return Fixture::plan(id,{"task_control"},
                {Fixture::action("control","task.control",text,{{"action",pair.second},{"taskId",f.task.value("id")},{"expectedRevision",f.task.value("revision")}})});};
            QVERIFY(send(window,f,pair.first));
            const auto expected=pair.second=="pause"?QString("paused"):pair.second=="cancel"?QString("cancelled"):QString("preview");
            QTRY_COMPARE(window.findChild<MusicTaskClient*>()->snapshot().value("status").toString(),expected);
        }
        QCOMPARE(f.server.count("POST","/v1/tasks"),1);
        QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),3);
        QCOMPARE(window.findChild<QTableView*>("queueView")->model()->rowCount(),0);
    }
    void lostAdoptionReceiptAfterReopenNeverRepostsOrAutoplays_data() {
        QTest::addColumn<bool>("committed");
        QTest::newRow("adopt-committed-response-lost")<<true;
        QTest::newRow("adopt-not-recorded-response-lost")<<false;
    }
    void lostAdoptionReceiptAfterReopenNeverRepostsOrAutoplays() {
        QFETCH(bool,committed); Fixture f(true); auto window=std::make_unique<MainWindow>();
        QVERIFY(open(f,*window)); music(f); QVERIFY(send(*window,f,QStringLiteral("推荐通勤歌曲"))); QVERIFY(settled(*window));
        const auto original=f.server.handler; QString originalId;
        f.server.handler=[&f,original,committed,&originalId](const Request &request) {
            if (request.path.endsWith("/actions") && request.body.value("action").toString()=="adopt") {
                originalId=request.body.value("requestId").toString();
                if (committed) {
                    f.task.insert("stateVersion",f.task.value("stateVersion").toInt()+1); f.task.insert("status","awaiting_apply");
                    f.task.insert("adoption",QJsonObject{{"id","adoption-original"},{"previewVersion",1}});
                    f.taskRequestReceipts.insert(originalId,{{"requestId",originalId},{"deviceId",f.task.value("deviceId")},{"taskId",f.task.value("id")},
                        {"status","recorded"},{"action","adopt"},{"expectedRevision",request.body.value("expectedRevision")}});
                    f.taskRequestSnapshots.insert(originalId,f.task); f.taskRequestOwners.insert(originalId,f.accountName);
                }
                Response lost; lost.hang=true; return lost;
            }
            if (request.path.endsWith("/actions") && request.body.value("action").toString()=="applied") {
                f.task.insert("status","adopted"); f.task.insert("stateVersion",f.task.value("stateVersion").toInt()+1);
                return Response{{{"task",f.task}}};
            }
            return original(request);
        };
        QVERIFY(sendLocal(*window,QStringLiteral("采用歌单")));
        QTRY_VERIFY(!originalId.isEmpty());
        auto adoptPosts=[&f] {int count=0;for(const auto &request:f.server.requests)if(request.method=="POST"&&request.body.value("action").toString()=="adopt")++count;return count;};
        QCOMPARE(adoptPosts(),1); QCOMPARE(window->findChild<QTableView*>("queueView")->model()->rowCount(),0);
        const auto device=window->findChild<MusicTaskClient*>()->deviceId();
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        const auto receiptPath="/v1/tasks/requests/"+device+"/"+originalId;
        QTRY_COMPARE(f.server.count("GET",receiptPath),1); QVERIFY(settled(*window)); QTest::qWait(50);
        QCOMPARE(adoptPosts(),1); QCOMPARE(window->findChild<QTableView*>("queueView")->model()->rowCount(),0);
        QVERIFY(sendLocal(*window,QStringLiteral("采用歌单"))); QVERIFY(settled(*window));
        QCOMPARE(adoptPosts(),1);
        if (committed) {
            QTRY_COMPARE(window->findChild<QTableView*>("queueView")->model()->rowCount(),1);
            QCOMPARE(f.server.count("GET",receiptPath),1);
        } else {
            QTRY_COMPARE(f.server.count("GET",receiptPath),2);
            QCOMPARE(window->findChild<QTableView*>("queueView")->model()->rowCount(),0);
        }
        destroy(window);
    }
    void adoptionJournalFailurePreventsDispatch() {
        Fixture f(true); MainWindow window; QVERIFY(open(f,window)); music(f); QVERIFY(send(window,f,QStringLiteral("推荐通勤歌曲")));
        QVERIFY(settled(window)); QSqlQuery query;
        QVERIFY(query.exec("CREATE TABLE task_adoption_requests (profile TEXT NOT NULL,device_id TEXT NOT NULL,task_id TEXT NOT NULL,"
                           "task_revision INTEGER NOT NULL,preview_version INTEGER NOT NULL,request_id TEXT NOT NULL UNIQUE,state TEXT NOT NULL,"
                           "PRIMARY KEY(profile,device_id,task_id,task_revision,preview_version))"));
        QVERIFY(query.exec("CREATE TRIGGER refuse_adoption_journal BEFORE INSERT ON task_adoption_requests BEGIN SELECT RAISE(ABORT,'owned test write failure'); END"));
        QVERIFY(sendLocal(window,QStringLiteral("采用歌单")));
        QTRY_VERIFY(window.findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains(QStringLiteral("无法记录当前采用请求")));
        QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),0);
        QCOMPARE(window.findChild<QTableView*>("queueView")->model()->rowCount(),0);
    }
    void adoptionFeedbackFollowsVerifiedFinalState_data() {
        QTest::addColumn<bool>("applied");
        QTest::newRow("adopted-after-reentrant-old-callback")<<true;
        QTest::newRow("genuine-unknown-keeps-warning")<<false;
    }
    void adoptionFeedbackFollowsVerifiedFinalState() {
        QFETCH(bool,applied);Fixture f(true);MainWindow window;QVERIFY(open(f,window));music(f);
        QVERIFY(send(window,f,QStringLiteral("推荐通勤歌曲")));QVERIFY(settled(window));
        const auto original=f.server.handler;QString originalRequest;
        f.server.handler=[&f,original,applied,&originalRequest](const Request &request)->Response {
            if(request.path.endsWith("/actions")&&request.body.value("action")=="adopt") {
                originalRequest=request.body.value("requestId").toString();
                if(!applied)return {{{"error",QJsonObject{{"message","owned fixture lost receipt"}}}},503};
                f.task.insert("stateVersion",f.task.value("stateVersion").toInt()+1);f.task.insert("status","awaiting_apply");
                f.task.insert("adoption",QJsonObject{{"id","verified-adoption"},{"previewVersion",1}});
                return {{{"task",f.task}}};
            }
            if(request.path.endsWith("/actions")&&request.body.value("action")=="applied") {
                f.task.insert("stateVersion",f.task.value("stateVersion").toInt()+1);f.task.insert("status","adopted");
                auto adoption=f.task.value("adoption").toObject();adoption.insert("appliedAt",QDateTime::currentMSecsSinceEpoch());f.task.insert("adoption",adoption);
                return {{{"task",f.task}}};
            }
            // 晚到的只读观察保持未完成，由下方显式信号固定回调顺序；不重发采用。
            if(request.path.endsWith("/late-adoption-observation")){Response response;response.hang=true;return response;}
            return original(request);
        };
        auto *task=window.findChild<MusicTaskClient*>();auto *dialog=window.findChild<RadioDialog*>();
        auto *text=dialog->findChild<QPlainTextEdit*>("text_RadioConversation");QVERIFY(task&&dialog&&text);
        QVERIFY(sendLocal(window,QStringLiteral("采用歌单")));QTRY_VERIFY(!originalRequest.isEmpty());
        auto posts=[&f]{int count=0;for(const auto &request:f.server.requests)if(request.method=="POST"&&request.body.value("action")=="adopt")++count;return count;};
        if(!applied) {
            QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("原采用结果尚未确认")));
            QCOMPARE(window.findChild<QTableView*>("queueView")->model()->rowCount(),0);QCOMPARE(posts(),1);return;
        }
        QTRY_COMPARE(task->snapshot().value("status").toString(),QString("adopted"));
        QTRY_VERIFY(!text->toPlainText().contains(QStringLiteral("原采用结果尚未确认")));
        QCOMPARE(window.findChild<QTableView*>("queueView")->model()->rowCount(),1);QCOMPARE(posts(),1);
        QSqlQuery query;QVERIFY(query.exec("UPDATE task_adoption_requests SET request_id='late-adoption-observation',state='unknown'"));query.finish();
        task->snapshotChanged(task->snapshot());
        const auto observedPath="/v1/tasks/requests/"+task->deviceId()+"/late-adoption-observation";
        QTRY_COMPARE(f.server.count("GET",observedPath),1);
        dialog->setPlaybackStatus(QStringLiteral("这份歌单已采用"));
        task->requestSettled("late-adoption-observation",false,"owned late unknown callback");
        QVERIFY(!text->toPlainText().contains(QStringLiteral("原采用结果尚未确认")));
        QCOMPARE(window.findChild<QTableView*>("queueView")->model()->rowCount(),1);QCOMPARE(posts(),1);
        QVERIFY(query.exec("SELECT state FROM task_adoption_requests"));QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(),QString("unknown"));query.finish();
    }
    void definiteControlRejectionIsDurableFailureWithoutRecoveryQuery() {
        Fixture f(true);auto window=std::make_unique<MainWindow>();QVERIFY(open(f,*window));music(f);
        QVERIFY(send(*window,f,QStringLiteral("推荐通勤歌曲")));QVERIFY(settled(*window));
        f.makePlan=[&f](const QString &id,const QString &text){return Fixture::plan(id,{"task_control"},
            {Fixture::action("control","task.control",text,{{"action","pause"},{"taskId",f.task.value("id")},{"expectedRevision",f.task.value("revision")}})});};
        const auto original=f.server.handler;
        f.server.handler=[original](const Request &request)->Response {
            if(request.path.endsWith("/actions"))return {{{"error",QJsonObject{{"code","TASK_REVISION_CONFLICT"},{"message","任务版本已更新"}}}},409};
            return original(request);
        };
        QVERIFY(send(*window,f,QStringLiteral("先暂停刚才的任务")));QVERIFY(settled(*window));const auto turn=f.messageId;
        QSqlQuery query;query.prepare("SELECT actions_json FROM radio_route_journal WHERE turn_id=?");query.addBindValue(turn);
        QVERIFY(query.exec());QVERIFY(query.next());
        QCOMPARE(QJsonDocument::fromJson(query.value(0).toByteArray()).object().value("control").toString(),QString("failed"));query.finish();
        QTRY_VERIFY(window->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains(QStringLiteral("后台已明确拒绝")));
        destroy(window);window=std::make_unique<MainWindow>();QVERIFY(open(f,*window));QVERIFY(settled(*window));QTest::qWait(70);
        QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),1);
        for(const auto &request:f.server.requests)QVERIFY(!request.path.startsWith("/v1/tasks/requests/"));
        QCOMPARE(window->findChild<QTableView*>("queueView")->model()->rowCount(),0);destroy(window);
    }
    void definiteAdoptionRejectionDoesNotBecomeUnknownOrRedispatch() {
        Fixture f(true);auto window=std::make_unique<MainWindow>();QVERIFY(open(f,*window));music(f);
        QVERIFY(send(*window,f,QStringLiteral("推荐通勤歌曲")));QVERIFY(settled(*window));
        const auto original=f.server.handler;
        f.server.handler=[original](const Request &request)->Response {
            if(request.path.endsWith("/actions")&&request.body.value("action").toString()=="adopt")
                return {{{"error",QJsonObject{{"code","NO_TASK_PREVIEW"},{"message","当前预览不可采用"}}}},409};
            return original(request);
        };
        QVERIFY(sendLocal(*window,QStringLiteral("采用歌单")));QVERIFY(settled(*window));
        QSqlQuery query;QVERIFY(query.exec("SELECT state FROM task_adoption_requests"));QVERIFY(query.next());QCOMPARE(query.value(0).toString(),QString("rejected"));query.finish();
        QVERIFY(sendLocal(*window,QStringLiteral("采用歌单")));QVERIFY(settled(*window));
        destroy(window);window=std::make_unique<MainWindow>();QVERIFY(open(f,*window));QVERIFY(settled(*window));
        QVERIFY(sendLocal(*window,QStringLiteral("采用歌单")));QVERIFY(settled(*window));
        QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),1);
        for(const auto &request:f.server.requests)QVERIFY(!request.path.startsWith("/v1/tasks/requests/"));
        QCOMPARE(window->findChild<QTableView*>("queueView")->model()->rowCount(),0);destroy(window);
    }
    void originalRouteWithoutMatchingActionFactsRemainsUnknown_data() {
        QTest::addColumn<bool>("missingFacts");QTest::newRow("legacy-without-facts")<<true;QTest::newRow("different-action")<<false;
    }
    void originalRouteWithoutMatchingActionFactsRemainsUnknown() {
        QFETCH(bool,missingFacts);Fixture f(true);auto window=std::make_unique<MainWindow>();QVERIFY(open(f,*window));music(f);
        const auto original=f.server.handler;
        f.server.handler=[original](const Request &request)->Response {
            auto response=original(request);if(request.method=="POST"&&request.path=="/v1/tasks")response.hang=true;return response;
        };
        QVERIFY(send(*window,f,QStringLiteral("推荐通勤歌曲")));QTRY_COMPARE(f.server.count("POST","/v1/tasks"),1);
        const auto turn=f.messageId;const auto device=window->findChild<ConversationClient*>()->deviceId();destroy(window);
        auto receipt=f.taskRequestReceipts.value(turn);QVERIFY(!receipt.isEmpty());
        if(missingFacts)receipt.remove("action");else {receipt.insert("action","cancel");receipt.insert("expectedRevision",1);}
        f.taskRequestReceipts.insert(turn,receipt);window=std::make_unique<MainWindow>();QVERIFY(open(f,*window));
        QTRY_COMPARE(f.server.count("GET","/v1/tasks/requests/"+device+"/"+turn),1);QVERIFY(settled(*window));
        QSqlQuery query;query.prepare("SELECT actions_json FROM radio_route_journal WHERE turn_id=?");query.addBindValue(turn);
        QVERIFY(query.exec());QVERIFY(query.next());
        QCOMPARE(QJsonDocument::fromJson(query.value(0).toByteArray()).object().value("music").toString(),QString("unknown"));query.finish();
        QVERIFY(sendLocal(*window,QStringLiteral("继续未完成步骤")));QVERIFY(settled(*window));
        QCOMPARE(f.server.count("POST","/v1/tasks"),1);QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),0);
        QVERIFY(window->findChild<MusicTaskClient*>()->snapshot().value("id").toString().isEmpty());
        QCOMPARE(window->findChild<QTableView*>("queueView")->model()->rowCount(),0);destroy(window);
    }
    void restoredControlNeedsItsOriginalActionTargetAndRevision_data() {
        QTest::addColumn<QString>("fault");
        for(const auto &value:QStringList{"valid","missing-facts","wrong-action","wrong-target","wrong-revision"})QTest::newRow(value.toUtf8())<<value;
    }
    void restoredControlNeedsItsOriginalActionTargetAndRevision() {
        QFETCH(QString,fault);Fixture f(true);auto window=std::make_unique<MainWindow>();QVERIFY(open(f,*window));music(f);
        QVERIFY(send(*window,f,QStringLiteral("推荐通勤歌曲")));QVERIFY(settled(*window));
        f.makePlan=[&f](const QString &id,const QString &text){return Fixture::plan(id,{"task_control"},
            {Fixture::action("control","task.control",text,{{"action","pause"},{"taskId",f.task.value("id")},{"expectedRevision",f.task.value("revision")}})});};
        const auto original=f.server.handler;QString requestId;
        f.server.handler=[original,&requestId](const Request &request)->Response {
            auto response=original(request);
            if(request.method=="POST"&&request.path.endsWith("/actions")) {requestId=request.body.value("requestId").toString();response.hang=true;}
            // 延迟原回执，确保测试等待的是校验完成，而非服务端刚收到请求。
            if(request.method=="GET"&&request.path.startsWith("/v1/tasks/requests/")&&
                request.path.section('/',-1)==requestId) response.delay=150;
            return response;
        };
        QVERIFY(send(*window,f,QStringLiteral("先暂停刚才的任务")));QTRY_VERIFY(!requestId.isEmpty());
        const auto turn=f.messageId;const auto device=window->findChild<ConversationClient*>()->deviceId();destroy(window);
        auto receipt=f.taskRequestReceipts.value(requestId);QVERIFY(!receipt.isEmpty());
        if(fault=="missing-facts"){receipt.remove("action");receipt.remove("expectedRevision");}
        if(fault=="wrong-action")receipt.insert("action","cancel");
        if(fault=="wrong-revision")receipt.insert("expectedRevision",99);
        if(fault=="wrong-target") {
            receipt.insert("taskId","music-routing-other-task");auto other=f.taskRequestSnapshots.value(requestId);other.insert("id","music-routing-other-task");
            f.taskRequestSnapshots.insert(requestId,other);
        }
        f.taskRequestReceipts.insert(requestId,receipt);window=std::make_unique<MainWindow>();
        auto *restoredTask=window->findChild<MusicTaskClient*>();QVERIFY(restoredTask);
        QSignalSpy settledReceipt(restoredTask,&MusicTaskClient::requestSettled);
        QSignalSpy rejectedReceipt(restoredTask,&MusicTaskClient::error);
        QVERIFY(open(f,*window));
        QTRY_COMPARE(f.server.count("GET","/v1/tasks/requests/"+device+"/"+requestId),1);QVERIFY(settled(*window));
        // 独立回执观察不占任务busy；正负例均须等正式回调完成再检查持久状态。
        if(fault=="valid") {
            QTRY_VERIFY([&] {
                for(const auto &arguments:settledReceipt)
                    if(arguments.value(0).toString()==requestId&&arguments.value(1).toBool()) return true;
                return false;
            }());
        } else {
            QTRY_VERIFY([&] {
                for(const auto &arguments:rejectedReceipt)
                    if(arguments.value(0).toString().contains(QStringLiteral("原操作回执格式无效"))) return true;
                return false;
            }());
        }
        QSqlQuery query;query.prepare("SELECT actions_json FROM radio_route_journal WHERE turn_id=?");query.addBindValue(turn);
        QVERIFY(query.exec());QVERIFY(query.next());
        QCOMPARE(QJsonDocument::fromJson(query.value(0).toByteArray()).object().value("control").toString(),fault=="valid"?QString("completed"):QString("unknown"));query.finish();
        QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),1);QCOMPARE(f.server.count("POST","/v1/tasks"),1);
        QCOMPARE(window->findChild<MusicTaskClient*>()->snapshot().value("id").toString(),QString("music-routing-task"));
        QCOMPARE(window->findChild<QTableView*>("queueView")->model()->rowCount(),0);destroy(window);
    }
    void cancelMemoryStillFindsIndependentMusic() {
        Fixture f(true);MainWindow window;QVERIFY(open(f,window));compound(f);QVERIFY(send(window,f,"remember jazz and find songs"));
        auto *dialog=window.findChild<RadioDialog*>();auto *panel=dialog->findChild<QWidget*>("panel_RadioPreferenceProposal");QVERIFY(panel->isHidden());
        QVERIFY(dialog->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("确认保存口味"));
        QTRY_COMPARE(f.server.count("POST","/v1/tasks"),1);
        dialog->findChild<QPushButton*>("button_RadioCancelPreference")->click();QVERIFY(panel->isHidden());
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(window.accountProfile()),QString());
        QCOMPARE(f.server.count("POST","/v1/tasks"),1);
        auto *transcript=dialog->findChild<QPlainTextEdit*>("text_RadioConversation");QTRY_VERIFY(transcript->toPlainText().contains("已取消这次记忆提议"));
    }
    void confirmedMemoryContinuesDependentMusicOnceWithEditedValue() {
        Fixture f(true);MainWindow window;QVERIFY(open(f,window));compound(f,true);QVERIFY(send(window,f,"remember jazz then find songs"));
        QCOMPARE(f.server.count("POST","/v1/tasks"),0);auto *dialog=window.findChild<RadioDialog*>();
        dialog->findChild<QLineEdit*>("edit_RadioPreferenceProposal")->setText("喜欢器乐爵士");
        auto *confirm=dialog->findChild<QPushButton*>("button_RadioConfirmPreference");confirm->click();confirm->click();
        QTRY_COMPARE(f.server.count("POST","/v1/tasks"),1);
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(window.accountProfile()),QString("喜欢器乐爵士"));
        Request posted;for(const auto &request:f.server.requests)if(request.path=="/v1/tasks")posted=request;
        QCOMPARE(posted.body.value("preference").toString(),QString("喜欢器乐爵士"));
        QCOMPARE(posted.body.value("instruction").toString(),QString("remember jazz then find songs"));QCOMPARE(posted.body.value("requestId").toString(),f.messageId);
        QVERIFY(dialog->findChild<QWidget*>("panel_RadioPreferenceProposal")->isHidden());
    }
    void cancellingDependencySkipsOnlyDependentAction() {
        Fixture f(true);MainWindow window;QVERIFY(open(f,window));compound(f,true);QVERIFY(send(window,f,"remember jazz then find songs"));
        auto *dialog=window.findChild<RadioDialog*>();dialog->findChild<QPushButton*>("button_RadioCancelPreference")->click();
        QTest::qWait(60);QCOMPARE(f.server.count("POST","/v1/tasks"),0);
        QTRY_VERIFY(dialog->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("因此没有执行"));
    }
    void naturalMemoryWithdrawalInvalidatesActuallyDeliveredConfirmation() {
        Fixture f(true); MainWindow window; QVERIFY(open(f,window)); compound(f,true);
        QVERIFY(send(window,f,QStringLiteral("记住我喜欢爵士，然后再推荐歌曲")));
        auto *dialog=window.findChild<RadioDialog*>();
        QTRY_VERIFY(dialog->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains(QStringLiteral("确认保存口味")));
        const auto originalActionId=f.messageId+QStringLiteral(":memory");
        QVERIFY(sendLocal(window,QStringLiteral("不记了，我撤回刚才那条，只这次聊聊。")));
        QTRY_VERIFY(dialog->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains(QStringLiteral("已取消这次记忆提议")));
        // 重放刚才实际出现过的确认标识，不能把已取消提议重新保存或唤醒依赖任务。
        emit dialog->preferenceProposalDecided(originalActionId,true,QStringLiteral("喜欢爵士；少人声"));
        QTest::qWait(60);
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(window.accountProfile()),QString());
        QCOMPARE(preferenceRevision(window.accountProfile()),0);
        QCOMPARE(f.server.count("POST","/v1/tasks"),0);
    }
    void clearPreferenceRequiresConfirmationAndKeepsIndependentMusic() {
        Fixture f(true); MainWindow window; QVERIFY(open(f,window));
        QString error; QVERIFY(RadioStore(QSqlDatabase::database()).save(window.accountProfile(),"喜欢爵士",&error));
        f.makePlan=[](const QString &id,const QString &text){return Fixture::plan(id,{"preference_deletion","scene_research"},{
            Fixture::action("clear","memory.propose",text,{{"operation","clear"},{"preference",""}}),
            Fixture::action("music","music.task",text,{{"mode","new"}})});};
        QVERIFY(send(window,f,"clear my preferences and find rock"));
        auto *dialog=window.findChild<RadioDialog*>();
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(window.accountProfile()),QString("喜欢爵士"));
        QVERIFY(dialog->findChild<QLineEdit*>("edit_RadioPreferenceProposal")->isReadOnly());
        dialog->findChild<QPushButton*>("button_RadioConfirmPreference")->click();
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(window.accountProfile()),QString());
        QTRY_COMPARE(f.server.count("POST","/v1/tasks"),1);
    }
    void continueAndNewUseDifferentTaskOperations() {
        Fixture f(true);MainWindow window;QVERIFY(open(f,window));music(f);QVERIFY(send(window,f,"find jazz"));QTRY_COMPARE(f.task.value("revision").toInt(),1);
        music(f,"continue");QVERIFY(send(window,f,"less vocals"));QTRY_COMPARE(f.task.value("revision").toInt(),2);
        QCOMPARE(f.server.count("POST","/v1/tasks"),1);QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),1);
        music(f,"new");QVERIFY(send(window,f,"change topic find rock"));QTRY_COMPARE(f.server.count("POST","/v1/tasks"),2);
    }
    void controlRequiresExactTaskVersionAndStableRequestId() {
        Fixture f(true);MainWindow window;QVERIFY(open(f,window));music(f);QVERIFY(send(window,f,"find jazz"));QTRY_COMPARE(f.task.value("revision").toInt(),1);
        f.makePlan=[](const QString &id,const QString &text){return Fixture::plan(id,{"task_control"},{Fixture::action("control","task.control",text,{{"action","pause"},{"taskId","music-routing-task"},{"expectedRevision",2}})});};
        QVERIFY(send(window,f,"pause now"));QTest::qWait(40);QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),0);
        f.makePlan=[](const QString &id,const QString &text){return Fixture::plan(id,{"task_control"},{Fixture::action("control","task.control",text,{{"action","pause"},{"taskId","music-routing-task"},{"expectedRevision",1}})});};
        QVERIFY(send(window,f,"pause this task"));QTRY_COMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),1);
        Request posted;for(const auto &request:f.server.requests)if(request.path.endsWith("/actions"))posted=request;
        QCOMPARE(posted.body.value("action").toString(),QString("pause"));QVERIFY(posted.body.value("requestId").toString().startsWith("route-"));QCOMPARE(posted.body.value("requestId").toString().size(),70);
    }
    void invalidRouteDoesNotUseLegacyOrStartMusic() {
        Fixture f(true);MainWindow window;QVERIFY(open(f,window));
        f.makePlan=[](const QString &id,const QString &text){return Fixture::plan(id,{"help"},{Fixture::action("bad","shell.exec",text,{{"command","anything"}})});};
        auto *dialog=window.findChild<RadioDialog*>();auto *input=dialog->findChild<QPlainTextEdit*>("edit_RadioInstruction");input->setPlainText("unknown action");dialog->findChild<QPushButton*>("button_RadioGenerate")->click();
        QTRY_VERIFY(dialog->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("未执行任何能力"));
        QCOMPARE(f.server.count("POST","/v1/tasks"),0);QCOMPARE(f.server.count("POST","/v1/assistant/plan"),0);QVERIFY(input->toPlainText().isEmpty());
    }
    void olderConfirmationCannotReplaceNewerMusicTurn() {
        Fixture f(true); MainWindow window; QVERIFY(open(f,window)); compound(f,true);
        QVERIFY(send(window,f,"remember jazz then find songs"));
        music(f); QVERIFY(send(window,f,"new topic find rock")); QTRY_COMPARE(f.server.count("POST","/v1/tasks"),1);
        auto *dialog=window.findChild<RadioDialog*>();
        dialog->findChild<QPushButton*>("button_RadioConfirmPreference")->click(); QTest::qWait(60);
        QCOMPARE(f.server.count("POST","/v1/tasks"),1);
        QTRY_VERIFY(dialog->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("较早的找歌操作未继续执行"));
    }
    void staleMemorySummaryCannotOverwriteNewLocalPreference() {
        Fixture f(true); MainWindow window; QVERIFY(open(f,window)); compound(f,true);
        QVERIFY(send(window,f,"remember jazz then find songs"));
        QString error; QVERIFY(RadioStore(QSqlDatabase::database()).save(window.accountProfile(),"更新后的器乐偏好",&error));
        auto *dialog=window.findChild<RadioDialog*>(); dialog->findChild<QPushButton*>("button_RadioConfirmPreference")->click();
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(window.accountProfile()),QString("更新后的器乐偏好"));
        QTRY_VERIFY(dialog->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("未用旧摘要覆盖"));
        QCOMPARE(f.server.count("POST","/v1/tasks"),0);
    }
    void delayedContinueRequiresServerBoundTaskRevision() {
        Fixture f(true); MainWindow window; QVERIFY(open(f,window)); music(f); QVERIFY(send(window,f,"find jazz"));
        auto *task=window.findChild<MusicTaskClient*>(); QTRY_COMPARE(task->snapshot().value("revision").toInt(),1);
        f.makePlan=[&f](const QString &id,const QString &text){return Fixture::plan(id,{"preference_proposal","scene_research"},{
            Fixture::action("memory","memory.propose",text,{{"operation","set"},{"preference","喜欢爵士"}}),
            Fixture::action("music","music.task",text,{{"mode","continue"},{"taskId",f.task.value("id")},{"expectedRevision",f.task.value("revision")}},QJsonArray{"memory"})});};
        QVERIFY(send(window,f,"remember jazz and adjust this playlist"));
        task->action("turn","independent direct edit"); QTRY_COMPARE(task->snapshot().value("revision").toInt(),2);
        auto *dialog=window.findChild<RadioDialog*>(); dialog->findChild<QPushButton*>("button_RadioConfirmPreference")->click();
        QTRY_VERIFY(dialog->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("较早的找歌操作未继续执行"));
        QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),1);
    }
    void accountSwitchInvalidatesPendingConfirmation() {
        Fixture f(true);MainWindow window;QVERIFY(open(f,window));compound(f,true);QVERIFY(send(window,f,"remember jazz then find songs"));
        auto *dialog=window.findChild<RadioDialog*>();const auto oldProfile=window.accountProfile();
        QVERIFY(dialog->findChild<QWidget*>("panel_RadioPreferenceProposal")->isHidden());
        QVERIFY(dialog->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("确认保存口味"));
        QVERIFY(f.login("another-account"));QVERIFY(dialog->findChild<QWidget*>("panel_RadioPreferenceProposal")->isHidden());
        dialog->findChild<QPushButton*>("button_RadioConfirmPreference")->click();QTest::qWait(40);
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(oldProfile),QString());QCOMPARE(f.server.count("POST","/v1/tasks"),0);
    }
    void restartRestoresUnconfirmedMemoryWithoutDispatch_data() {
        QTest::addColumn<bool>("clear");
        QTest::newRow("save-preference") << false;
        QTest::newRow("clear-preference") << true;
    }
    void restartRestoresUnconfirmedMemoryWithoutDispatch() {
        QFETCH(bool,clear); Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        const auto profile=window->accountProfile();
        QVERIFY(RadioStore(QSqlDatabase::database()).save(profile,"原有爵士口味")); const int revision=preferenceRevision(profile);
        if (clear) f.makePlan=[](const QString &id,const QString &text) {
            return Fixture::plan(id,{"preference_deletion"},{Fixture::action("memory","memory.propose",text,{{"operation","clear"},{"preference",""}})});
        };
        else compound(f,true);
        QVERIFY(send(*window,f,clear?"清除我保存的口味":"remember jazz then find songs")); QVERIFY(settled(*window));
        const auto mid=f.messageId; const auto route=f.routePath(); const auto path=QSqlDatabase::database().databaseName();
        QCOMPARE(f.server.count("POST",route),1); QCOMPARE(f.server.count("POST","/v1/tasks"),0);
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        QTRY_VERIFY(f.server.count("GET",route)>0); QVERIFY(settled(*window));
        QCOMPARE(QSqlDatabase::database().databaseName(),path);
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile),QString("原有爵士口味"));
        QCOMPARE(preferenceRevision(profile),revision); QCOMPARE(f.server.count("POST",route),1);
        QCOMPARE(f.server.count("POST","/v1/tasks"),0); QCOMPARE(f.server.count("POST","/v1/assistant/plan"),0);
        auto *editor=window->findChild<QLineEdit*>("edit_RadioPreferenceProposal");
        QTRY_COMPARE(editor->isReadOnly(),clear);
        if (!clear) QTRY_COMPARE(editor->text(),QString("喜欢爵士；少人声"));
        QVERIFY(f.routePlans.value(mid).value("messageId").toString()==mid);
        destroy(window);
    }
    void restoredEditedConfirmationIsCommittedAndDispatchedOnce() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); compound(f,true);
        QVERIFY(send(*window,f,"remember jazz then find songs")); QVERIFY(settled(*window));
        const auto profile=window->accountProfile(); const auto route=f.routePath(); const auto mid=f.messageId;
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        QTRY_VERIFY(f.server.count("GET",route)>0);
        auto *editor=window->findChild<QLineEdit*>("edit_RadioPreferenceProposal");
        QTRY_COMPARE(editor->text(),QString("喜欢爵士；少人声")); editor->setText("喜欢器乐爵士");
        auto *confirm=window->findChild<QPushButton*>("button_RadioConfirmPreference"); confirm->click(); confirm->click();
        QTRY_COMPARE(f.server.count("POST","/v1/tasks"),1); QVERIFY(settled(*window));
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile),QString("喜欢器乐爵士"));
        QCOMPARE(preferenceRevision(profile),1); QCOMPARE(f.server.count("POST",route),1);
        Request posted; for(const auto &request:f.server.requests)if(request.path=="/v1/tasks")posted=request;
        QCOMPARE(posted.body.value("requestId").toString(),mid); QCOMPARE(posted.body.value("preference").toString(),QString("喜欢器乐爵士"));
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); QVERIFY(settled(*window)); QTest::qWait(80);
        window->findChild<QPushButton*>("button_RadioConfirmPreference")->click(); QTest::qWait(40);
        QCOMPARE(preferenceRevision(profile),1); QCOMPARE(f.server.count("POST","/v1/tasks"),1);
        destroy(window);
    }
    void restoredCancellationSkipsDependentMusicWithoutChangingPreference() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); compound(f,true);
        QVERIFY(send(*window,f,"remember jazz then find songs")); QVERIFY(settled(*window));
        const auto route=f.routePath(); const auto profile=window->accountProfile();
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        QTRY_VERIFY(f.server.count("GET",route)>0);
        QTRY_COMPARE(window->findChild<QLineEdit*>("edit_RadioPreferenceProposal")->text(),QString("喜欢爵士；少人声"));
        QVERIFY(sendLocal(*window,"取消口味"));
        QTRY_VERIFY(window->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("因此没有执行"));
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile),QString()); QCOMPARE(preferenceRevision(profile),0);
        QCOMPARE(f.server.count("POST","/v1/tasks"),0); QCOMPARE(f.server.count("POST",route),1);
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); QVERIFY(settled(*window)); QTest::qWait(80);
        QVERIFY(sendLocal(*window,"确认保存口味"));
        QTRY_VERIFY(window->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("当前没有可确认或取消的口味提议"));
        QCOMPARE(f.server.count("POST","/v1/tasks"),0); QCOMPARE(preferenceRevision(profile),0);
        destroy(window);
    }
    void restoredClearConfirmationRemovesPreferenceOnlyOnce() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        const auto profile=window->accountProfile(); QVERIFY(RadioStore(QSqlDatabase::database()).save(profile,"喜欢爵士"));
        f.makePlan=[](const QString &id,const QString &text) {
            return Fixture::plan(id,{"preference_deletion"},{Fixture::action("memory","memory.propose",text,{{"operation","clear"},{"preference",""}})});
        };
        QVERIFY(send(*window,f,"清除我保存的口味")); QVERIFY(settled(*window)); const auto route=f.routePath();
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        QTRY_VERIFY(f.server.count("GET",route)>0); QTRY_VERIFY(window->findChild<QLineEdit*>("edit_RadioPreferenceProposal")->isReadOnly());
        auto *confirm=window->findChild<QPushButton*>("button_RadioConfirmPreference"); confirm->click(); confirm->click();
        QTRY_COMPARE(RadioStore(QSqlDatabase::database()).preference(profile),QString()); QCOMPARE(preferenceRevision(profile),2);
        QVERIFY(settled(*window)); QCOMPARE(f.server.count("POST",route),1); QCOMPARE(f.server.count("POST","/v1/tasks"),0);
        destroy(window);
    }
    void restoredProposalCannotOverwritePreferenceAfterABAChange() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        const auto profile=window->accountProfile(); QVERIFY(RadioStore(QSqlDatabase::database()).save(profile,"最初口味")); compound(f,true);
        QVERIFY(send(*window,f,"remember jazz then find songs")); QVERIFY(settled(*window)); const auto route=f.routePath();
        QVERIFY(RadioStore(QSqlDatabase::database()).save(profile,"临时更新口味"));
        QVERIFY(RadioStore(QSqlDatabase::database()).save(profile,"最初口味")); QCOMPARE(preferenceRevision(profile),3);
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        QTRY_VERIFY(f.server.count("GET",route)>0); QVERIFY(settled(*window));
        window->findChild<QPushButton*>("button_RadioConfirmPreference")->click(); QTest::qWait(80);
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile),QString("最初口味")); QCOMPARE(preferenceRevision(profile),3);
        QCOMPARE(f.server.count("POST","/v1/tasks"),0); QCOMPARE(f.server.count("POST",route),1);
        destroy(window);
    }
    void otherAccountCannotRestoreOrConfirmOriginalProposal() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); compound(f,true);
        QVERIFY(send(*window,f,"remember jazz then find songs")); QVERIFY(settled(*window));
        const auto owner=window->accountProfile(); const auto route=f.routePath(); destroy(window);
        window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window,"other-user")); QVERIFY(settled(*window));
        const auto other=window->accountProfile(); QVERIFY(other!=owner);
        QCOMPARE(f.server.count("GET",route),0);
        QVERIFY(sendLocal(*window,"确认保存口味"));
        QTRY_VERIFY(window->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("当前没有可确认或取消的口味提议"));
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(other),QString()); QCOMPARE(f.server.count("POST","/v1/tasks"),0);
        destroy(window); QVERIFY(f.login()); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        QTRY_VERIFY(f.server.count("GET",route)>0); QCOMPARE(window->accountProfile(),owner);
        QTRY_COMPARE(window->findChild<QLineEdit*>("edit_RadioPreferenceProposal")->text(),QString("喜欢爵士；少人声"));
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(owner),QString()); QCOMPARE(f.server.count("POST","/v1/tasks"),0);
        destroy(window);
    }
    void confirmedButUndispatchedStepWaitsForExplicitContinuation() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); compound(f,true);
        QVERIFY(send(*window,f,"remember jazz then find songs")); QVERIFY(settled(*window));
        const auto profile=window->accountProfile(); const auto route=f.routePath(); const auto mid=f.messageId;
        const auto *conversation=window->findChild<ConversationClient*>();
        const auto cid=conversation->snapshot().value("id").toString(); const auto device=conversation->deviceId();
        QString error; QJsonObject receipt;
        // 模拟确认事务刚提交、界面尚未来得及派发依赖动作的崩溃点。
        QVERIFY(RadioStore(QSqlDatabase::database()).setRouteConfirmationTurn(profile,device,cid,mid,mid,&error));
        QVERIFY2(RadioStore(QSqlDatabase::database()).decidePreference(profile,device,cid,mid,"memory",true,"喜欢器乐爵士",&receipt,&error),qPrintable(error));
        QCOMPARE(f.server.count("POST","/v1/tasks"),0);
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        QTRY_VERIFY(f.server.count("GET",route)>0); QVERIFY(settled(*window));
        QTRY_VERIFY(window->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("继续未完成步骤"));
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile),QString("喜欢器乐爵士"));
        QCOMPARE(preferenceRevision(profile),1); QCOMPARE(f.server.count("POST","/v1/tasks"),0);
        QVERIFY(sendLocal(*window,"继续未完成步骤")); QTRY_COMPARE(f.server.count("POST","/v1/tasks"),1);
        QVERIFY(settled(*window)); QCOMPARE(f.server.count("POST",route),1); QCOMPARE(preferenceRevision(profile),1);
        Request posted;for(const auto &request:f.server.requests)if(request.path=="/v1/tasks")posted=request;
        QCOMPARE(posted.body.value("requestId").toString(),mid);
        destroy(window);
    }
    void dispatchedRequestAfterRestartIsOnlyObserved_data() {
        QTest::addColumn<bool>("committed");
        QTest::newRow("commit-with-lost-receipt") << true;
        QTest::newRow("unknown-without-receipt") << false;
    }
    void dispatchedRequestAfterRestartIsOnlyObserved() {
        QFETCH(bool,committed); Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); compound(f,true);
        QVERIFY(send(*window,f,"remember jazz then find songs")); QVERIFY(settled(*window));
        const auto route=f.routePath(); const auto mid=f.messageId;
        const auto device=window->findChild<ConversationClient*>()->deviceId();
        const auto original=f.server.handler;
        f.server.handler=[original,committed](const Request &request)->Response {
            if (request.method=="POST" && request.path=="/v1/tasks") {
                auto response=committed ? original(request) : Response{};
                response.hang=true; return response;
            }
            return original(request);
        };
        window->findChild<QPushButton*>("button_RadioConfirmPreference")->click();
        QTRY_COMPARE(f.server.count("POST","/v1/tasks"),1);
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        const auto receiptPath="/v1/tasks/requests/"+device+"/"+mid;
        QTRY_VERIFY(f.server.count("GET",route)>0); QTRY_COMPARE(f.server.count("GET",receiptPath),1);
        QVERIFY(settled(*window)); QTest::qWait(100);
        QCOMPARE(f.server.count("POST","/v1/tasks"),1); QCOMPARE(f.server.count("POST",route),1);
        QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),0);
        if (committed) QTRY_COMPARE(window->findChild<MusicTaskClient*>()->snapshot().value("id").toString(),QString("music-routing-task"));
        else {
            const auto snapshot=window->findChild<MusicTaskClient*>()->snapshot();
            QVERIFY(snapshot.value("id").toString().isEmpty());
            QVERIFY(snapshot.value("clientObservationOnly").toBool());
        }
        QVERIFY(sendLocal(*window,"继续未完成步骤"));
        QTest::qWait(80); QCOMPARE(f.server.count("POST","/v1/tasks"),1);
        QCOMPARE(f.server.count("POST",route),1); QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),0);
        QCOMPARE(preferenceRevision(window->accountProfile()),1);
        destroy(window);
    }
    void changedServerPlanCannotRestoreLocalAuthorization() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); compound(f,true);
        QVERIFY(send(*window,f,"remember jazz then find songs")); QVERIFY(settled(*window));
        const auto route=f.routePath(); const auto mid=f.messageId; const auto profile=window->accountProfile();
        auto plan=f.routePlans.value(mid); auto actions=plan.value("actions").toArray(); auto memory=actions.first().toObject();
        memory.insert("parameters",QJsonObject{{"operation","set"},{"preference","被篡改的偏好"}}); actions[0]=memory; plan.insert("actions",actions);
        f.routePlans.insert(mid,plan);
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        QTRY_VERIFY(f.server.count("GET",route)>0); QVERIFY(settled(*window));
        QVERIFY(sendLocal(*window,"确认保存口味"));
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile),QString()); QCOMPARE(preferenceRevision(profile),0);
        QCOMPARE(f.server.count("POST","/v1/tasks"),0); QCOMPARE(f.server.count("POST",route),1);
        destroy(window);
    }
    void restoredContinuationCannotUseOlderTaskRevision() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); music(f);
        QVERIFY(send(*window,f,"find jazz")); QVERIFY(settled(*window));
        auto *task=window->findChild<MusicTaskClient*>(); QTRY_COMPARE(task->snapshot().value("revision").toInt(),1);
        f.makePlan=[&f](const QString &id,const QString &text) {
            return Fixture::plan(id,{"preference_proposal","scene_research"},{
                Fixture::action("memory","memory.propose",text,{{"operation","set"},{"preference","喜欢爵士"}}),
                Fixture::action("music","music.task",text,{{"mode","continue"},{"taskId",f.task.value("id")},{"expectedRevision",f.task.value("revision")}},QJsonArray{"memory"})});
        };
        QVERIFY(send(*window,f,"remember jazz and adjust this playlist")); QVERIFY(settled(*window)); const auto route=f.routePath();
        task->action("turn","independent direct edit"); QTRY_COMPARE(task->snapshot().value("revision").toInt(),2); QVERIFY(settled(*window));
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        QTRY_VERIFY(f.server.count("GET",route)>0);
        QTRY_COMPARE(window->findChild<MusicTaskClient*>()->snapshot().value("revision").toInt(),2);
        QTRY_COMPARE(window->findChild<QLineEdit*>("edit_RadioPreferenceProposal")->text(),QString("喜欢爵士"));
        window->findChild<QPushButton*>("button_RadioConfirmPreference")->click(); QVERIFY(settled(*window));
        QTRY_VERIFY(window->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("较早的找歌操作未继续执行"));
        QCOMPARE(f.server.count("POST","/v1/tasks"),1); QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),1);
        QCOMPARE(window->findChild<MusicTaskClient*>()->snapshot().value("revision").toInt(),2);
        destroy(window);
    }
    void olderProposalDoesNotReviveAfterNewerConversationTurnAndRestart() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); compound(f,true);
        QVERIFY(send(*window,f,"remember jazz then find songs")); QVERIFY(settled(*window)); const auto olderRoute=f.routePath();
        music(f); QVERIFY(send(*window,f,"new topic find rock")); QVERIFY(settled(*window));
        QCOMPARE(f.server.count("POST","/v1/tasks"),1); const auto profile=window->accountProfile();
        destroy(window); window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        QTRY_VERIFY(f.server.count("GET",olderRoute)>0); QVERIFY(settled(*window));
        QVERIFY(sendLocal(*window,"确认保存口味"));
        QTRY_VERIFY(window->findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("当前没有可确认或取消的口味提议"));
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile),QString()); QCOMPARE(preferenceRevision(profile),0);
        QCOMPARE(f.server.count("POST","/v1/tasks"),1); QCOMPARE(f.server.count("POST",olderRoute),1);
        destroy(window);
    }
    void oldUnknownReceiptCannotReplaceNewerLinkedTask() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); compound(f,true);
        QVERIFY(send(*window,f,"remember jazz then find songs")); QVERIFY(settled(*window));
        const auto mid=f.messageId; const auto route=f.routePath(); const auto device=window->findChild<ConversationClient*>()->deviceId();
        const auto original=f.server.handler;
        f.server.handler=[original](const Request &request)->Response {
            auto response=original(request);
            if(request.method=="POST" && request.path=="/v1/tasks") response.hang=true;
            return response;
        };
        window->findChild<QPushButton*>("button_RadioConfirmPreference")->click(); QTRY_COMPARE(f.server.count("POST","/v1/tasks"),1);
        destroy(window);
        // 设备离线期间，后台已关联较新的任务；旧请求查询只能补事实，不能抢占它。
        auto newer=f.task; newer.insert("id","music-routing-newer-task"); newer.insert("revision",3); newer.insert("stateVersion",8);
        auto preview=newer.value("preview").toObject(); preview.insert("version",3); preview.insert("title","newer linked playlist"); newer.insert("preview",preview);
        f.task=newer; f.append("newer-user-turn","newer-user-turn","user","user","new topic find rock");
        f.current.insert("latestTaskId",newer.value("id"));
        f.current.insert("latestTaskLink",QJsonObject{{"taskId",newer.value("id")},{"turnId","newer-user-turn"},{"revision",3}});
        f.server.handler=[original,newer](const Request &request)->Response {
            if (request.method=="GET" && request.path=="/v1/tasks/music-routing-newer-task") return {{{"task",newer}}};
            return original(request);
        };
        window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        QTRY_VERIFY(f.server.count("GET",route)>0);
        const auto receiptPath="/v1/tasks/requests/"+device+"/"+mid;
        QTRY_COMPARE(f.server.count("GET",receiptPath),1); QVERIFY(settled(*window)); QTest::qWait(100);
        auto *task=window->findChild<MusicTaskClient*>();
        QCOMPARE(task->snapshot().value("id").toString(),QString("music-routing-newer-task"));
        QCOMPARE(task->snapshot().value("revision").toInt(),3);
        QCOMPARE(task->snapshot().value("preview").toObject().value("title").toString(),QString("newer linked playlist"));
        QCOMPARE(f.server.count("POST","/v1/tasks"),1); QCOMPARE(f.server.count("POST",route),1);
        QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),0);
        destroy(window);
    }
    void failedMemoryTransitionDoesNotBlockAnotherProposal() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window));
        bool triggerCreated=false;
        f.makePlan=[&triggerCreated](const QString &id,const QString &text) {
            if (!triggerCreated) {
                QSqlQuery query;
                triggerCreated=query.exec("CREATE TEMP TRIGGER reject_first_memory_transition BEFORE UPDATE ON radio_route_journal "
                    "WHEN OLD.turn_id='"+id+"' BEGIN SELECT RAISE(ABORT,'owned transition failure'); END");
            }
            return Fixture::plan(id,{"preference_proposal"},{Fixture::action("memory","memory.propose",text,
                {{"operation","set"},{"preference",text.contains("second")?"第二条器乐口味":"第一条爵士口味"}})});
        };
        QVERIFY(send(*window,f,"remember first jazz preference")); QVERIFY(triggerCreated); QVERIFY(settled(*window));
        QCOMPARE(window->findChild<QLineEdit*>("edit_RadioPreferenceProposal")->text(),QString());
        QVERIFY(send(*window,f,"remember second instrumental preference")); QVERIFY(settled(*window));
        QTRY_COMPARE(window->findChild<QLineEdit*>("edit_RadioPreferenceProposal")->text(),QString("第二条器乐口味"));
        window->findChild<QPushButton*>("button_RadioConfirmPreference")->click(); QVERIFY(settled(*window));
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(window->accountProfile()),QString("第二条器乐口味"));
        QCOMPARE(preferenceRevision(window->accountProfile()),1); QCOMPARE(f.server.count("POST","/v1/tasks"),0);
        destroy(window);
    }
    void olderMemoryConfirmationCannotPauseAfterNewerTopic() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); music(f);
        QVERIFY(send(*window,f,"find jazz")); QVERIFY(settled(*window));
        f.makePlan=[&f](const QString &id,const QString &text) {
            return Fixture::plan(id,{"preference_proposal","task_control"},{
                Fixture::action("memory","memory.propose",text,{{"operation","set"},{"preference","喜欢爵士"}}),
                Fixture::action("control","task.control",text,{{"action","pause"},{"taskId",f.task.value("id")},{"expectedRevision",f.task.value("revision")}},QJsonArray{"memory"})});
        };
        QVERIFY(send(*window,f,"remember jazz then pause this task")); QVERIFY(settled(*window));
        f.makePlan=[](const QString &id,const QString &text) {
            return Fixture::plan(id,{"help"},{Fixture::action("reply","assistant.reply",text,{{"kind","help"},{"text","这里是新话题的帮助说明"}})});
        };
        QVERIFY(send(*window,f,"new topic how can I use this app")); QVERIFY(settled(*window));
        auto *confirm=window->findChild<QPushButton*>("button_RadioConfirmPreference"); confirm->click(); confirm->click();
        QVERIFY(settled(*window)); QTest::qWait(80);
        QCOMPARE(f.server.count("POST","/v1/tasks/music-routing-task/actions"),0);
        QCOMPARE(window->findChild<MusicTaskClient*>()->snapshot().value("status").toString(),QString("preview"));
        QCOMPARE(window->findChild<MusicTaskClient*>()->snapshot().value("revision").toInt(),1);
        QCOMPARE(f.server.count("POST","/v1/tasks"),1);
        destroy(window);
    }
    void failedConfirmationTransactionKeepsProposalRetryable() {
        Fixture f(true); auto window=std::make_unique<MainWindow>(); QVERIFY(open(f,*window)); compound(f,true);
        QVERIFY(send(*window,f,"remember jazz then find songs")); QVERIFY(settled(*window));
        const auto profile=window->accountProfile();
        {
            QSqlQuery query;
            QVERIFY(query.exec("CREATE TEMP TRIGGER reject_confirmation_receipt BEFORE UPDATE ON radio_route_journal "
                "WHEN NEW.receipts_json<>OLD.receipts_json BEGIN SELECT RAISE(ABORT,'owned confirmation failure'); END"));
        }
        auto *confirm=window->findChild<QPushButton*>("button_RadioConfirmPreference"); confirm->click(); QVERIFY(settled(*window));
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile),QString()); QCOMPARE(preferenceRevision(profile),0);
        QCOMPARE(f.server.count("POST","/v1/tasks"),0); QTRY_VERIFY(confirm->isEnabled());
        QTRY_COMPARE(window->findChild<QLineEdit*>("edit_RadioPreferenceProposal")->text(),QString("喜欢爵士；少人声"));
        {
            QSqlQuery query; QVERIFY(query.exec("DROP TRIGGER reject_confirmation_receipt"));
        }
        confirm->click(); confirm->click(); QTRY_COMPARE(f.server.count("POST","/v1/tasks"),1); QVERIFY(settled(*window));
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile),QString("喜欢爵士；少人声")); QCOMPARE(preferenceRevision(profile),1);
        destroy(window);
    }
};
QTEST_MAIN(RoutingFlowTest)
#include "routing_flow_test.moc"
