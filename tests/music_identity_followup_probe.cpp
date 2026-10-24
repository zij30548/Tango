// 复用封存探针的私有连接、只读观测和原生窗口，不修改原源码或运行它的旧main。
#define main tango_music_requirements_original_main
#include "music_requirements_live_probe.cpp"
#undef main
#include "music_identity_followup_helpers.h"

namespace {
using IdentityFollowupProbe::verifiedArtistOrdinal;
QString identitySettle(NativeSession &session,int timeoutMs,const QString &boundTurn={}) {
    return waitForProbe([&]{const auto current=session.conversation->snapshot();const auto latest=latestUser(current);bool known=false;
        QJsonArray routes;
        for(const auto &value:readRoutes(session.window->accountProfile(),session.conversation->deviceId(),current.value("id").toString(),&known)){
            auto row=value.toObject();
            // 身份确认续接原动作；只观察本轮或已核候选绑定的原轮，不让其它旧历史影响等待。
            if(!boundTurn.isEmpty()&&row.value("turnId").toString()==boundTurn){row.insert("originTurnId",boundTurn);row.insert("turnId",latest);}
            routes.append(row);
        }
        return QJsonObject{{"conversationBusy",session.conversation->busy()||current.value("journalPending").toBool()},
            {"taskBusy",session.task->busy()},{"task",session.task->snapshot()},{"conversation",current},
            {"routesKnown",known},{"routes",routes},{"latestMessage",latest},
            {"catalogChoices",session.window->property("tangoCatalogChoices").toJsonObject()},{"pendingMemory",session.pendingMemory()}};
    },timeoutMs);
}
QJsonObject selectionEvidence(const QJsonObject &before) {
    const auto choice=before.value("catalogChoices").toObject();const auto task=before.value("task").toObject();QJsonArray candidates;
    QString kind,turn;
    if(!choice.isEmpty()&&choice.value("turn")==before.value("messageId")&&
        choice.value("conversation")==before.value("conversationId")){
        candidates=choice.value("artists").toArray();kind="current_catalog_choices";turn=choice.value("turn").toString();
    } else if(task.value("status")=="needs_clarification"&&task.value("id")==before.value("conversation").toObject().value("latestTaskId")&&
              task.value("brief").toObject().value("requiredArtist").toString()==QStringLiteral("邓紫棋")){
        candidates=task.value("providedArtists").toArray();kind="current_task_provided_artists";turn=before.value("messageId").toString();
    }
    const int ordinal=verifiedArtistOrdinal(candidates);if(!ordinal)return {};
    const auto selected=candidates.at(ordinal-1).toObject();
    return {{"kind",kind},{"ordinal",ordinal},{"id",selected.value("id")},{"name",selected.value("name")},
        {"candidates",candidates},{"boundTurn",turn},{"conversationId",before.value("conversationId")},
        {"taskId",task.value("id")},{"revision",task.value("revision")}};
}
int checkIdentityGuards() {
    const QJsonObject canonical{{"id",7763},{"name","G.E.M.邓紫棋"}};
    const QJsonObject falseName{{"id",9},{"name","G.E.M.邓紫棋"}};
    if(verifiedArtistOrdinal({falseName,canonical})!=2||verifiedArtistOrdinal({falseName})!=0||
        verifiedArtistOrdinal({canonical,canonical})!=0||
        verifiedArtistOrdinal({QJsonObject{{"id",7763},{"name","其他歌手"},{"aliases",QJsonArray{"G.E.M.邓紫棋"}}}})!=0)return 1;
    QJsonObject before{{"messageId","current"},{"conversationId","conversation"},{"catalogChoices",QJsonObject{
        {"turn","current"},{"conversation","conversation"},{"artists",QJsonArray{falseName,canonical}}}}};
    if(selectionEvidence(before).value("ordinal").toInt()!=2)return 1;
    auto stale=before;auto choices=stale.value("catalogChoices").toObject();choices.insert("turn","old");stale.insert("catalogChoices",choices);
    if(!selectionEvidence(stale).isEmpty())return 1;
    QJsonObject view{{"conversationBusy",true},{"conversation",QJsonObject{{"journalPending",true}}}};
    QElapsedTimer elapsed;elapsed.start();bool journal=true;QTimer::singleShot(1700,[&]{journal=false;});
    if(waitForProbe([&]{view.insert("conversationBusy",journal);return view;},6000)!="settled"||elapsed.elapsed()<3000)return 1;
    const QJsonObject needsPreview{{"requirePreview",true},{"minimumTracks",3},{"requireLockedTracks",3}};
    if(IdentityFollowupProbe::stepPreconditions(needsPreview,{}))return 1;
    qInfo("补充身份守卫通过：真实候选第二项、名称/ID/唯一性、当前轮绑定、journal迟到等待、锁定前提；无GUI/网络/模型。");
    return 0;
}
}

