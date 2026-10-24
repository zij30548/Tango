#ifndef ROUTING_FIXTURES_H
#define ROUTING_FIXTURES_H
#include "appbackendclient.h"
#include "conversationclient.h"
#include "musictaskclient.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <functional>
#include <memory>

namespace RoutingFixtures {
struct Request {
    QByteArray method;
    QString path;
    QJsonObject body;
};
struct Response { QJsonObject body; int status = 200; int delay = 0; bool hang = false; };
class FakeServer : public QObject {
public:
    QTcpServer server;
    QVector<Request> requests;
    std::function<Response(const Request &)> handler;
    FakeServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                auto bytes = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes, handled] {
                    bytes->append(socket->readAll());
                    if (*handled) return;
                    const auto end = bytes->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const auto header = bytes->left(end);
                    int length = 0;
                    for (const auto &line : header.split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    if (bytes->size() < end + 4 + length) return;
                    *handled = true;
                    Request request{header.split(' ').value(0), QUrl::fromEncoded(header.split(' ').value(1)).path(),
                        QJsonDocument::fromJson(bytes->mid(end + 4, length)).object()};
                    requests.append(request);
                    const auto response = handler(request);
                    if (response.hang) return;
                    QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(response.delay, this, [guard, response] {
                        if (!guard || guard->state() != QAbstractSocket::ConnectedState) return;
                        const auto body = QJsonDocument(response.body).toJson(QJsonDocument::Compact);
                        guard->write("HTTP/1.1 " + QByteArray::number(response.status) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        guard->disconnectFromHost();
                    });
                });
            }
        });
    }
    QByteArray url() const { return "http://127.0.0.1:" + QByteArray::number(server.serverPort()); }
    int count(const QByteArray &method, const QString &path) const {
        int count = 0; for (const auto &r : requests) if (r.method == method && r.path == path) ++count; return count;
    }
};
class Environment {
public:
    ~Environment() { for (auto entry = values.crbegin(); entry != values.crend(); ++entry)
        if (entry->existed) qputenv(entry->name.constData(), entry->value); else qunsetenv(entry->name.constData()); }
    void set(const QByteArray &name, const QByteArray &value) {
        values.append({name, qgetenv(name.constData()), qEnvironmentVariableIsSet(name.constData())});
        qputenv(name.constData(), value);
    }
private:
    struct Value { QByteArray name, value; bool existed; }; QVector<Value> values;
};

