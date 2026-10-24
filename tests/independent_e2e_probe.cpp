#include "appbackendclient.h"
#include "conversationclient.h"
#include "independent_catalog_fixture.h"
#include "mainwindow.h"
#include "musictaskclient.h"
#include "radiodialog.h"
#include "radiostore.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <functional>
#include <memory>

namespace {
bool writeJson(const QString &path,const QJsonObject &value) {
    QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))return false;
    file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    return file.write(QJsonDocument(value).toJson(QJsonDocument::Indented))>=0&&file.commit();
}
// 观测不包含身份凭据、播放地址或观测控制令牌；合成题面和曲库来源仍保留作判分证据。
QJsonValue redact(const QJsonValue &value) {
    if(value.isArray()){QJsonArray out;for(const auto &row:value.toArray())out.append(redact(row));return out;}
    if(!value.isObject())return value;
    QJsonObject out;
    const auto source=value.toObject();
    for(auto it=source.constBegin();it!=source.constEnd();++it){const auto key=it.key().toLower();
        if(key.contains("token")||key.contains("password")||key.contains("cookie")||key.contains("authorization")||
            key=="apikey"||key=="api_key"||key=="url"||key=="mediaurl"||key=="streamurl"||key=="baseurl")continue;
        out.insert(it.key(),redact(it.value()));
    }
    return out;
}
QJsonArray readRoutes(const QString &profile,const QString &device,const QString &conversation) {
    QJsonArray out;QSqlQuery query(QSqlDatabase::database());
    // 不能调用 RadioStore.routes()：恢复API会把dispatched改成unknown；评测观测必须只读。
    query.prepare("SELECT turn_id,plan_json,actions_json,receipts_json,base_revision,confirmation_turn FROM radio_route_journal WHERE profile=? AND device_id=? AND conversation_id=? ORDER BY created_at,turn_id");
    query.addBindValue(profile);query.addBindValue(device);query.addBindValue(conversation);
    if(query.exec())while(query.next())out.append(QJsonObject{{"turnId",query.value(0).toString()},
        {"plan",QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object()},
        {"actions",QJsonDocument::fromJson(query.value(2).toString().toUtf8()).object()},
        {"receipts",QJsonDocument::fromJson(query.value(3).toString().toUtf8()).object()},
        {"baseRevision",query.value(4).toDouble()},{"confirmationTurn",query.value(5).toString()}});
    return out;
}
QJsonObject readPreference(const QString &profile) {
    QJsonObject out{{"preference",""},{"revision",0},{"items",QJsonArray()}};QSqlQuery query(QSqlDatabase::database());
    query.prepare("SELECT preference FROM radio_preferences WHERE profile=?");query.addBindValue(profile);
    if(query.exec()&&query.next())out.insert("preference",query.value(0).toString());
    query.prepare("SELECT revision FROM radio_preference_versions WHERE profile=?");query.addBindValue(profile);
    if(query.exec()&&query.next())out.insert("revision",query.value(0).toDouble());
    query.prepare("SELECT id,value,scope,source,confirmed_at,revision FROM radio_preference_items WHERE profile=? ORDER BY position");query.addBindValue(profile);
    QJsonArray items;if(query.exec())while(query.next())items.append(QJsonObject{{"id",query.value(0).toString()},{"value",query.value(1).toString()},
        {"scope",query.value(2).toString()},{"source",query.value(3).toString()},{"confirmedAt",query.value(4).toString()},{"revision",query.value(5).toDouble()}});
    out.insert("items",items);return out;
}
QString latestUser(const QJsonObject &snapshot) {
    const auto messages=snapshot.value("messages").toArray();for(int index=messages.size()-1;index>=0;--index){const auto row=messages.at(index).toObject();if(row.value("role")=="user")return row.value("id").toString();}return {};
}
struct Control {
    QNetworkAccessManager network;QUrl base;QByteArray token;
    Control(const QUrl &url,const QByteArray &secret):base(url),token(secret){network.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));}
    QJsonObject post(const QString &path,const QJsonObject &body) {
        QUrl url=base;url.setPath(path);QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");request.setRawHeader("X-Tango-Evaluation-Control",token);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
        auto *reply=network.post(request,QJsonDocument(body).toJson(QJsonDocument::Compact));
        if(!QTest::qWaitFor([&]{return reply->isFinished();},10000)){reply->abort();reply->deleteLater();return {{"controlError","timeout"}};}
        auto result=QJsonDocument::fromJson(reply->readAll()).object();
        const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();if(status<200||status>=300)result.insert("controlError",QString("HTTP %1").arg(status));reply->deleteLater();return result;
    }
};
struct NativeSession {
    std::unique_ptr<MainWindow> window;
    QStringList errors;QJsonArray resolved,failed;
    ConversationClient *conversation=nullptr;MusicTaskClient *task=nullptr;RadioDialog *dialog=nullptr;
    QJsonObject capturedProposal;
    void open() {
        window=std::make_unique<MainWindow>();window->show();window->p_PlayerObject->setMuted(true);
        if(AppBackendClient::instance()->authenticated())window->updateUsername(AppBackendClient::instance()->username());
        QMetaObject::invokeMethod(window.get(),"openRadio",Qt::DirectConnection);
        dialog=window->findChild<RadioDialog *>();conversation=window->findChild<ConversationClient *>();task=window->findChild<MusicTaskClient *>();
        if(task)QObject::connect(task,&MusicTaskClient::error,window.get(),[this](const QString &error){errors.append(error);});
        if(conversation)QObject::connect(conversation,&ConversationClient::error,window.get(),[this](const QString &error){errors.append(error);});
        QObject::connect(window.get(),&MainWindow::onlinePlaybackResolved,window.get(),[this](qint64 id,bool trial){resolved.append(QJsonObject{{"id",double(id)},{"trial",trial}});});
        QObject::connect(window.get(),&MainWindow::onlinePlaybackFailed,window.get(),[this](qint64 id,const QString &reason){failed.append(QJsonObject{{"id",double(id)},{"reason",reason}});});
    }
    void close() { window.reset();dialog=nullptr;conversation=nullptr;task=nullptr;
        if(QSqlDatabase::contains())QSqlDatabase::database(QSqlDatabase::defaultConnection,false).close();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); }
    QJsonObject pendingMemory() const {
        QJsonObject out{{"present",false},{"operation",""},{"text",""},{"canConfirm",false},{"canCancel",false}};if(!dialog)return out;
        auto *confirm=dialog->findChild<QPushButton *>("button_RadioConfirmPreference");auto *cancel=dialog->findChild<QPushButton *>("button_RadioCancelPreference");
        auto *edit=dialog->findChild<QLineEdit *>("edit_RadioPreferenceProposal");auto *label=dialog->findChild<QLabel *>("label_RadioPreferenceProposal");
        const bool active=confirm&&confirm->isEnabled();out.insert("present",active);out.insert("canConfirm",active);out.insert("canCancel",cancel&&cancel->isEnabled());
        if(active){out.insert("text",edit?edit->text():QString());out.insert("operation",label&&label->text().contains("清除")?"clear":"set");}return out;
    }
    void captureProposal() {
        if(!capturedProposal.isEmpty()||!window||!conversation)return;
        for(const auto &value:readRoutes(window->accountProfile(),conversation->deviceId(),conversation->snapshot().value("id").toString())){
            const auto route=value.toObject();const auto states=route.value("actions").toObject();
            for(const auto &entry:route.value("plan").toObject().value("actions").toArray()){
                const auto action=entry.toObject();const auto id=action.value("id").toString();
                if(action.value("capability")=="memory.propose"&&states.value(id)=="awaiting_confirmation"){
                    const auto parameters=action.value("parameters").toObject();capturedProposal={{"actionId",route.value("turnId").toString()+":"+id},
                        {"operation",parameters.value("operation")},{"preference",parameters.value("preference")}};return;
                }
            }
        }
    }
    bool confirmationAllowed(const QJsonObject &options) const {
        const auto pending=pendingMemory();if(!pending.value("present").toBool())return false;
        const auto expected=options.value("expectedOperation").toString();if(!expected.isEmpty()&&pending.value("operation")!=expected)return false;
        const auto normalize=[](const QString &text){return text.normalized(QString::NormalizationForm_KC).toCaseFolded();};
        for(const auto &value:options.value("confirmGuard").toObject().value("containsValues").toArray())
            if(!value.isString()||!normalize(pending.value("text").toString()).contains(normalize(value.toString())))return false;
        return true;
    }
    QJsonObject clientEvent(const QString &name,const QJsonObject &options={}) {
        QJsonObject event{{"action",name},{"ok",false}};if(!dialog)return event;
        if(name=="confirm_preference") {
            auto *confirm=dialog->findChild<QPushButton *>("button_RadioConfirmPreference");
            if(!confirm||!confirm->isEnabled()||!confirmationAllowed(options)){event.insert("error","没有满足明确核对条件的待确认提议，未点击");event.insert("errorCode","confirmation_precondition_not_met");return event;}
            captureProposal();confirm->click();event.insert("ok",true);
        } else if(name=="replay_preference_confirmation") {
            if(capturedProposal.isEmpty()){event.insert("error","没有捕获过真实待确认行动，未伪造actionId");return event;}
            const auto key=capturedProposal.value("actionId").toString();const auto turn=key.section(':',0,-2),actionId=key.section(':',-1);QString originalState;
            for(const auto &value:readRoutes(window->accountProfile(),conversation->deviceId(),conversation->snapshot().value("id").toString())){
                const auto row=value.toObject();if(row.value("turnId")==turn)originalState=row.value("actions").toObject().value(actionId).toString();
            }
            event.insert("observedOriginalState",originalState);
            if(originalState!="cancelled"){event.insert("errorCode","replay_precondition_not_met");event.insert("error","原提议尚未核实取消，未重放仍可能有效的授权");return event;}
            // 仅按冻结用例的显式本机事件重放旧信号，验证产品拒绝失效授权；不生成新路由或确认原话。
            const bool invoked=QMetaObject::invokeMethod(dialog,"preferenceProposalDecided",Qt::DirectConnection,
                Q_ARG(QString,capturedProposal.value("actionId").toString()),Q_ARG(bool,true),
                Q_ARG(QString,capturedProposal.value("preference").toString()));
            event.insert("ok",invoked);event.insert("capturedActionId",capturedProposal.value("actionId"));
        } else event.insert("error","未支持的明确本机事件");
        if(event.value("ok").toBool())event.insert("waitStatus",settle(10000));
        return event;
    }
    QJsonObject snapshot() const {
        const auto cs=conversation?conversation->snapshot():QJsonObject();const auto ts=task?task->snapshot():QJsonObject();
        QJsonArray recommendations;if(dialog)for(const auto &row:dialog->recommendations())recommendations.append(QJsonObject{{"id",double(row.song.id)},
            {"title",row.song.title},{"artist",row.song.artist},{"album",row.song.album},{"version",row.song.version.isEmpty()?QStringLiteral("unknown"):row.song.version},{"durationMs",row.song.durationMs}});
        QJsonArray queueIds;if(window)for(int index=0;index<window->p_PlayerList->mediaCount();++index){const auto url=window->p_PlayerList->media(index).request().url();const auto id=QUrlQuery(url).queryItemValue("id");queueIds.append(id.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(double(id.toLongLong())));}
        auto *transcript=dialog?dialog->findChild<QPlainTextEdit *>("text_RadioConversation"):nullptr;
        const QString profile=window?window->accountProfile():QString();
        return {{"conversationId",cs.value("id")},{"messageId",latestUser(cs)},{"conversation",cs},{"task",ts},
            {"preference",readPreference(profile)},{"routes",readRoutes(profile,conversation?conversation->deviceId():QString(),cs.value("id").toString())},
            {"pendingMemory",pendingMemory()},{"recommendations",recommendations},
            {"lookupContext",window?window->property("tangoLookupContext").toJsonObject():QJsonObject()},
            {"playback",QJsonObject{{"state",window?int(window->p_PlayerObject->state()):0},{"queueCount",window?window->p_PlayerList->mediaCount():0},
                {"currentIndex",window?window->p_PlayerList->currentIndex():-1},{"queueIds",queueIds},{"resolvedIds",resolved},{"failedIds",failed}}},
            {"transcript",transcript?transcript->toPlainText():QString()},{"errors",QJsonArray::fromStringList(errors)}};
    }
    bool send(const QString &text) {
        if(!dialog||!conversation||!task)return false;
        auto *input=dialog->findChild<QPlainTextEdit *>("edit_RadioInstruction");auto *generate=dialog->findChild<QPushButton *>("button_RadioGenerate");
        if(!input||!generate)return false;
        // 发送按钮只在有草稿时启用；必须先模拟用户输入，再等待恢复及权限检查允许发送。
        input->setPlainText(text);
        if(!QTest::qWaitFor([&]{return !conversation->busy()&&!task->busy()&&generate->isEnabled();},10000)){
            errors.append("原生评测草稿已填入，但界面仍不可发送；未绕过控件或权限检查");return false;
        }
        QTest::keyClick(input,Qt::Key_Return);
        return QTest::qWaitFor([&]{return input->toPlainText().isEmpty();},10000);
    }
    QString settle(int timeoutMs) const {
        QElapsedTimer deadline,stable;deadline.start();stable.start();QByteArray previous;
        while(deadline.elapsed()<timeoutMs){QTest::qWait(100);if(!conversation||!task||!dialog)return "missing_client";
            const auto state=task->snapshot();const QString status=state.value("status").toString();
            const bool running=QStringList{"planning","researching","composing","waiting_tools"}.contains(status)&&!state.value("clientObservationOnly").toBool();
            const auto signature=QJsonDocument(QJsonObject{{"conversation",conversation->snapshot()},{"task",state},{"pendingMemory",pendingMemory()}}).toJson(QJsonDocument::Compact);
            if(signature!=previous){previous=signature;stable.restart();}
            if(!conversation->busy()&&!task->busy()&&!running&&stable.elapsed()>=1400)return "settled";
        }
        return "timeout";
    }
    QString waitForStatus(const QString &status,int timeoutMs) const {
        if(status.isEmpty())return settle(timeoutMs);
        const bool reached=QTest::qWaitFor([&]{return task&&conversation&&!conversation->busy()&&task->snapshot().value("status")==status&&
            (status!="waiting_tools"||!task->snapshot().value("pendingSteps").toArray().isEmpty());},timeoutMs);
        return reached?"reached_"+status:"timeout";
    }
};
}