int main(int argc,char **argv) {
    umask(0077);
    if(argc==2&&QByteArray(argv[1])=="--check-identity-guards"){QCoreApplication application(argc,argv);return checkIdentityGuards();}
    QApplication application(argc,argv);application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName("Tango identity followup diagnostic");
    QCommandLineParser parser;parser.addHelpOption();parser.addOption({"output","新范围私有观察文件","path"});parser.process(application);
    const QFileInfo outputInfo(parser.value("output"));const auto output=outputInfo.absoluteFilePath(),run=outputInfo.absolutePath();
    if(parser.value("output").isEmpty()||!QDir::isAbsolutePath(parser.value("output"))||QFileInfo::exists(output)||
        QFileInfo(run).isSymLink()||QDir(run).canonicalPath()!=run||QFileInfo(run).ownerId()!=uint(getuid()))return 2;
    const auto manifest=privateJson(run+"/manifest.json"),authorization=privateJson(run+"/authorization.json");
    const auto connection=privateJson(run+"/connection.json"),netease=privateJson(run+"/netease-connection.json"),plan=privateJson(run+"/plan.json");
    const auto scope=QStringLiteral("music_identity_followup_v1"),statement=authorization.value("statement").toString();
    const QUrl backend(connection.value("backendURL").toString());
    if(manifest.value("scope")!=scope||authorization.value("scope")!=scope||authorization.value("bindingScopeOnly")!=true||
        authorization.value("newAuthorization")!=true||authorization.value("manifestSeal")!=manifest.value("seal")||
        !(statement.contains(QStringLiteral("身份澄清"))||statement.contains(scope))||authorization.value("maximumProviderRequests").toInt()<1||
        authorization.value("maximumProviderRequests").toInt()>40||plan.value("tasks").toArray().size()!=2||
        connection.value("manifestSeal")!=manifest.value("seal")||netease.value("manifestSeal")!=manifest.value("seal")||netease.value("anonymous")!=true||
        connection.value("dataDir").toString()!=run+"/qt-data"||backend.scheme()!="http"||backend.host()!="127.0.0.1"||backend.port()<1||
        !backend.path().isEmpty()||backend.hasQuery()||backend.hasFragment()||!backend.userInfo().isEmpty())return 2;
    QFile lock(run+"/probe-execution-lock.json");if(!lock.open(QIODevice::WriteOnly|QIODevice::NewOnly))return 2;
    lock.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);lock.write(QJsonDocument(QJsonObject{{"pid",double(getpid())},{"scope",scope},{"manifestSeal",manifest.value("seal")}}).toJson());lock.close();
    const auto environment=connection.value("qtEnvironment").toObject();
    for(const auto &key:QStringList{"MUSICPLAYER_BACKEND_URL","MUSICPLAYER_DEVELOPMENT_MODE","MUSICPLAYER_DEVELOPMENT_TOKEN_FILE","MUSICPLAYER_DATA_DIR","MUSICPLAYER_PROJECT_DIR"})
        qputenv(key.toUtf8().constData(),environment.value(key).toString().toUtf8());
    qputenv("MUSICPLAYER_NETEASE_PORT",QByteArray::number(netease.value("port").toInt()));qputenv("MUSICPLAYER_REDIS_PORT","invalid-identity-diagnostic");
    qputenv("LANGSMITH_TRACING","false");qputenv("LANGCHAIN_TRACING_V2","false");QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    QFile controlFile(connection.value("controlTokenFile").toString());if(!controlFile.open(QIODevice::ReadOnly))return 2;
    const auto token=controlFile.readAll().trimmed();if(token.size()!=64)return 2;QNetworkAccessManager network;network.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    const auto context=[&](const QString &id,int turn){auto url=backend;url.setPath("/__music_requirements__/context");QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");request.setRawHeader("X-Tango-Evaluation-Control",token);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
        auto *reply=network.post(request,QJsonDocument(QJsonObject{{"caseId",id},{"turn",turn}}).toJson(QJsonDocument::Compact));
        const bool ready=QTest::qWaitFor([&]{return reply->isFinished();},10000);
        const bool accepted=ready&&reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==200&&QJsonDocument::fromJson(reply->readAll()).object().value("accepted")==true;
        if(!ready)reply->abort();reply->deleteLater();return accepted;};
    QJsonObject report{{"schemaVersion",1},{"scope",scope},{"manifestSeal",manifest.value("seal")},{"observations",QJsonArray()},
        {"setupObservations",QJsonArray()},{"tasks",QJsonArray()},{"automaticRetry",false},{"modelJudge",false},{"overallAccuracy",QJsonValue(QJsonValue::Null)},
        {"originalScoreRewritten",false},{"catalogFixture",false}};
    if(!writeJson(output,report))return 2;
    QObject bootstrap;auto *account=AppBackendClient::instance();bool done=false;QString error;
    account->bootstrapDevelopmentSession(&bootstrap,[&](const QString &failure){error=failure;done=true;});
    if(!QTest::qWaitFor([&]{return done;},20000)||!error.isEmpty()||!account->authenticated())return 1;
    done=false;account->refreshAgentStatus(&bootstrap,[&](const QString &failure){error=failure;done=true;});
    if(!QTest::qWaitFor([&]{return done;},20000)||!error.isEmpty()||!account->agentReady()||!account->routingAvailable())return 1;
    NativeSession session;session.open();if(!session.conversation||!session.task||!session.dialog)return 1;
    QJsonArray observations,setups,tasks;
    const auto stop=[&](const QString &reason){QSet<QString> seen;for(const auto &value:observations){const auto row=value.toObject();seen.insert(row.value("caseId").toString()+":"+QString::number(row.value("turn").toInt()));}
        for(const auto &value:plan.value("tasks").toArray()){const auto task=value.toObject();int turn=0;for(const auto &input:task.value("inputs").toArray()){
            ++turn;const auto id=task.value("id").toString();if(!seen.contains(id+":"+QString::number(turn)))observations.append(QJsonObject{
                {"caseId",id},{"turn",turn},{"plannedInput",input},{"sent",false},{"waitStatus","blocked_run_stopped"},{"blockedReason",reason}});}}
        report.insert("observations",observations);report.insert("tasks",tasks);report.insert("stopped",true);report.insert("stopReason",reason);report.insert("finished",false);
        writeJson(output,redact(report).toObject());session.close();return 1;};
    const auto initial=identitySettle(session,20000);if(probeMustStop(initial))return stop("initial_"+initial);
    for(const auto &value:plan.value("tasks").toArray()){const auto definition=value.toObject();const auto id=definition.value("id").toString();
        if(!context(id,0))return stop("setup_context_rejected");const auto previous=session.conversation->snapshot().value("id").toString();
        const bool setupSent=session.send("新对话");const auto setupState=setupSent?identitySettle(session,30000):QString("input_not_accepted");
        const bool prepared=probePrepared(session.snapshot());setups.append(QJsonObject{{"caseId",id},{"sent",setupSent},{"waitStatus",setupState},{"prepared",prepared},{"after",redact(session.snapshot())}});
        report.insert("setupObservations",setups);writeJson(output,redact(report).toObject());if(!setupSent||setupState!="settled"||!prepared)return stop("setup_not_clean");
        QString withinCase,originalTask;int turn=0;
        for(const auto &input:definition.value("inputs").toArray()){++turn;const auto step=input.toObject();const auto before=session.snapshot();
            QJsonObject evidence;QString text=step.value("text").toString(),boundTurn;
            if(step.value("kind")=="select_verified_artist"){
                evidence=selectionEvidence(before);if(evidence.isEmpty())return stop("current_canonical_identity_missing_or_ambiguous");
                boundTurn=evidence.value("boundTurn").toString();text=QStringLiteral("选择歌手 %1").arg(evidence.value("ordinal").toInt());
            }
            if(!IdentityFollowupProbe::stepPreconditions(step,before))return stop("required_preview_or_locks_missing");
            if(!context(id,turn))return stop("turn_context_rejected");session.errors.clear();session.resolved={};session.failed={};QElapsedTimer elapsed;elapsed.start();
            const bool sent=session.send(text);const auto state=sent?identitySettle(session,180000,boundTurn):QString("input_not_accepted");
            auto row=session.snapshot();const auto current=row.value("conversationId").toString();const bool isolated=probeBusinessIsolated(previous,current,withinCase);
            if(isolated&&withinCase.isEmpty())withinCase=current;
            const auto currentTask=row.value("task").toObject().value("id").toString();if(originalTask.isEmpty()&&!currentTask.isEmpty())originalTask=currentTask;
            row.insert("caseId",id);row.insert("turn",turn);row.insert("text",text);row.insert("plannedInput",step);row.insert("before",before);row.insert("selectionEvidence",evidence);
            row.insert("sent",sent);row.insert("waitStatus",state);row.insert("elapsedMs",double(elapsed.elapsed()));row.insert("businessConversationIsolated",isolated);
            row.insert("businessStatus","unverified_until_evidence_review");observations.append(redact(row));report.insert("observations",observations);writeJson(output,redact(report).toObject());
            session.window->grab().save(run+QString("/%1-%2.png").arg(id).arg(turn));qInfo().noquote()<<id<<"turn"<<turn<<state;
            if(!sent||probeMustStop(state)||!isolated)return stop("incomplete_or_unknown_turn");
            if(!originalTask.isEmpty()&&!currentTask.isEmpty()&&originalTask!=currentTask)return stop("task_identity_changed");
            if(row.value("playback").toObject().value("queueCount").toInt()!=0||!row.value("preference").toObject().value("preference").toString().isEmpty())return stop("unexpected_playback_or_memory_write");
            if(step.value("kind")=="select_verified_artist"&&state=="needs_user")return stop("selection_did_not_complete_clarification");
        }
        tasks.append(QJsonObject{{"id",id},{"businessStatus","unverified_until_evidence_review"}});report.insert("tasks",tasks);writeJson(output,redact(report).toObject());
    }
    report.insert("finished",true);report.insert("finishedAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));writeJson(output,redact(report).toObject());session.close();return 0;
}