struct Fixture {
    Environment environment;
    QTemporaryDir directory;
    FakeServer server;
    AppBackendClient ownedBackend{nullptr, 100};
    AppBackendClient *backend;
    ConversationClient *client = nullptr;
    QJsonObject current, task;
    QHash<QString,QJsonObject> receipts;
    QHash<QString,QJsonObject> routePlans, accountConversations, accountTasks, taskRequestReceipts, taskRequestSnapshots;
    QHash<QString,QString> instructions, routeOwners, taskRequestOwners;
    QSet<QString> routeReplies, eventIds;
    QString accountName = "fixture";
    QString instruction, messageId, routeStatus = "completed";
    bool routingVersion = true;
    int routeDelay = 0, runningReads = 0;
    QJsonObject overridePlan;
    std::function<QJsonObject(const QString &, const QString &)> makePlan;
    explicit Fixture(bool singleton = false) {
        environment.set("MUSICPLAYER_DATA_DIR",directory.path().toUtf8());
        environment.set("MUSICPLAYER_BACKEND_URL",server.url());
        environment.set("MUSICPLAYER_DEVELOPMENT_MODE","");
        environment.set("MUSICPLAYER_DEVELOPMENT_TOKEN_FILE","");
        environment.set("MUSICPLAYER_NETEASE_PORT","1");
        environment.set("MUSICPLAYER_REDIS_PORT","invalid-fixture-port");
        backend = singleton ? AppBackendClient::instance() : &ownedBackend;
        makePlan = [](const QString &mid,const QString &text) {
            return plan(mid,{"help"},{action("reply","assistant.reply",text,{{"kind","help"},{"text","可以找歌、查新作、解释推荐、调整任务与管理本机口味。"}})});
        };
        server.handler = [this](const Request &r) -> Response {
            if(r.path=="/v1/auth/login") {
                const auto requested = r.body.value("username").toString();
                if (requested != accountName) {
                    accountConversations.insert(accountName, current); accountTasks.insert(accountName, task);
                    accountName = requested;
                    current = accountConversations.value(accountName); task = accountTasks.value(accountName);
                }
                const auto userId = accountName == "fixture" ? QString("routing-user") : "routing-" + accountName;
                return {{{"token",QString(43,QLatin1Char('s'))},{"user",QJsonObject{{"id",userId},{"username",accountName}}},{"expiresAt",QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODate)}}};
            }
            if(r.path=="/v1/auth/logout") return {{{"ok",true}}};
            if(r.path=="/v1/agent/status") {
                QJsonObject value{{"configured",true},{"model","offline-routing-fixture"},{"taskRuntime",true},{"taskVersion",1},{"conversationVersion",1}};
                if(routingVersion)value.insert("routingVersion",1);
                return {value};
            }
            if(r.path=="/v1/account/usage")return {{{"daily",QJsonObject{{"limit",30},{"used",0},{"remaining",30},{"resetAt","2030-01-01T00:00:00Z"}}},{"total",QJsonObject{{"limit",300},{"used",0},{"remaining",300}}},{"disabled",false}}};
            if(r.path.startsWith("/v1/conversations/latest/"))return {{{"conversation",current.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(current)}}};
            if(r.path=="/v1/conversations" && r.method=="POST") {
                current={{"id","conversation-routing"},{"deviceId",r.body.value("deviceId")},{"version",0},{"messages",QJsonArray()},{"hasEarlier",false},{"earliestSequence",0}};
                return {{{"conversation",current}}};
            }
            if(r.path==messagesPath() && r.method=="POST") {
                messageId=r.body.value("messageId").toString();instruction=r.body.value("content").toString();
                instructions.insert(messageId, instruction);
                if(!receipts.contains(messageId)) {
                    append(messageId,messageId,"user","user",instruction);
                    receipts.insert(messageId,{{"messageId",messageId},{"turnId",messageId},{"status","accepted"},{"dispatchHint",routingVersion?"router":"task"}});
                }
                return {{{"conversation",current},{"receipt",receipts.value(messageId)}}};
            }
            if(r.path.startsWith(messagesPath()+"/")&&r.path.endsWith("/route")) {
                const auto mid=r.path.section('/',-2,-2);
                // GET 只观察已提交的方案；不能用最新一条指令重建旧轮次，或在恢复时偷偷创建路由。
                if (!routePlans.contains(mid)) {
                    if (r.method != "POST") return {{{"error",QJsonObject{{"message","route not found"}}}},404};
                    routePlans.insert(mid, overridePlan.isEmpty() ? makePlan(mid, instructions.value(mid)) : overridePlan);
                    routeOwners.insert(mid, accountName);
                }
                if (routeOwners.value(mid) != accountName) return {{{"error",QJsonObject{{"message","route not found"}}}},404};
                auto selected=routePlans.value(mid);
                auto status=routeStatus;
                if(runningReads>0) {status="running";--runningReads;}
                if(status=="completed"&&!routeReplies.contains(mid)) {
                    routeReplies.insert(mid);
                    for(const auto &value:selected.value("actions").toArray()) {
                        const auto a=value.toObject();if(a.value("capability").toString()=="assistant.reply")
                            append("route-"+mid+"-"+a.value("id").toString(),mid,"assistant","route",a.value("parameters").toObject().value("text").toString());
                    }
                }
                return {{{"conversation",current},{"route",QJsonObject{{"id","route-"+mid},{"messageId",mid},{"status",status},{"result",status=="completed"?QJsonValue(selected):QJsonValue(QJsonValue::Null)}}}},200,routeDelay};
            }
            if(r.path.startsWith(messagesPath()+"/")&&r.method=="GET")return {{{"conversation",current},{"receipt",receipts.value(r.path.section('/',-1))}}};
            if(r.path=="/v1/conversations/conversation-routing/events") {
                const auto id=r.body.value("eventId").toString();
                if(!eventIds.contains(id)) {
                    eventIds.insert(id);
                    if(r.body.value("kind").toString()=="local_reply")append(id,r.body.value("turnId").toString(),"assistant","device_report",r.body.value("content").toString());
                    if(r.body.value("kind").toString()=="task_sync") {
                        current.insert("latestTaskId",r.body.value("taskId"));
                        current.insert("latestTaskLink",QJsonObject{{"taskId",r.body.value("taskId")},{"turnId",r.body.value("turnId")},{"revision",r.body.value("taskRevision")}});
                    }
                }
                return {{{"conversation",current},{"receipt",QJsonObject{{"eventId",id},{"status","recorded"}}}}};
            }
            if (r.path.startsWith("/v1/tasks/requests/")) {
                const auto key=r.path.section('/',-1);
                if (taskRequestReceipts.contains(key) && taskRequestOwners.value(key)==accountName)
                    return {{{"task",taskRequestSnapshots.value(key)},{"receipt",taskRequestReceipts.value(key)}}};
                return {{{"task",QJsonValue(QJsonValue::Null)},{"receipt",QJsonObject{{"requestId",key},{"deviceId",r.path.section('/',-2,-2)},{"status","unknown"}}}}};
            }
            if(r.path.startsWith("/v1/tasks/latest/"))return {{{"task",task.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(task)}}};
            if(r.path=="/v1/tasks"&&r.method=="POST") {
                task={{"id","music-routing-task"},{"deviceId",r.body.value("deviceId")},{"revision",1},{"stateVersion",1},{"status","preview"},{"pendingSteps",QJsonArray()},
                    {"messages",QJsonArray{QJsonObject{{"id","task-user-1"},{"role","user"},{"content",r.body.value("instruction")},{"revision",1}}}},
                    {"preview",QJsonObject{{"version",1},{"title","fixture playlist"},{"tracks",QJsonArray{QJsonObject{{"id",101},{"title","安静的歌"},{"artist","测试歌手"},{"durationMs",180000},{"reason","离线测试候选"}}}}}}};
                const auto key=r.body.value("requestId").toString();
                taskRequestReceipts.insert(key,{{"requestId",key},{"deviceId",r.body.value("deviceId")},{"taskId",task.value("id")},{"status","recorded"},{"action","create"}});
                taskRequestSnapshots.insert(key,task);
                taskRequestOwners.insert(key,accountName);
                return {{{"task",task}}};
            }
            if(r.path=="/v1/tasks/music-routing-task/actions") {
                if(r.body.value("expectedRevision")!=task.value("revision"))return {{{"error",QJsonObject{{"message","stale revision"}}}},409};
                task.insert("stateVersion",task.value("stateVersion").toInt()+1);
                const auto name=r.body.value("action").toString();
                if(name=="turn") {
                    const int revision=task.value("revision").toInt()+1;task.insert("revision",revision);
                    auto messages=task.value("messages").toArray();messages.append(QJsonObject{{"id","task-user-"+QString::number(revision)},{"role","user"},{"content",r.body.value("instruction")},{"revision",revision}});task.insert("messages",messages);
                } else if(QStringList{"pause","cancel","resume"}.contains(name)) {
                    task.insert("revision",task.value("revision").toInt()+1);
                    task.insert("status",name=="pause"?"paused":name=="cancel"?"cancelled":"preview");
                }
                const auto key=r.body.value("requestId").toString();
                taskRequestReceipts.insert(key,{{"requestId",key},{"deviceId",r.body.value("deviceId")},{"taskId",task.value("id")},
                    {"status","recorded"},{"action",name},{"expectedRevision",r.body.value("expectedRevision")}});
                taskRequestSnapshots.insert(key,task);taskRequestOwners.insert(key,accountName);
                return {{{"task",task}}};
            }
            if(r.path=="/v1/tasks/music-routing-task")return {{{"task",task}}};
            return {{{"error",QJsonObject{{"message","unexpected fixture route"}}}},404};
        };
        client=new ConversationClient(nullptr,backend);
    }
    ~Fixture(){delete client;}
    static QJsonObject action(const QString &id,const QString &capability,const QString &source,const QJsonObject &parameters,const QJsonArray &dependencies={}) {
        return {{"id",id},{"capability",capability},{"source",source},{"dependsOn",dependencies},{"parameters",parameters}};
    }
    static QJsonObject plan(const QString &mid,const QJsonArray &intents,const QJsonArray &actions) {
        return {{"version",1},{"messageId",mid},{"planner","deterministic"},{"strategy","direct"},{"intents",intents},{"actions",actions},{"missingSlots",QJsonArray()},
            {"slots",QJsonObject{{"artist",""},{"targetMinutes",0}}}};
    }
    void append(const QString &id,const QString &turn,const QString &role,const QString &source,const QString &content) {
        const int version=current.value("version").toInt()+1;auto messages=current.value("messages").toArray();
        messages.append(QJsonObject{{"id",id},{"turnId",turn},{"role",role},{"source",source},{"content",content},{"sequence",version}});
        current.insert("messages",messages);current.insert("version",version);current.insert("earliestSequence",1);
    }
    bool login(const QString &username = QStringLiteral("fixture")) {
        bool done=false;QString error;
        backend->login(username,"fixture-password",backend,[&](const QString &e){error=e;done=true;});
        if(!QTest::qWaitFor([&]{return done;},1500)||!error.isEmpty())return false;
        done=false;backend->refreshAgentStatus(backend,[&](const QString &e){error=e;done=true;});
        return QTest::qWaitFor([&]{return done;},1500)&&error.isEmpty();
    }
    QString messagesPath()const{return "/v1/conversations/conversation-routing/messages";}
    QString routePath()const{return messagesPath()+"/"+messageId+"/route";}
};
}
#endif