int main(int argc,char **argv) {
    bool truthOnly=false;for(int index=1;index<argc;++index)truthOnly=truthOnly||QByteArray(argv[index])=="--truth-only";
    std::unique_ptr<QCoreApplication> application;
    if(truthOnly)application=std::make_unique<QCoreApplication>(argc,argv);
    else {auto *gui=new QApplication(argc,argv);gui->setQuitOnLastWindowClosed(false);application.reset(gui);}
    QCoreApplication::setApplicationName("Tango independent E2E probe");
    QCommandLineParser parser;parser.addHelpOption();parser.addOption({"plan","私有输入计划JSON","path"});parser.addOption({"backend-url","隔离评测回环后台","url"});
    parser.addOption({"output","逐轮观察JSON","path"});parser.addOption({"catalog-truth","固定合成曲库完整元数据JSON","path"});parser.addOption({"truth-only","只导出冻结曲库真值，不启动HTTP或产品客户端"});parser.process(*application);
    const QFileInfo planPath(parser.value("plan"));
    if(parser.value("plan").isEmpty()||!planPath.isFile()||planPath.isSymLink()||planPath.size()>8*1024*1024){qCritical("评测计划路径无效，须为有限大小的普通文件");return 2;}
    QFile inputFile(planPath.absoluteFilePath());if(!inputFile.open(QIODevice::ReadOnly)){qCritical("无法读取评测计划");return 2;}
    QJsonParseError parseError;const auto plan=QJsonDocument::fromJson(inputFile.readAll(),&parseError).object();inputFile.close();
    const auto frozen=QDateTime::fromString(plan.value("catalogAnchorUtc").toString(),Qt::ISODateWithMs);
    if(parseError.error!=QJsonParseError::NoError||!frozen.isValid()){qCritical("评测计划或冻结曲库时间无效");return 2;}
    if(truthOnly){
        const QFileInfo output(parser.value("catalog-truth"));
        if(parser.isSet("backend-url")||parser.isSet("output")||parser.value("catalog-truth").isEmpty()||output.isSymLink()||output.isDir()||!output.dir().exists()||
            output.absoluteFilePath()==planPath.absoluteFilePath()){qCritical("只导出真值须指定独立普通输出路径，不能传后台或观察输出参数");return 2;}
        // 纯数据模式不监听端口、不实例化产品客户端，也不读取本机账号、配置或授权。
        IndependentCatalogFixture catalog(frozen,false);catalog.loadDefinitions(plan.value("fixtureDefinitions").toObject());
        return writeJson(output.absoluteFilePath(),catalog.truth())?0:2;
    }
    const QUrl backend(parser.value("backend-url"));
    if(parseError.error!=QJsonParseError::NoError||plan.value("cases").toArray().isEmpty()||
        backend.scheme()!="http"||backend.host()!="127.0.0.1"||backend.port()<1||!backend.userInfo().isEmpty()||!backend.path().isEmpty()||backend.hasQuery()||backend.hasFragment()||
        plan.value("controlToken").toString().isEmpty()||parser.value("output").isEmpty()) {qCritical("评测参数无效：须使用隔离回环后台、控制令牌、题集和输出路径");return 2;}
    QTemporaryDir temporary; if(!temporary.isValid())return 2;
    IndependentCatalogFixture catalog(frozen);if(!catalog.server.isListening())return 2;
    catalog.loadDefinitions(plan.value("fixtureDefinitions").toObject());
    if(!parser.value("catalog-truth").isEmpty()&&!writeJson(parser.value("catalog-truth"),catalog.truth()))return 2;
    qputenv("MUSICPLAYER_BACKEND_URL",backend.toString().toUtf8());qputenv("MUSICPLAYER_DEVELOPMENT_MODE","");qputenv("MUSICPLAYER_DEVELOPMENT_TOKEN_FILE","");
    qputenv("MUSICPLAYER_NETEASE_PORT",QByteArray::number(catalog.server.serverPort()));qputenv("MUSICPLAYER_REDIS_PORT","invalid-independent-fixture");
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    Control control(backend,plan.value("controlToken").toString().toUtf8());
    const auto firstCase=plan.value("cases").toArray().first().toObject();
    const auto handshake=control.post("/__evaluation__/context",{{"caseId",firstCase.value("id")},{"turn",1}});
    // 写账号前核验拥有者控制端点，防止误把正常本机后台当成隔离评测进程。
    if(handshake.value("accepted")!=true){qCritical("隔离评测后台控制端点核验失败，未创建账号或发送用户消息");return 2;}
    QJsonObject report{{"schemaVersion",1},{"catalog",catalog.description()},{"modelCallsExecutedByProbe",0},{"observations",QJsonArray()},{"setupObservations",QJsonArray()},{"cases",QJsonArray()},
        {"recoveryScope","销毁窗口并关闭本机SQLite连接后重建；未杀死整个Qt或后台进程"},
        {"unsupportedFixtures",QJsonArray{"unknown_original_request"}},
        {"conditionalFixtures",QJsonArray{"controllable_running_task","history_snapshot","pending_candidate","account_A_private_data"}},
        {"productCapabilityGaps",QJsonArray{"artist_biography_tool"}}};
    QJsonArray observations,caseResults,setupObservations;
    const QString password="independent-owned-fixture-password-2026";
    auto *account=AppBackendClient::instance();
    int caseIndex=0;
    for(const auto &caseValue:plan.value("cases").toArray()) {
        const auto item=caseValue.toObject();const QString caseId=item.value("id").toString();++caseIndex;
        if(caseId.isEmpty()){qCritical("题目缺少id");return 2;}
        QJsonArray missingFixtures;
        const QSet<QString> fixedFixtures{"account_A_device_A","catalog_core","artist_identity_and_releases","artist_history"};
        for(const auto &required:item.value("requiresFixtures").toArray()){
            const QString name=required.toString();bool present=fixedFixtures.contains(name);
            if(name=="catalog_core"||name=="artist_identity_and_releases"||name=="artist_history")present=!plan.value("fixtureDefinitions").toObject().value(name).toObject().isEmpty();
            if(name=="confirmed_preferences")present=item.contains("initialPreference")&&!item.value("initialPreference").toString().isEmpty();
            if(QStringList{"pending_candidate","history_snapshot"}.contains(name))present=!item.value("setupInstructions").toArray().isEmpty();
            if(name=="controllable_running_task")present=item.value("catalogDelayMs").toInt()>0&&!item.value("setupInstructions").toArray().isEmpty();
            if(name=="account_A_private_data")present=!item.value("accountAPrivatePreference").toString().isEmpty();
            if(!present)missingFixtures.append(required);
        }
        // 缺少前置状态属于评测搭建受阻，不能发给模型后计为模型失败或悄悄移出分母。
        if(!missingFixtures.isEmpty()){
            caseResults.append(QJsonObject{{"id",caseId},{"setupOk",false},{"error","缺少明确要求的隔离夹具"},{"blockedFixtures",missingFixtures}});
            int blockedTurn=0;for(const auto &turnValue:item.value("turns").toArray()){
                auto observed=turnValue.toObject();observed.insert("caseId",caseId);observed.insert("turn",++blockedTurn);observed.insert("ok",false);
                observed.insert("waitStatus","blocked_missing_fixture");observed.insert("missingFixtures",missingFixtures);observations.append(redact(observed));
            }
            report.insert("observations",observations);report.insert("cases",caseResults);if(!writeJson(parser.value("output"),redact(report).toObject()))return 2;
            continue;
        }
        const QString directory=temporary.filePath(QString::number(caseIndex));QDir().mkpath(directory+"/netease");
        qputenv("MUSICPLAYER_DATA_DIR",directory.toUtf8());QFile token(directory+"/netease/token");if(!token.open(QIODevice::WriteOnly))return 2;token.write(QByteArray(64,'f'));token.close();
        catalog.delayMs=qBound(0,item.value("catalogDelayMs").toInt(),10000);
        catalog.untrustedToolDescription=item.value("untrustedToolDescription").toString();
        NativeSession session;bool done=false;QString error;
        const auto registerOwned=[&]() {
            session.open();done=false;error.clear();const QString username="eval-"+QUuid::createUuid().toString(QUuid::WithoutBraces).left(16);
            account->registerUser(username,password,session.window.get(),[&](const QString &failure){error=failure;done=true;});
            bool ready=QTest::qWaitFor([&]{return done;},15000)&&error.isEmpty();
            if(ready){done=false;account->refreshAgentStatus(session.window.get(),[&](const QString &failure){error=failure;done=true;});ready=QTest::qWaitFor([&]{return done;},10000)&&error.isEmpty()&&account->routingAvailable()&&account->taskRuntimeAvailable();}
            if(ready)QMetaObject::invokeMethod(session.window.get(),"openRadio",Qt::DirectConnection);return ready;
        };
        bool setup=registerOwned();QJsonObject privateAccountEvidence;
        if(setup&&!item.value("accountAPrivatePreference").toString().isEmpty()){
            const QString profileA=session.window->accountProfile();QString storageError;
            setup=RadioStore(QSqlDatabase::database()).save(profileA,item.value("accountAPrivatePreference").toString(),&storageError);
            if(setup)privateAccountEvidence.insert("accountAPreference",readPreference(profileA));else error=storageError;
            session.close();QObject logoutA;done=false;account->logout(&logoutA,[&](const QString &){done=true;});
            if(!QTest::qWaitFor([&]{return done;},5000))setup=false;
            if(setup){setup=registerOwned();session.capturedProposal={};if(setup){privateAccountEvidence.insert("accountBInitialPreference",readPreference(session.window->accountProfile()));privateAccountEvidence.insert("differentAccountScope",profileA!=session.window->accountProfile());}}
        }
        if(setup&&item.contains("initialPreference")){QString storageError;setup=RadioStore(QSqlDatabase::database()).save(session.window->accountProfile(),item.value("initialPreference").toString(),&storageError);if(!setup)error=storageError;}
        if(setup)QMetaObject::invokeMethod(session.window.get(),"openRadio",Qt::DirectConnection);
        if(setup&&!item.value("setupInstructions").toArray().isEmpty()){
            const auto context=control.post("/__evaluation__/context",{{"caseId",caseId},{"turn",1},{"phase","setup"}});
            if(context.contains("controlError")){setup=false;error="前置指令控制上下文不可用";}
            int setupIndex=0;
            for(const auto &setupValue:item.value("setupInstructions").toArray()){
                ++setupIndex;const auto instruction=setupValue.isString()?QJsonObject{{"text",setupValue}}:setupValue.toObject();
                const int setupCatalogStart=catalog.requests.size();const auto before=session.snapshot();const bool sent=setup&&session.send(instruction.value("text").toString());
                const QString waited=sent?session.waitForStatus(instruction.value("waitFor").toString(),qBound(1000,instruction.value("timeoutMs").toInt(180000),180000)):"setup_input_not_accepted";
                session.captureProposal();auto observed=session.snapshot();observed.insert("caseId",caseId);observed.insert("phase","setup");observed.insert("setupIndex",setupIndex);
                observed.insert("text",instruction.value("text"));observed.insert("before",before);observed.insert("waitStatus",waited);
                const bool completed=sent&&(waited=="settled"||waited.startsWith("reached_"));observed.insert("ok",completed);
                QJsonArray queries;for(int index=setupCatalogStart;index<catalog.requests.size();++index)queries.append(catalog.requests.at(index));observed.insert("catalogRequests",queries);
                observed.insert("backend",control.post("/__evaluation__/observation",{{"caseId",caseId},{"turn",1},{"phase","setup"}}));setupObservations.append(redact(observed));
                if(!completed){setup=false;error="前置指令未到达冻结要求的状态";}
                report.insert("setupObservations",setupObservations);report.insert("observations",observations);report.insert("cases",caseResults);if(!writeJson(parser.value("output"),redact(report).toObject()))return 2;
            }
            for(const auto &required:item.value("requiresFixtures").toArray()){
                if(required=="pending_candidate"&&session.task->snapshot().value("status")!="preview"){setup=false;error="前置步骤没有建立可核验preview";}
                if(required=="controllable_running_task"&&(session.task->snapshot().value("status")!="waiting_tools"||session.task->snapshot().value("pendingSteps").toArray().isEmpty())){setup=false;error="前置步骤没有建立真实pendingSteps";}
            }
        }
        QJsonObject fixtureEvidence{{"catalogDelayMs",catalog.delayMs}};
        if(!catalog.untrustedToolDescription.isEmpty())fixtureEvidence.insert("untrustedToolInjection",QJsonObject{{"trackId",70001},{"field","song.description"},{"value",catalog.untrustedToolDescription},{"endpoint","/search"}});
        caseResults.append(QJsonObject{{"id",caseId},{"setupOk",setup},{"error",error},{"privateAccountEvidence",privateAccountEvidence},{"fixtureEvidence",fixtureEvidence}});
        int turn=0;bool prerequisiteFailed=!setup;
        for(const auto &turnValue:item.value("turns").toArray()) {
            const auto step=turnValue.toObject();++turn;QElapsedTimer timer;timer.start();
            const int firstCatalog=catalog.requests.size();session.errors.clear();session.resolved={};session.failed={};
            const auto before=session.snapshot();
            const auto context=control.post("/__evaluation__/context",{{"caseId",caseId},{"turn",turn},{"phase","evaluation"}});
            const QString action=step.value("clientAction").toString();QString text=step.value("text").toString();bool sent=false;QString waitStatus;
            const bool localReopen=action=="reopen"&&text.isEmpty();
            if(prerequisiteFailed)waitStatus="not_executed_prerequisite_failed";
            else if(context.contains("controlError"))waitStatus="control_context_failed";
            else if(localReopen) {session.close();session.open();sent=true;waitStatus=session.settle(15000);}
            else if(action=="confirm_preference"&&text.isEmpty()&&!session.confirmationAllowed(step))waitStatus="confirmation_precondition_not_met";
            else {
                if(text.isEmpty()) {
                    if(action=="confirm_preference")text=session.pendingMemory().value("operation")=="clear"?"确认清除口味":"确认保存口味";
                    else if(action=="cancel_preference")text="取消口味";
                    else if(action=="lock_first_three")text="保留前三首";
                    else if(action=="adopt")text="采用歌单";
                    else if(action=="resume")text="继续任务";
                    else if(action=="continue_pending")text="继续未完成步骤";
                    else if(action=="undo")text="撤回修改";
                    else if(action=="new_conversation")text="新对话";
                    else if(action=="choose_artist_1")text="选择歌手1";
                }
                sent=!text.isEmpty()&&session.send(text);waitStatus=sent?session.settle(qBound(1000,step.value("timeoutMs").toInt(180000),180000)):"input_not_accepted";
            }
            session.captureProposal();const auto afterInput=session.snapshot();QJsonArray clientEvents;bool eventsOk=true;
            if(sent&&waitStatus=="settled")for(const auto &eventValue:step.value("afterActions").toArray()){
                const auto options=eventValue.isObject()?eventValue.toObject():QJsonObject();const auto name=eventValue.isString()?eventValue.toString():options.value("action").toString();
                auto event=session.clientEvent(name,options);event.insert("after",session.snapshot());eventsOk=eventsOk&&event.value("ok").toBool()&&event.value("waitStatus")=="settled";clientEvents.append(redact(event));
            }
            auto observed=session.snapshot();observed.insert("caseId",caseId);observed.insert("turn",turn);observed.insert("text",text);observed.insert("afterInput",afterInput);observed.insert("clientEvents",clientEvents);
            observed.insert("before",before);
            if(!action.isEmpty())observed.insert("clientAction",action);
            observed.insert("ok",sent&&waitStatus=="settled"&&eventsOk);observed.insert("waitStatus",waitStatus);observed.insert("elapsedMs",double(timer.elapsed()));
            QJsonArray queries;for(int index=firstCatalog;index<catalog.requests.size();++index)queries.append(catalog.requests.at(index));observed.insert("catalogRequests",queries);
            QJsonObject observationRequest{{"caseId",caseId},{"turn",turn}};
            if(!observed.value("conversationId").toString().isEmpty())observationRequest.insert("conversationId",observed.value("conversationId"));
            if(sent&&!localReopen&&!observed.value("messageId").toString().isEmpty())observationRequest.insert("messageId",observed.value("messageId"));
            observed.insert("backend",control.post("/__evaluation__/observation",observationRequest));
            observations.append(redact(observed));report.insert("observations",observations);report.insert("cases",caseResults);
            if(!writeJson(parser.value("output"),redact(report).toObject()))return 2;
            qInfo().noquote()<<caseId<<"turn"<<turn<<waitStatus;
            if(!sent||waitStatus!="settled"||!eventsOk)prerequisiteFailed=true;
        }
        session.close();QObject logoutContext;done=false;account->logout(&logoutContext,[&](const QString &){done=true;});const bool loggedOut=QTest::qWaitFor([&]{return done;},5000);if(!loggedOut)qWarning("隔离账号退出请求未收到回执，本地会话已清除");
    }
    report.insert("observations",observations);report.insert("cases",caseResults);report.insert("catalogRequests",catalog.requests);
    return writeJson(parser.value("output"),redact(report).toObject())?0:2;
}
