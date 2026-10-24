#include "appbackendclient.h"
#include "conversationclient.h"
#include "mainwindow.h"
#include "musictaskclient.h"
#include "radiodialog.h"
#include "radiostore.h"
#include "routing_fixtures.h"
#include <QApplication>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QTimer>
#include <QTableView>
#include "songlistdelegate.h"
#include <sys/stat.h>
#include <QUrlQuery>
#include <QCryptographicHash>
#include <unistd.h>
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
// 只观测本轮拥有者数据；凭据、播放地址和能力令牌不得写入证据。
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
QJsonArray readRoutes(const QString &profile,const QString &device,const QString &conversation,bool *known=nullptr) {
    if(known)*known=true;
    if(profile.isEmpty()||device.isEmpty()||conversation.isEmpty())return {};
    QJsonArray out;QSqlQuery query(QSqlDatabase::database());
    // 不能调用 RadioStore.routes()：恢复API会把dispatched改成unknown；评测观测必须只读。
    query.prepare("SELECT turn_id,plan_json,actions_json,receipts_json,base_revision,confirmation_turn FROM radio_route_journal WHERE profile=? AND device_id=? AND conversation_id=? ORDER BY created_at,turn_id");
    query.addBindValue(profile);query.addBindValue(device);query.addBindValue(conversation);
    if(!query.exec()){if(known)*known=false;return {};}
    while(query.next())out.append(QJsonObject{{"turnId",query.value(0).toString()},
        {"plan",QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object()},
        {"actions",QJsonDocument::fromJson(query.value(2).toString().toUtf8()).object()},
        {"receipts",QJsonDocument::fromJson(query.value(3).toString().toUtf8()).object()},
        {"baseRevision",query.value(4).toDouble()},{"confirmationTurn",query.value(5).toString()}});
    return out;
}
QString probeActivity(const QJsonObject &view) {
    // 只检查当前轮路由；旧会话或旧历史 pending 不会拖住本题。只读查询失败不能冒充空闲。
    if(!view.value("routesKnown").toBool(true))return "unknown";
    bool running=view.value("conversationBusy").toBool()||view.value("taskBusy").toBool(),needsUser=false;
    const auto task=view.value("task").toObject();const auto status=task.value("status").toString();
    running|=QStringList{"planning","researching","composing","waiting_tools"}.contains(status);
    needsUser|=QStringList{"needs_clarification","awaiting_clarification","awaiting_confirmation"}.contains(status);
    const auto latest=view.value("latestMessage").toString();const auto choice=view.value("catalogChoices").toObject();
    for(const auto &value:view.value("routes").toArray()){
        const auto route=value.toObject();if(route.value("turnId").toString()!=latest)continue;
        const auto states=route.value("actions").toObject();
        for(auto it=states.constBegin();it!=states.constEnd();++it){
            const auto action=it.value().toString();
            const bool artistChoice=choice.value("turn").toString()==latest&&choice.value("action").toString()==it.key()&&
                !choice.value("artists").toArray().isEmpty();
            if(action=="unknown")return "unknown";
            if(action=="pending"||(action=="dispatched"&&!artistChoice))running=true;
            if(action=="awaiting_confirmation"||(action=="dispatched"&&artistChoice))needsUser=true;
        }
    }
    return running?"running":needsUser?"needs_user":"idle";
}
QString waitForProbe(const std::function<QJsonObject()> &observe,int timeoutMs) {
    QElapsedTimer deadline,stable;deadline.start();stable.start();QByteArray previous;
    while(deadline.elapsed()<timeoutMs){QTest::qWait(50);const auto view=observe();
        const auto signature=QJsonDocument(view).toJson(QJsonDocument::Compact);
        if(signature!=previous){previous=signature;stable.restart();}
        const auto activity=probeActivity(view);
        if(activity!="running"&&stable.elapsed()>=1400)return activity=="idle"?"settled":activity;
    }
    return "timeout";
}
bool probeMustStop(const QString &state,bool isolated=true) {
    return !isolated||!QStringList{"settled","needs_user","blocked_needs_user"}.contains(state);
}
bool probePrepared(const QJsonObject &view) {
    // 正式新对话是懒创建：此时应为空态，首条业务消息才创建新会话。
    const auto conversation=view.value("conversation").toObject();
    return conversation.value("id").toString().isEmpty()&&conversation.value("messages").toArray().isEmpty()&&
        !conversation.value("journalPending").toBool()&&!conversation.value("clearPending").toBool()&&!conversation.value("clearing").toBool()&&
        view.value("task").toObject().isEmpty()&&view.value("recommendations").toArray().isEmpty()&&
        view.value("lookupContext").toObject().isEmpty()&&view.value("routes").toArray().isEmpty()&&
        view.value("catalogChoices").toObject().isEmpty()&&
        !view.value("pendingMemory").toObject().value("present").toBool()&&
        view.value("playlistCandidates").toArray().isEmpty();
}
bool probeBusinessIsolated(const QString &previous,const QString &current,const QString &withinCase) {
    return !current.isEmpty()&&(previous.isEmpty()||current!=previous)&&(withinCase.isEmpty()||current==withinCase);
}
int checkWaitGuards() {
    // 本模式只用内存库和事件循环验证等待逻辑，不创建窗口、HTTP、账号或模型。
    auto database=QSqlDatabase::addDatabase("QSQLITE");database.setDatabaseName(":memory:");if(!database.open())return 1;
    QSqlQuery query;
    if(!query.exec("CREATE TABLE radio_route_journal(profile TEXT,device_id TEXT,conversation_id TEXT,turn_id TEXT,plan_json TEXT,actions_json TEXT,receipts_json TEXT,base_revision INTEGER,confirmation_turn TEXT,created_at INTEGER)"))return 1;
    if(!query.exec("INSERT INTO radio_route_journal VALUES('owned','device','conversation','current','{}','{\"lookup\":\"dispatched\"}','{}',0,'',1)"))return 1;
    if(!query.exec("INSERT INTO radio_route_journal VALUES('owned','device','conversation','old','{}','{\"lookup\":\"pending\"}','{}',0,'',0)"))return 1;
    const auto observe=[] {bool known=false;const auto rows=readRoutes("owned","device","conversation",&known);
        return QJsonObject{{"routesKnown",known},{"latestMessage","current"},{"routes",rows},{"task",QJsonObject()}};};
    auto view=observe();if(probeActivity(view)!="running")return 1;
    QElapsedTimer elapsed;elapsed.start();
    QTimer::singleShot(1800,[]{QSqlQuery change;change.exec("UPDATE radio_route_journal SET actions_json='{\"lookup\":\"completed\"}' WHERE turn_id='current'");});
    if(waitForProbe(observe,6000)!="settled"||elapsed.elapsed()<3100)return 1;
    // 旧轮pending仍在，但不会把本轮已经完成的查询误判为运行中。
    if(probeActivity(observe())!="idle")return 1;
    query.exec("SELECT actions_json FROM radio_route_journal WHERE turn_id='old'");
    if(!query.next()||query.value(0).toString()!="{\"lookup\":\"pending\"}")return 1;
    view={{"latestMessage","current"},{"routes",QJsonArray{QJsonObject{{"turnId","current"},{"actions",QJsonObject{{"lookup","pending"}}}}}}};
    if(waitForProbe([&]{return view;},1700)!="timeout")return 1;
    view.insert("routes",QJsonArray{QJsonObject{{"turnId","current"},{"actions",QJsonObject{{"lookup","awaiting_confirmation"}}}}});
    if(probeActivity(view)!="needs_user")return 1;
    view.insert("routes",QJsonArray{QJsonObject{{"turnId","current"},{"actions",QJsonObject{{"lookup","dispatched"}}}}});
    view.insert("catalogChoices",QJsonObject{{"turn","current"},{"action","lookup"},{"artists",QJsonArray{QJsonObject{{"id",7763}}}}});
    if(probeActivity(view)!="needs_user")return 1;
    view.insert("catalogChoices",QJsonObject{{"turn","old"},{"action","lookup"},{"artists",QJsonArray{QJsonObject{{"id",7763}}}}});
    if(probeActivity(view)!="running")return 1;
    view.insert("routesKnown",false);if(probeActivity(view)!="unknown")return 1;
    if(!probeMustStop("timeout")||!probeMustStop("unknown")||!probeMustStop("settled",false)||probeMustStop("needs_user"))return 1;
    QJsonObject prepared{{"conversation",QJsonObject{{"journalPending",false},{"clearPending",false},{"clearing",false}}}};
    if(!probePrepared(prepared)||!probeBusinessIsolated("old","new","")||!probeBusinessIsolated("old","new","new")||
        probeBusinessIsolated("old","old","")||probeBusinessIsolated("old","","")||probeBusinessIsolated("old","third","new"))return 1;
    prepared.insert("lookupContext",QJsonObject{{"stale",true}});if(probePrepared(prepared))return 1;
    qInfo("音乐探针等待守卫通过：延迟真实只读路由、当前轮隔离、待确认、身份澄清、unknown及整跑停止。");
    database.close();return 0;
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
struct NativeSession {
    std::unique_ptr<MainWindow> window;
    QStringList errors;QJsonArray resolved,failed;
    ConversationClient *conversation=nullptr;MusicTaskClient *task=nullptr;RadioDialog *dialog=nullptr;
    QJsonObject capturedProposal;
    void open() {
        window=std::make_unique<MainWindow>();window->show();window->p_PlayerObject->setMuted(true);
        // 构造函数会排队启动开发身份连接；先让它执行并等明确就绪，再打开助手和读取会话。
        QTest::qWait(1);
        if(AppBackendClient::developmentMode()){
            const bool ready=QTest::qWaitFor([this]{auto *label=window->findChild<QLabel *>("label_DisplayUser");
                return AppBackendClient::instance()->authenticated()&&label&&label->text()==QStringLiteral("本机开发测试");},20000);
            if(!ready){errors.append("窗口开发身份尚未就绪，未打开助手或发送业务消息");return;}
        }
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
        // 从真实界面队列模型读取元数据ID，不从音频URL猜曲目身份。
        QJsonArray queueIds;auto *queueView=window?window->findChild<QTableView *>("queueView"):nullptr;
        auto *queueModel=queueView?queueView->model():nullptr;
        if(queueModel)for(int index=0;index<queueModel->rowCount();++index){const auto value=queueModel->index(index,1).data(SongListDelegate::SongIdRole);queueIds.append(value.isValid()?QJsonValue(double(value.toLongLong())):QJsonValue(QJsonValue::Null));}
        auto *transcript=dialog?dialog->findChild<QPlainTextEdit *>("text_RadioConversation"):nullptr;
        auto *browser=dialog?dialog->findChild<QTextBrowser *>("browser_RadioConversation"):nullptr;
        QJsonArray playlistAnchors;
        if(browser){auto matches=QRegularExpression(QStringLiteral("href=\"(tango-playlist[^\"]{0,500})\"")).globalMatch(browser->toHtml());
            while(matches.hasNext()&&playlistAnchors.size()<10)playlistAnchors.append(matches.next().captured(1));}
        QJsonArray playlistCandidates;
        const auto playlistContext=window?window->property("tangoPlaylistContext").toJsonObject():QJsonObject();
        for(const auto &value:playlistContext.value("playlists").toArray())if(playlistCandidates.size()<10){const auto row=value.toObject();
            playlistCandidates.append(QJsonObject{{"id",row.value("id")},{"name",row.value("name")}});}
        const QString profile=window?window->accountProfile():QString();
        return {{"conversationId",cs.value("id")},{"messageId",latestUser(cs)},{"conversation",cs},{"task",ts},
            {"preference",readPreference(profile)},{"routes",readRoutes(profile,conversation?conversation->deviceId():QString(),cs.value("id").toString())},
            {"pendingMemory",pendingMemory()},{"recommendations",recommendations},
            {"visibleConversation",browser?browser->toPlainText().left(60000):QString()},
            {"conversationBrowserVisible",browser&&browser->isVisible()},
            {"visiblePlaylistAnchors",playlistAnchors},{"playlistCandidates",playlistCandidates},
            {"lookupContext",window?window->property("tangoLookupContext").toJsonObject():QJsonObject()},
            {"catalogChoices",window?window->property("tangoCatalogChoices").toJsonObject():QJsonObject()},
            {"playback",QJsonObject{{"state",window?int(window->p_PlayerObject->state()):0},{"queueCount",window?window->p_PlayerList->mediaCount():0},
                {"positionMs",double(window?window->p_PlayerObject->position():0)},{"durationMs",double(window?window->p_PlayerObject->duration():0)},{"currentIndex",window?window->p_PlayerList->currentIndex():-1},{"queueIds",queueIds},{"resolvedIds",resolved},{"failedIds",failed}}},
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
        if(!conversation||!task||!dialog)return "missing_client";
        return waitForProbe([this]{const auto current=conversation->snapshot();bool known=false;
            const auto routes=readRoutes(window->accountProfile(),conversation->deviceId(),current.value("id").toString(),&known);
            return QJsonObject{{"conversationBusy",conversation->busy()},{"taskBusy",task->busy()},{"task",task->snapshot()},
                {"routesKnown",known},{"routes",routes},{"latestMessage",latestUser(current)},
                {"catalogChoices",window->property("tangoCatalogChoices").toJsonObject()},
                {"conversation",current},{"pendingMemory",pendingMemory()}};},timeoutMs);
    }
    QString waitForStatus(const QString &status,int timeoutMs) const {
        if(status.isEmpty())return settle(timeoutMs);
        const bool reached=QTest::qWaitFor([&]{return task&&conversation&&!conversation->busy()&&task->snapshot().value("status")==status&&
            (status!="waiting_tools"||!task->snapshot().value("pendingSteps").toArray().isEmpty());},timeoutMs);
        return reached?"reached_"+status:"timeout";
    }
};
int checkSetupGuards() {
    // 真实产品窗口与正式客户端，只有回环固定后台；不创建任何模型或曲库适配器。
    RoutingFixtures::Fixture fixture(true);int creates=0;
    const auto original=fixture.server.handler;
    fixture.server.handler=[&fixture,&creates,original](const RoutingFixtures::Request &incoming)->RoutingFixtures::Response {
        if(incoming.method=="POST"&&incoming.path=="/v1/conversations"){
            auto response=original(incoming);fixture.current.insert("id",QString("conversation-owned-%1").arg(++creates));
            response.body.insert("conversation",fixture.current);return response;
        }
        auto request=incoming;
        const auto prefix="/v1/conversations/"+fixture.current.value("id").toString();
        if(!fixture.current.isEmpty()&&request.path.startsWith(prefix+"/"))
            request.path="/v1/conversations/conversation-routing"+request.path.mid(prefix.size());
        return original(request);
    };
    if(!fixture.login()){qWarning("准备守卫失败：固定账号登录");return 1;}
    NativeSession session;session.open();
    if(!session.conversation||!session.dialog||session.settle(5000)!="settled"){qWarning("准备守卫失败：窗口初始化");return 1;}
    if(!session.send("你能做什么")||session.settle(5000)!="settled"){qWarning("准备守卫失败：旧轮业务");return 1;}
    const auto old=session.conversation->snapshot().value("id").toString();
    const auto requestsBefore=fixture.server.count("POST","/v1/conversations");
    if(old.isEmpty()||requestsBefore!=1){qWarning("准备守卫失败：旧会话证据");return 1;}
    if(!session.send("新对话")||session.settle(5000)!="settled"||!probePrepared(session.snapshot())||
        fixture.server.count("POST","/v1/conversations")!=requestsBefore){qWarning("准备守卫失败：懒创建空态");return 1;}
    if(!session.send("你能做什么")||session.settle(5000)!="settled"){qWarning("准备守卫失败：新业务");return 1;}
    const auto after=session.snapshot();const auto current=after.value("conversationId").toString();
    if(!probeBusinessIsolated(old,current,{})||creates!=2||after.value("conversation").toObject().value("messages").toArray().size()!=2){
        qWarning("准备守卫失败：发送后的会话归属");return 1;}
    const auto routes=after.value("routes").toArray();
    if(routes.size()!=1||routes.first().toObject().value("turnId")!=after.value("messageId")){qWarning("准备守卫失败：路由跨会话");return 1;}
    qInfo("新对话准备守卫通过：旧会话→正式新对话懒空态（零创建请求）→下一业务创建不同ID，消息和路由隔离；模型/真实曲库0。");
    session.close();return 0;
}
}


namespace {
QJsonObject privateJson(const QString &path) {
    const QFileInfo info(path);
    if(!info.isFile()||info.isSymLink()||info.size()>2*1024*1024||info.ownerId()!=uint(getuid())||
        (info.permissions()&(QFileDevice::ReadGroup|QFileDevice::WriteGroup|QFileDevice::ReadOther|QFileDevice::WriteOther)))return {};
    QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
QJsonObject currentRoute(const QJsonObject &snapshot) {
    for(const auto &value:snapshot.value("routes").toArray())if(value.toObject().value("turnId")==snapshot.value("messageId"))return value.toObject();return {};
}

}

int main(int argc,char **argv) {
    umask(0077);
    if(argc==2&&QByteArray(argv[1])=="--check-wait-guards"){QCoreApplication application(argc,argv);return checkWaitGuards();}
    QApplication application(argc,argv);application.setQuitOnLastWindowClosed(false);
    if(argc==2&&QByteArray(argv[1])=="--check-setup-guards")return checkSetupGuards();
    QCoreApplication::setApplicationName("Tango music requirements live probe");
    QCommandLineParser parser;parser.addHelpOption();parser.addOption({"output","本轮目录下绝对观察文件路径","path"});parser.process(application);
    const QFileInfo outputInfo(parser.value("output"));const QString output=outputInfo.absoluteFilePath();
    const QString run=outputInfo.absolutePath();const QFileInfo runInfo(run);
    if(parser.value("output").isEmpty()||!QDir::isAbsolutePath(parser.value("output"))||QFileInfo::exists(output)||!runInfo.isDir()||runInfo.isSymLink()||QDir(run).canonicalPath()!=run||runInfo.ownerId()!=uint(getuid()))return 2;
    const auto manifest=privateJson(run+"/manifest.json"),authorization=privateJson(run+"/authorization.json");
    const auto connection=privateJson(run+"/connection.json"),netease=privateJson(run+"/netease-connection.json"),plan=privateJson(run+"/plan.json");
    const QUrl backend(connection.value("backendURL").toString());
    if(manifest.value("purpose")!="actual_qt_music_requirements_acceptance"||authorization.value("manifestSeal")!=manifest.value("seal")||authorization.value("newAuthorization")!=true||
        authorization.value("maximumProviderRequests").toInt()<1||authorization.value("maximumProviderRequests").toInt()>80||
        connection.value("manifestSeal")!=manifest.value("seal")||netease.value("manifestSeal")!=manifest.value("seal")||netease.value("anonymous")!=true||
        connection.value("dataDir").toString()!=run+"/qt-data"||plan.value("tasks").toArray().size()!=8||
        backend.scheme()!="http"||backend.host()!="127.0.0.1"||backend.port()<1||!backend.path().isEmpty()||backend.hasQuery()||backend.hasFragment()||!backend.userInfo().isEmpty())return 2;
    QFile lock(run+"/probe-execution-lock.json");if(!lock.open(QIODevice::WriteOnly|QIODevice::NewOnly))return 2;
    lock.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);lock.write(QJsonDocument(QJsonObject{{"pid",double(getpid())},{"manifestSeal",manifest.value("seal")},{"mode","real_qt_no_fixture"}}).toJson());lock.close();
    const auto environment=connection.value("qtEnvironment").toObject();
    for(const QString &key:QStringList{"MUSICPLAYER_BACKEND_URL","MUSICPLAYER_DEVELOPMENT_MODE","MUSICPLAYER_DEVELOPMENT_TOKEN_FILE","MUSICPLAYER_DATA_DIR","MUSICPLAYER_PROJECT_DIR"})
        qputenv(key.toUtf8().constData(),environment.value(key).toString().toUtf8());
    qputenv("MUSICPLAYER_NETEASE_PORT",QByteArray::number(netease.value("port").toInt()));
    qputenv("MUSICPLAYER_REDIS_PORT","invalid-isolated-music-requirements");
    qputenv("LANGSMITH_TRACING","false");qputenv("LANGCHAIN_TRACING_V2","false");
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    QFile controlFile(connection.value("controlTokenFile").toString());
    if(!controlFile.open(QIODevice::ReadOnly))return 2;
    const auto controlToken=controlFile.readAll().trimmed();if(controlToken.size()!=64)return 2;
    QNetworkAccessManager controlNetwork;controlNetwork.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    const auto context=[&](const QString &caseId,int turn) {
        auto target=backend;target.setPath("/__music_requirements__/context");QNetworkRequest request(target);
        request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");request.setRawHeader("X-Tango-Evaluation-Control",controlToken);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
        auto *reply=controlNetwork.post(request,QJsonDocument(QJsonObject{{"caseId",caseId},{"turn",turn}}).toJson(QJsonDocument::Compact));
        const bool ready=QTest::qWaitFor([&]{return reply->isFinished();},10000);
        const bool accepted=ready&&reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==200&&QJsonDocument::fromJson(reply->readAll()).object().value("accepted")==true;
        if(!ready)reply->abort();reply->deleteLater();return accepted;
    };
    QJsonObject report{{"schemaVersion",1},{"scope","真实MainWindow输入、正式HTTP和真实匿名网易云；所有模型角色共用新80次上限"},
        {"manifestSeal",manifest.value("seal")},{"observations",QJsonArray()},{"tasks",QJsonArray()},{"setupObservations",QJsonArray()},
        {"semanticAccuracy",QJsonValue(QJsonValue::Null)},{"overallBusinessCompletionRate",QJsonValue(QJsonValue::Null)},
        {"automaticRetry",false},{"catalogFixture",false},{"modelJudge",false}};
    if(!writeJson(output,report))return 2;
    QObject bootstrapContext;auto *account=AppBackendClient::instance();bool done=false;QString bootstrapError;
    account->bootstrapDevelopmentSession(&bootstrapContext,[&](const QString &error){bootstrapError=error;done=true;});
    if(!QTest::qWaitFor([&]{return done;},20000)||!bootstrapError.isEmpty()||!account->authenticated()){
        report.insert("runError","development_bootstrap_failed");writeJson(output,report);return 1;}
    done=false;account->refreshAgentStatus(&bootstrapContext,[&](const QString &error){bootstrapError=error;done=true;});
    if(!QTest::qWaitFor([&]{return done;},20000)||!bootstrapError.isEmpty()||!account->agentReady()||!account->routingAvailable()){
        report.insert("runError","formal_agent_status_unavailable");writeJson(output,report);return 1;}
    NativeSession session;session.open();
    if(!session.dialog||!session.task||!session.conversation){report.insert("runError","native_client_missing");writeJson(output,report);return 1;}
    QJsonArray observations,tasks,setups;
    const auto stopRun=[&](const QString &reason){
        // 停止后补齐冻结题面的未执行槽位，但不再推进context或发送用户消息，晚调用仍归原题。
        QSet<QString> seen;for(const auto &row:observations)seen.insert(row.toObject().value("caseId").toString()+":"+QString::number(row.toObject().value("turn").toInt()));
        QSet<QString> reported;for(const auto &row:tasks)reported.insert(row.toObject().value("id").toString());
        for(const auto &value:plan.value("tasks").toArray()){const auto item=value.toObject();const auto id=item.value("id").toString();int turn=0;
            for(const auto &input:item.value("inputs").toArray()){++turn;if(seen.contains(id+":"+QString::number(turn)))continue;
                observations.append(QJsonObject{{"caseId",id},{"turn",turn},{"text",input},{"sent",false},{"waitStatus","blocked_run_stopped"},
                    {"blockedReason",reason},{"businessStatus","blocked"},{"executionEnded",false}});}
            if(!reported.contains(id))tasks.append(QJsonObject{{"id",id},{"executionPrerequisitesSatisfied",false},{"businessStatus","blocked_or_incomplete"},{"stopReason",reason}});
        }
        report.insert("observations",observations);report.insert("tasks",tasks);report.insert("stopped",true);report.insert("stopReason",reason);
        report.insert("finished",false);report.insert("remainingSlotsRecordedBlocked",true);report.insert("finishedAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        writeJson(output,redact(report).toObject());session.close();return 1;
    };
    const auto initialState=session.settle(20000);if(probeMustStop(initialState))return stopRun("initial_"+initialState);
    for(const auto &value:plan.value("tasks").toArray()) {
        const auto definition=value.toObject();const auto id=definition.value("id").toString();
        if(!context(id,0))return stopRun("case_context_rejected");
        // 使用正式聊天的新对话入口隔离每题；此操作单列为准备，不冒充业务题通过。
        const auto previousConversation=session.conversation->snapshot().value("id").toString();
        const bool setupSent=session.send("新对话");const auto setupState=setupSent?session.settle(30000):QString("input_not_accepted");
        const bool isolatedConversation=probePrepared(session.snapshot());
        setups.append(QJsonObject{{"caseId",id},{"sent",setupSent},{"waitStatus",setupState},{"isolatedConversation",isolatedConversation},{"after",redact(session.snapshot())}});
        report.insert("setupObservations",setups);writeJson(output,redact(report).toObject());
        if(!setupSent||setupState!="settled"||!isolatedConversation)return stopRun("case_setup_not_isolated_or_"+setupState);
        bool blocked=false;int turn=0;QString withinCaseConversation;
        for(const auto &input:definition.value("inputs").toArray()) {
            ++turn;session.errors.clear();session.resolved={};session.failed={};const auto before=session.snapshot();
            if(!context(id,turn))return stopRun("turn_context_rejected");
            const auto text=input.toString();QElapsedTimer elapsed;elapsed.start();bool sent=false;QString state="blocked_needs_user";
            if(!blocked&&!text.isEmpty()){sent=session.send(text);state=sent?session.settle(180000):QString("input_not_accepted");}
            auto row=session.snapshot();row.insert("caseId",id);row.insert("turn",turn);row.insert("text",text);row.insert("before",before);
            row.insert("sent",sent);row.insert("waitStatus",state);row.insert("elapsedMs",double(elapsed.elapsed()));row.insert("currentRoute",currentRoute(row));
            row.insert("executionEnded",sent&&(state=="settled"||state=="needs_user"));row.insert("businessStatus",state=="needs_user"?"needs_user":"unverified_until_evidence_review");
            const auto currentConversation=row.value("conversationId").toString();
            const bool businessIsolated=!sent||probeBusinessIsolated(previousConversation,currentConversation,withinCaseConversation);
            row.insert("businessConversationIsolated",businessIsolated);
            if(sent&&businessIsolated&&withinCaseConversation.isEmpty())withinCaseConversation=currentConversation;
            // 静止或错误回复都可能settled，绝不自动当作任务完成。
            observations.append(redact(row));report.insert("observations",observations);
            if(!writeJson(output,redact(report).toObject()))return 2;
            session.window->grab().save(run+QString("/%1-%2.png").arg(id).arg(turn));
            qInfo().noquote()<<id<<"turn"<<turn<<state;
            if(!businessIsolated)return stopRun("business_conversation_not_isolated");
            if(probeMustStop(state))return stopRun("turn_"+id+"_"+QString::number(turn)+"_"+state);
            if(state=="needs_user")blocked=true;
        }
        tasks.append(QJsonObject{{"id",id},{"executionPrerequisitesSatisfied",!blocked},{"businessStatus","unverified_until_evidence_review"}});
        report.insert("tasks",tasks);writeJson(output,redact(report).toObject());
    }
    report.insert("finished",true);report.insert("finishedAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    writeJson(output,redact(report).toObject());session.close();return 0;
}
