#include "appbackendclient.h"
#include "musictaskclient.h"
#include "neteaseclient.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QtTest>
#include <algorithm>
#include <functional>
#include <memory>

namespace {
class ScopedEnvironment {
public:
    void set(const QByteArray &name, const QByteArray &value) {
        saved.append({name, qgetenv(name.constData()), qEnvironmentVariableIsSet(name.constData())});
        qputenv(name.constData(), value);
    }
    ~ScopedEnvironment() {
        for (auto it = saved.crbegin(); it != saved.crend(); ++it)
            if (it->existed) qputenv(it->name.constData(), it->value); else qunsetenv(it->name.constData());
    }
private:
    struct Value { QByteArray name, value; bool existed; };
    QVector<Value> saved;
};

class FakeServer : public QObject {
public:
    struct Request {
        QByteArray target, headers, body;
        QString path() const { return QUrl::fromEncoded(target).path(); }
        QUrlQuery query() const { return QUrlQuery(QUrl::fromEncoded(target)); }
        QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
    };
    struct Response {
        QJsonObject body;
        int status = 200;
        int delay = 0;
        bool hang = false;
        bool stream = false;
        QVector<QByteArray> chunks = {};
        int chunkDelay = 5;
        bool closeStream = false;
        QByteArray contentType = "text/event-stream";
        int extraContentLength = 0;
    };
    QTcpServer server;
    QVector<Request> requests;
    QVector<QPointer<QTcpSocket>> eventSockets;
    QHash<QString, QVector<Response>> overrides;
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
                    const int end = bytes->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray headers = bytes->left(end);
                    int length = 0;
                    for (const auto &line : headers.split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    if (bytes->size() < end + 4 + length) return;
                    *handled = true;
                    Request request{headers.split(' ').value(1), headers, bytes->mid(end + 4, length)};
                    requests.append(request);
                    const QString path = request.path();
                    Response response;
                    if (!overrides[path].isEmpty()) response = overrides[path].takeFirst();
                    else if (handler) response = handler(request);
                    else response = {{{"error", "unhandled test route"}}, 404};
                    // 旧业务夹具只关心曲目；统一补全新增本机作品接口的受控分页回执。
                    if (path == "/artist/tracks" && response.body.value("songs").isArray()) {
                        if (!response.body.contains("artistId")) response.body.insert("artistId", request.query().queryItemValue("artistId").toLongLong());
                        if (!response.body.contains("offset")) response.body.insert("offset", request.query().queryItemValue("offset").toInt());
                        if (!response.body.contains("limit")) response.body.insert("limit", request.query().queryItemValue("limit").toInt());
                        if (!response.body.contains("total")) response.body.insert("total", response.body.value("songs").toArray().size() + request.query().queryItemValue("offset").toInt());
                        if (!response.body.contains("more")) response.body.insert("more", false);
                        if (!response.body.contains("checkedAt")) response.body.insert("checkedAt", QDateTime::currentMSecsSinceEpoch());
                    }
                    if (response.hang) return;
                    QPointer<QTcpSocket> guard(socket);
                    if (response.stream) {
                        eventSockets.append(guard);
                        QTimer::singleShot(response.delay, this, [this, guard, response] {
                            if (!guard || guard->state() != QAbstractSocket::ConnectedState) return;
                            guard->write("HTTP/1.1 " + QByteArray::number(response.status) + " Test\r\nContent-Type: " +
                                response.contentType + "\r\nConnection: keep-alive\r\n\r\n");
                            int delay = 0;
                            for (const auto &chunk : response.chunks) {
                                delay += response.chunkDelay;
                                QTimer::singleShot(delay, this, [guard, chunk] {
                                    if (guard && guard->state() == QAbstractSocket::ConnectedState) guard->write(chunk);
                                });
                            }
                            if (response.closeStream) QTimer::singleShot(delay + 5, this, [guard] {
                                if (guard) guard->disconnectFromHost();
                            });
                        });
                        return;
                    }
                    QTimer::singleShot(response.delay, this, [guard, response] {
                        if (!guard || guard->state() != QAbstractSocket::ConnectedState) return;
                        const QByteArray body = QJsonDocument(response.body).toJson(QJsonDocument::Compact);
                        guard->write("HTTP/1.1 " + QByteArray::number(response.status) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()+response.extraContentLength) + "\r\n\r\n" + body);
                        guard->disconnectFromHost();
                    });
                });
            }
        });
    }
    QByteArray base() const { return "http://127.0.0.1:" + QByteArray::number(server.serverPort()); }
    int count(const QString &path) const {
        int value = 0; for (const auto &request : requests) if (request.path() == path) ++value; return value;
    }
    Request last(const QString &path) const {
        for (auto it = requests.crbegin(); it != requests.crend(); ++it) if (it->path() == path) return *it;
        return {};
    }
    void writeEvent(const QByteArray &bytes) {
        for (const auto &socket : eventSockets)
            if (socket && socket->state() == QAbstractSocket::ConnectedState) socket->write(bytes);
    }
    int activeStreams() const {
        int count = 0;
        for (const auto &socket : eventSockets)
            if (socket && socket->state() == QAbstractSocket::ConnectedState) ++count;
        return count;
    }
};

QJsonObject progressEvent(int version, const QString &stage = "checking", const QString &status = "researching",
                          int revision = 1) {
    return {{"taskId", "task-1"}, {"revision", revision}, {"stateVersion", version},
        {"status", status}, {"stage", stage}, {"message", QStringLiteral("真实阶段：%1").arg(stage)},
        {"at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
}
QByteArray eventFrame(const QJsonObject &progress) {
    return "id: " + QByteArray::number(progress.value("stateVersion").toInt()) + "\nevent: progress\ndata: " +
        QJsonDocument(progress).toJson(QJsonDocument::Compact) + "\n\n";
}

QJsonObject track(qint64 id, const QString &artist = QStringLiteral("Artist")) {
    QJsonArray credits;
    const auto names = artist.split(QRegularExpression(QStringLiteral("\\s*(?:/|、|,|，|&|;|；| feat\\.? | featuring )\\s*")), Qt::SkipEmptyParts);
    for (const auto &name : names) {
        const qint64 artistId = name == "陶喆" ? 101 : name == "邓紫棋" || name == "G.E.M.邓紫棋" ? 7763 : name == "周杰伦" ? 6452 : 9099;
        credits.append(QJsonObject{{"id", artistId}, {"name", name}});
    }
    return {{"id", id}, {"title", QStringLiteral("真实曲目 %1").arg(id)}, {"artist", artist},
        {"artists", credits},
        {"album", "真实专辑"}, {"coverUrl", "https://p1.music.126.net/cover.jpg"}, {"durationMs", 200000}};
}
struct Fixture {
    ScopedEnvironment env;
    QTemporaryDir dir;
    FakeServer app, music;
    AppBackendClient backend;
    MusicTaskClient *client = nullptr;
    QJsonObject current;
    QJsonArray results;
    int sequence = 0;
    Fixture() {
        Q_ASSERT(dir.isValid()); QDir(dir.path()).mkpath("netease");
        QFile token(dir.filePath("netease/token")); token.open(QIODevice::WriteOnly); token.write("isolated-token-123456789012345"); token.close();
        env.set("MUSICPLAYER_DATA_DIR", dir.path().toUtf8());
        env.set("MUSICPLAYER_NETEASE_PORT", QByteArray::number(music.server.serverPort()));
        env.set("MUSICPLAYER_BACKEND_URL", app.base());
        app.handler = [this](const FakeServer::Request &request) -> FakeServer::Response {
            const auto path = request.path();
            if (path == "/v1/auth/login") {
                const auto name = request.json().value("username").toString();
                return {{{"token", "session-for-test-123456789012345"}, {"user", QJsonObject{{"id", "id-"+name},{"username",name}}},
                    {"expiresAt",QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODate)}}};
            }
            if (path == "/v1/auth/logout") return {{{"ok",true}}};
            if (path == "/v1/agent/status") return {{{"configured",true},{"model","test-model"},{"taskRuntime",true},{"taskVersion",1}}};
            if (path == "/v1/tasks") {
                current = {{"id","task-1"},{"deviceId",request.json().value("deviceId")},{"revision",1},{"stateVersion",++sequence},
                    {"status","waiting_tools"},{"pendingSteps",QJsonArray{scene(),release()}},{"preview",QJsonObject{}}};
                return {{{"task",current}}};
            }
            if (path.startsWith("/v1/tasks/latest/")) return {{{"task", current.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(current)}}};
            if (path == "/v1/tasks/task-1") return {{{"task",current}}};
            if (path.endsWith("/result")) {
                results.append(request.json());
                auto pending = current.value("pendingSteps").toArray();
                const auto parts = path.split('/'); const auto stepId = parts.value(parts.size()-2);
                QJsonArray retained;
                for (const auto &step : pending) if (step.toObject().value("id").toString() != stepId) retained.append(step);
                current.insert("pendingSteps",retained); current.insert("stateVersion",++sequence);
                if (retained.isEmpty()) current.insert("status","preview");
                return {{{"task",current}}};
            }
            if (path == "/v1/tasks/task-1/actions") {
                current.insert("revision",current.value("revision").toInt()+1);
                current.insert("stateVersion",++sequence); current.insert("pendingSteps",QJsonArray());
                const auto action = request.json().value("action").toString();
                current.insert("status",action == "adopt" ? "awaiting_apply" : action == "applied" ? "adopted" : "paused");
                if (action == "adopt") current.insert("adoption",QJsonObject{{"id","adoption-1"},{"previewVersion",1}});
                return {{{"task",current}}};
            }
            return {{{"error",QJsonObject{{"message","unhandled test route"}}}},404};
        };
        music.handler = [this](const FakeServer::Request &request) -> FakeServer::Response {
            const auto path=request.path();
            if (path == "/health") return {{{"app","quiet-netease"},{"version",1},{"catalogVersion",2},{"profileIsolation",1}}};
            if (path == "/radio/search") return {{{"playlists",QJsonArray{QJsonObject{{"id",101},{"name","喝酒 R&B"}},QJsonObject{{"id",102},{"name","无关"}}}}},200,100};
            if (path == "/radio/playlist") {
                const auto id=request.query().queryItemValue("id").toInt();
                return {{{"playlist",QJsonObject{{"id",id},{"name",id==101?"喝酒 R&B":"儿童儿歌"},{"tags",QJsonArray{"夜晚"}}}},
                    {"songs",QJsonArray{track(11),track(12,"Avoid"),track(13),track(11),track(0)}}}};
            }
            if (path == "/assistant/artists") {
                const auto name = request.query().queryItemValue("keywords");
                return {{{"artists",QJsonArray{QJsonObject{{"id",name == "陶喆" ? 101 : 7763},{"name",name == "陶喆" ? "陶喆" : "邓紫棋"},{"aliases",QJsonArray()}}}}},200,100};
            }
            if (path == "/artist/tracks") return {{{"songs", QJsonArray()}, {"more",false}}};
            if (path == "/assistant/releases") return {{{"artist",QJsonObject{{"id",7763}}},{"checkedAt",QDateTime::currentMSecsSinceEpoch()},
                {"albums",QJsonArray{QJsonObject{{"id",201},{"name","新作"},{"publishTime",QDateTime::currentMSecsSinceEpoch()-86400000}}}}}};
            if (path == "/assistant/album") return {{{"album",QJsonObject{{"id",201},{"name","新作"},{"artistId",7763},
                {"publishTime",published}}},{"songs",QJsonArray{track(21,"邓紫棋")}}}};
            return {{{"error",QJsonObject{{"message","unexpected music route"}}}},404};
        };
        // Keep one frozen source timestamp across release list and detail.
        published=QDateTime::currentMSecsSinceEpoch()-86400000;
        client = new MusicTaskClient(nullptr,&backend);
    }
    ~Fixture() { delete client; }
    qint64 published=0;
    QJsonObject scene() const { return {{"id","scene-1"},{"skill","scene.research"},{"version",1},{"leaseId","lease-scene"},{"revision",1},
        {"input",QJsonObject{{"queries",QJsonArray{"喝酒 R&B"}},{"excludedArtists",QJsonArray{"Avoid"}},{"excludedIds",QJsonArray{13}}}}}; }
    QJsonObject release() const { return {{"id","release-1"},{"skill","releases.research"},{"version",1},{"leaseId","lease-release"},{"revision",1},
        {"input",QJsonObject{{"artist","邓紫棋"}}}}; }
    bool login(const QString &name="alice") {
        bool done=false;QString failure;
        backend.login(name,"test-password",client,[&](const QString &error){done=true;failure=error;});
        QElapsedTimer t;t.start();while(!done&&t.elapsed()<2000)QTest::qWait(5);
        return done&&failure.isEmpty();
    }
};
}
class MusicTaskClientTest : public QObject {
    Q_OBJECT
private slots:
    void auditWrongOriginalActionMustNotSettle() {
        Fixture f; QVERIFY(f.login());
        const auto device=f.client->deviceId();
        const QString request="route-original-control-request", taskId="task-original-control-owned";
        const auto path="/v1/tasks/requests/"+device+"/"+request;
        QJsonObject task{{"id",taskId},{"deviceId",device},{"revision",4},{"stateVersion",11},{"status","cancelled"},{"pendingSteps",QJsonArray()}};
        QJsonObject receipt{{"requestId",request},{"deviceId",device},{"taskId",taskId},{"status","recorded"},{"action","cancel"},{"expectedRevision",3}};
        f.app.overrides[path]={{{{"receipt",receipt},{"task",task}}}};
        QSignalSpy settled(f.client,&MusicTaskClient::requestSettled);
        QSignalSpy errors(f.client,&MusicTaskClient::error);
        f.client->observeRequest(request,{{"action","pause"},{"taskId",taskId},{"expectedRevision",3}},false);
        QTRY_VERIFY(settled.count()+errors.count()>0);
        QCOMPARE(settled.count(),0);
    }
    void auditDefiniteTaskRejectionMustRetainProtocolMetadata() {
        Fixture f; QVERIFY(f.login());
        f.app.overrides["/v1/tasks/task-original-control-owned/actions"]={{{{"error",QJsonObject{{"code","STALE_REVISION"},{"message","fixture rejects before commit"}}}},409}};
        bool called=false; QJsonObject metadata; QString error;
        f.backend.musicTaskAction("task-original-control-owned",
            {{"deviceId",f.client->deviceId()},{"requestId","route-original-control-request"},{"action","pause"},{"expectedRevision",1}},
            f.client,[&](const QJsonObject &data,const QString &failure){called=true;metadata=data;error=failure;});
        QTRY_VERIFY(called); QVERIFY(!error.isEmpty());
        QCOMPARE(metadata.value("httpStatus").toInt(),409);
        QVERIFY(metadata.value("completeHttp").toBool());
        QCOMPARE(metadata.value("errorCode").toString(),QString("STALE_REVISION"));
        QCOMPARE(metadata.size(),3);
    }
    void taskSubmissionRejectionAndUnknownStayDistinct_data() {
        QTest::addColumn<QString>("operation"); QTest::addColumn<int>("status");
        QTest::addColumn<QString>("code"); QTest::addColumn<bool>("truncated"); QTest::addColumn<bool>("definite");
        const QVector<QPair<int,QString>> failures{{400,"INVALID_TASK_REQUEST"},{409,"TASK_REVISION_CONFLICT"},
            {403,"AI_ACCOUNT_DISABLED"},{429,"AI_RATE_LIMITED"},{409,"IDEMPOTENCY_CONFLICT"},
            {409,"IDEMPOTENT_REPLAY"},{409,"TASK_WRITE_CONFLICT"},{409,"NEW_UNKNOWN_CODE"},
            {503,"MODEL_NOT_CONFIGURED"},{500,"INVALID_TASK_REQUEST"},{409,""}};
        for(const auto &operation:QStringList{"create","pause","adopt"}) {
            for(int i=0;i<failures.size();++i)
                QTest::newRow((operation+"-"+QString::number(i)).toUtf8())<<operation<<failures[i].first<<failures[i].second<<false<<(i<4);
            QTest::newRow((operation+"-truncated").toUtf8())<<operation<<409<<QString("TASK_REVISION_CONFLICT")<<true<<false;
        }
    }
    void taskSubmissionRejectionAndUnknownStayDistinct() {
        QFETCH(QString,operation); QFETCH(int,status); QFETCH(QString,code); QFETCH(bool,truncated); QFETCH(bool,definite);
        Fixture f; QVERIFY(f.login()); const auto device=f.client->deviceId();
        const QString taskId="task-known-rejection-owned", request="request-known-rejection-owned";
        const auto path=operation=="create"?QString("/v1/tasks"):"/v1/tasks/"+taskId+"/actions";
        if(operation!="create") {
            f.current={{"id",taskId},{"deviceId",device},{"revision",1},{"stateVersion",1},{"status","preview"},{"pendingSteps",QJsonArray()}};
            f.app.overrides["/v1/tasks/"+taskId]={{{{"task",f.current}}}};
            f.client->restore(taskId); QTRY_VERIFY(!f.client->busy());
        }
        FakeServer::Response refusal{{{"error",QJsonObject{{"code",code},{"message","fixture response"},{"private","must not escape"}}}},status};
        refusal.extraContentLength=truncated?40:0; f.app.overrides[path]={refusal};
        QSignalSpy rejected(f.client,&MusicTaskClient::requestRejected),settled(f.client,&MusicTaskClient::requestSettled),accepted(f.client,&MusicTaskClient::instructionAccepted);
        if(operation=="create")f.client->submit("fixture request",QString(),{},request);
        else if(operation=="adopt")f.client->adopt(request);
        else f.client->action(operation,{}, {},request);
        QTRY_COMPARE(rejected.count()+settled.count(),1);
        QCOMPARE(rejected.count(),definite?1:0); QCOMPARE(settled.count(),definite?0:1);
        if(!definite)QVERIFY(!settled.first().at(1).toBool());
        QCOMPARE(f.client->snapshot().contains("clientConnection"),!definite);
        QCOMPARE(accepted.count(),0); QVERIFY(!f.client->busy());
        QTest::qWait(50); QCOMPARE(f.app.count(path),1); QCOMPARE(f.music.requests.size(),0);
    }
    void routedOriginalFactsMustMatchBeforeSettling_data() {
        QTest::addColumn<QString>("fault");QTest::addColumn<bool>("selectTask");
        for(const auto &fault:QStringList{"valid-create","valid-pause","valid-turn","missing-action","wrong-action","missing-revision",
            "wrong-revision","fractional-revision","wrong-target","no-version-change","legacy-null","create-fractional-current","create-unsafe-current"})
            for(bool select:{false,true})QTest::newRow((fault+(select?"-select":"-independent")).toUtf8())<<fault<<select;
    }
    void routedOriginalFactsMustMatchBeforeSettling() {
        QFETCH(QString,fault);QFETCH(bool,selectTask);Fixture f;QVERIFY(f.login());
        const auto device=f.client->deviceId();const QString request="request-routed-original-owned",taskId="task-routed-original-owned";
        QJsonObject facts{{"action","pause"},{"taskId",taskId},{"expectedRevision",3}};
        QJsonObject receipt{{"requestId",request},{"deviceId",device},{"taskId",taskId},{"status","recorded"},{"action","pause"},{"expectedRevision",3}};
        QJsonObject task{{"id",taskId},{"deviceId",device},{"revision",4},{"stateVersion",11},{"status","paused"},{"pendingSteps",QJsonArray()}};
        if(fault=="valid-create"||fault.startsWith("create-")){facts={{"action","create"}};receipt.insert("action","create");receipt.remove("expectedRevision");}
        if(fault=="valid-turn"){facts.insert("action","turn");receipt.insert("action","turn");}
        if(fault=="missing-action")receipt.remove("action");
        if(fault=="wrong-action")receipt.insert("action","cancel");
        if(fault=="missing-revision")receipt.remove("expectedRevision");
        if(fault=="wrong-revision")receipt.insert("expectedRevision",2);
        if(fault=="fractional-revision")receipt.insert("expectedRevision",3.5);
        if(fault=="wrong-target"){receipt.insert("taskId","task-other-routed-owned");task.insert("id","task-other-routed-owned");}
        if(fault=="no-version-change")task.insert("revision",3);
        if(fault=="legacy-null"){receipt.remove("action");receipt.remove("expectedRevision");}
        if(fault=="create-fractional-current")task.insert("revision",1.5);
        if(fault=="create-unsafe-current")task.insert("revision",9007199254740992.);
        const auto path="/v1/tasks/requests/"+device+"/"+request;f.app.overrides[path]={{{{"receipt",receipt},{"task",task}}}};
        QSignalSpy settled(f.client,&MusicTaskClient::requestSettled),errors(f.client,&MusicTaskClient::error);
        f.client->observeRequest(request,facts,selectTask);QTRY_COMPARE(settled.count()+errors.count(),1);
        const bool valid=fault.startsWith("valid-");QCOMPARE(settled.count(),valid?1:0);
        QCOMPARE(f.app.count(path),1);QCOMPARE(f.app.count("/v1/tasks"),0);QCOMPARE(f.music.requests.size(),0);
        if(!selectTask||!valid)QVERIFY(f.client->snapshot().value("id").toString().isEmpty());
    }
    void adoptionReceiptRequiresOriginalActionAndRevision_data() {
        QTest::addColumn<QString>("fault");
        for(const auto &value:QStringList{"valid","missing-facts","wrong-action","wrong-revision","fractional-revision","wrong-task","wrong-device","unknown"})
            QTest::newRow(value.toUtf8())<<value;
    }
    void adoptionReceiptRequiresOriginalActionAndRevision() {
        QFETCH(QString,fault); Fixture f; QVERIFY(f.login());
        const auto device=f.client->deviceId(); const QString taskId="task-adoption-owned", requestId="adopt-original-owned";
        f.current={{"id",taskId},{"deviceId",device},{"revision",1},{"stateVersion",2},{"status","awaiting_apply"},{"pendingSteps",QJsonArray()}};
        f.app.overrides["/v1/tasks/"+taskId]={{{{"task",f.current}}}};
        f.client->restore(taskId); QTRY_COMPARE(f.client->snapshot().value("id").toString(),taskId);
        QJsonObject receipt{{"requestId",requestId},{"deviceId",device},{"taskId",taskId},{"status","recorded"},{"action","adopt"},{"expectedRevision",1}};
        QJsonValue replyTask=f.current;
        if(fault=="missing-facts"){receipt.remove("action");receipt.remove("expectedRevision");}
        if(fault=="wrong-action")receipt.insert("action","cancel");
        if(fault=="wrong-revision")receipt.insert("expectedRevision",2);
        if(fault=="fractional-revision")receipt.insert("expectedRevision",1.5);
        if(fault=="wrong-task")receipt.insert("taskId","task-another-owned");
        if(fault=="wrong-device")receipt.insert("deviceId","device-another-owned");
        if(fault=="unknown"){receipt={{"requestId",requestId},{"deviceId",device},{"status","unknown"}};replyTask=QJsonValue(QJsonValue::Null);}
        const auto path="/v1/tasks/requests/"+device+"/"+requestId;
        f.app.overrides[path]={{{{"receipt",receipt},{"task",replyTask}}}};
        QSignalSpy settled(f.client,&MusicTaskClient::requestSettled),errors(f.client,&MusicTaskClient::error);
        f.client->observeAdoptionRequest(requestId,taskId,1);
        QTRY_COMPARE(settled.count()+errors.count(),1);
        QCOMPARE(settled.count(),fault=="valid"?1:0); QCOMPARE(f.app.count(path),1);
        QCOMPARE(f.client->snapshot().value("id").toString(),taskId);
        QCOMPARE(f.app.count("/v1/tasks/"+taskId+"/actions"),0); QCOMPARE(f.music.requests.size(),0);
    }
    void lateAdoptionReceiptCannotSelectNewerVisibleTask() {
        Fixture f; QVERIFY(f.login()); const auto device=f.client->deviceId();
        const QString taskId="task-adoption-owned", newer="task-newer-owned", requestId="adopt-original-owned";
        f.current={{"id",taskId},{"deviceId",device},{"revision",1},{"stateVersion",2},{"status","awaiting_apply"},{"pendingSteps",QJsonArray()}};
        f.app.overrides["/v1/tasks/"+taskId]={{{{"task",f.current}}}};
        f.client->restore(taskId); QTRY_COMPARE(f.client->snapshot().value("id").toString(),taskId);
        const auto path="/v1/tasks/requests/"+device+"/"+requestId;
        FakeServer::Response delayed{{{"receipt",QJsonObject{{"requestId",requestId},{"deviceId",device},{"taskId",taskId},{"status","recorded"},{"action","adopt"},{"expectedRevision",1}}},{"task",f.current}}};
        delayed.delay=150; f.app.overrides[path]={delayed}; QSignalSpy settled(f.client,&MusicTaskClient::requestSettled);
        f.client->observeAdoptionRequest(requestId,taskId,1); QTRY_COMPARE(f.app.count(path),1);
        auto next=f.current;next.insert("id",newer);next.insert("status","preview");
        f.app.overrides["/v1/tasks/"+newer]={{{{"task",next}}}};f.client->restore(newer);
        QTRY_COMPARE(f.client->snapshot().value("id").toString(),newer); QTest::qWait(200);
        QCOMPARE(f.client->snapshot().value("id").toString(),newer); QCOMPARE(settled.count(),0);
    }
    void originalRequestReceiptRestoreIsObservationOnly_data() {
        QTest::addColumn<QString>("identifier");
        QTest::newRow("create-message") << QStringLiteral("message-original-create");
        QTest::newRow("task-control") << QStringLiteral("route-original-control-request");
    }
    void originalRequestReceiptRestoreIsObservationOnly() {
        QFETCH(QString, identifier);
        Fixture f; QVERIFY(f.login());
        const auto device = f.client->deviceId();
        const QString path = "/v1/tasks/requests/" + device + QLatin1Char('/') + identifier;
        f.current = {{"id", "task-request-receipt"}, {"deviceId", device}, {"revision", 4},
            {"stateVersion", 11}, {"status", "waiting_tools"}, {"pendingSteps", QJsonArray{f.scene()}}};
        QJsonObject facts{{"action","create"}};
        if(identifier.startsWith("route-")) facts={{"action","pause"},{"taskId","task-request-receipt"},{"expectedRevision",3}};
        QJsonObject response{{"receipt", QJsonObject{{"deviceId", device}, {"requestId", identifier},
            {"status", "recorded"}, {"taskId", "task-request-receipt"}, {"action","create"}}}, {"task", f.current}};
        if(identifier.startsWith("route-")) {
            auto receipt=response.value("receipt").toObject(); receipt.insert("action","pause");receipt.insert("expectedRevision",3);response.insert("receipt",receipt);
        }
        f.app.overrides[path] = {FakeServer::Response{response}};
        // 重建客户端模拟进程关闭；原操作不依赖当前内存中的发送记录。
        delete f.client; f.client = new MusicTaskClient(nullptr, &f.backend);
        QCOMPARE(f.client->deviceId(), device);
        QSignalSpy settled(f.client, &MusicTaskClient::requestSettled);
        QSignalSpy accepted(f.client, &MusicTaskClient::instructionAccepted);
        f.client->observeRequest(identifier,facts);
        QTRY_COMPARE(settled.count(), 1);
        QCOMPARE(settled.first().at(0).toString(), identifier);
        QVERIFY(settled.first().at(1).toBool());
        QCOMPARE(f.client->snapshot().value("id").toString(), QString("task-request-receipt"));
        QVERIFY(f.client->snapshot().value("clientObservationOnly").toBool());
        QCOMPARE(accepted.count(), 0);
        QVERIFY(f.app.last(path).headers.startsWith("GET "));
        QCOMPARE(f.app.count("/v1/tasks"), 0);
        QCOMPARE(f.app.count("/v1/tasks/task-request-receipt/actions"), 0);
        QCOMPARE(f.music.requests.size(), 0);
    }
    void originalRequestUnknownOrMalformedNeverSettles_data() {
        QTest::addColumn<QString>("fault");
        QTest::addColumn<bool>("selectTask");
        for (const auto &fault : QStringList{"unknown", "request", "device", "task-id", "task-device", "http"})
            for (const bool selectTask : {true, false})
                QTest::newRow((fault + (selectTask ? "-select" : "-receipt-only")).toUtf8().constData()) << fault << selectTask;
    }
    void originalRequestUnknownOrMalformedNeverSettles() {
        QFETCH(QString, fault);
        QFETCH(bool, selectTask);
        Fixture f; QVERIFY(f.login());
        const QString identifier = "message-original-unknown";
        const auto device = f.client->deviceId();
        const QString path = "/v1/tasks/requests/" + device + QLatin1Char('/') + identifier;
        QJsonObject receipt{{"deviceId", device}, {"requestId", identifier}, {"status", "recorded"},
            {"taskId", "task-request-receipt"}, {"action","create"}};
        QJsonObject task{{"id", "task-request-receipt"}, {"deviceId", device}, {"revision", 4},
            {"stateVersion", 11}, {"status", "preview"}, {"pendingSteps", QJsonArray()}};
        if (fault == "request") receipt.insert("requestId", "other-request-identifier");
        if (fault == "device") receipt.insert("deviceId", "other-device-identifier");
        if (fault == "task-id") task.insert("id", "different-task-identifier");
        if (fault == "task-device") task.insert("deviceId", "other-device-identifier");
        QJsonObject body{{"receipt", receipt}, {"task", task}};
        if (fault == "unknown") {
            receipt.insert("status", "unknown"); receipt.remove("taskId");
            body = {{"receipt", receipt}, {"task", QJsonValue::Null}};
        }
        f.app.overrides[path] = {{body, fault == "http" ? 503 : 200}};
        QSignalSpy settled(f.client, &MusicTaskClient::requestSettled);
        QSignalSpy errors(f.client, &MusicTaskClient::error);
        f.client->observeRequest(identifier,{{"action","create"}}, selectTask);
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(!f.client->busy());
        QCOMPARE(settled.count(), 0); QCOMPARE(f.app.count(path), 1);
        QCOMPARE(f.app.count("/v1/tasks"), 0); QCOMPARE(f.music.requests.size(), 0);
        QVERIFY(f.client->snapshot().value("id").toString().isEmpty());
    }
    void originalRequestReceiptRejectsInvalidIdentifiers() {
        Fixture f; QVERIFY(f.login());
        QSignalSpy errors(f.client, &MusicTaskClient::error);
        for (const auto &identifier : QStringList{"short", "../request", QString(101, QChar('x'))})
            f.client->observeRequest(identifier,{{"action","create"}});
        QCOMPARE(errors.count(), 3);
        for (const auto &request : f.app.requests)
            QVERIFY(!request.path().startsWith("/v1/tasks/requests/"));
    }
    void originalRequestLateReceiptCannotCrossAccounts() {
        Fixture f; QVERIFY(f.login());
        const QString identifier = "message-original-account";
        const auto device = f.client->deviceId();
        const QString path = "/v1/tasks/requests/" + device + QLatin1Char('/') + identifier;
        QJsonObject task{{"id", "task-request-receipt"}, {"deviceId", device}, {"revision", 1},
            {"stateVersion", 1}, {"status", "preview"}, {"pendingSteps", QJsonArray()}};
        const QJsonObject response{{"receipt", QJsonObject{{"deviceId", device}, {"requestId", identifier},
            {"status", "recorded"}, {"taskId", "task-request-receipt"}, {"action","create"}}}, {"task", task}};
        f.app.overrides[path] = {FakeServer::Response{response, 200, 150}};
        QSignalSpy settled(f.client, &MusicTaskClient::requestSettled);
        f.client->observeRequest(identifier,{{"action","create"}}); QTRY_COMPARE(f.app.count(path), 1);
        QVERIFY(f.login("bob")); QTest::qWait(180);
        QCOMPARE(settled.count(), 0); QVERIFY(f.client->snapshot().isEmpty());
        QCOMPARE(f.music.requests.size(), 0);
    }
    void oldRequestReceiptDoesNotReplaceNewTaskOrCancelItsTool() {
        Fixture f; QVERIFY(f.login());
        const QString identifier = "message-old-request-observation";
        const auto device = f.client->deviceId();
        const QString path = "/v1/tasks/requests/" + device + QLatin1Char('/') + identifier;
        auto step = f.scene(); step.insert("revision", 3); f.sequence = 8;
        f.current = {{"id", "task-new-current"}, {"deviceId", device}, {"revision", 3},
            {"stateVersion", 8}, {"status", "waiting_tools"}, {"pendingSteps", QJsonArray{step}}};
        const auto previous = f.app.handler;
        f.app.handler = [&f, previous](const FakeServer::Request &request) -> FakeServer::Response {
            if (request.path() == "/v1/tasks/task-new-current") return {{{"task", f.current}}};
            return previous(request);
        };
        f.client->restore("task-new-current"); QTRY_VERIFY(!f.client->busy());
        f.client->action("resume");
        QTRY_COMPARE(f.music.count("/radio/search"), 1);
        QVERIFY(!f.client->snapshot().value("clientObservationOnly").toBool());
        auto old = f.current; old.insert("id", "task-original-old"); old.insert("pendingSteps", QJsonArray());
        old.insert("status", "preview");
        const QJsonObject response{{"receipt", QJsonObject{{"deviceId", device}, {"requestId", identifier},
            {"status", "recorded"}, {"taskId", "task-original-old"}, {"action","create"}}}, {"task", old}};
        f.app.overrides[path] = {FakeServer::Response{response}};
        QSignalSpy settled(f.client, &MusicTaskClient::requestSettled);
        f.client->observeRequest(identifier,{{"action","create"}}, false);
        QTRY_COMPARE(settled.count(), 1);
        QCOMPARE(settled.first().at(0).toString(), identifier); QVERIFY(settled.first().at(1).toBool());
        QCOMPARE(f.client->snapshot().value("id").toString(), QString("task-new-current"));
        QVERIFY(!f.client->snapshot().value("clientObservationOnly").toBool());
        // 搜索及结果回执必须正常完成；旧事实查询不能删除正在执行的工具上下文。
        QTRY_COMPARE(f.results.size(), 1);
        QTRY_COMPARE(f.client->snapshot().value("status").toString(), QString("preview"));
        QCOMPARE(f.client->snapshot().value("id").toString(), QString("task-new-current"));
        QCOMPARE(f.app.count("/v1/tasks"), 0);
        QCOMPARE(f.app.count("/v1/tasks/task-original-old/actions"), 0);
    }
    void oldRequestReceiptCannotCrossAccountDuringIndependentObservation() {
        Fixture f; QVERIFY(f.login());
        const QString identifier = "message-old-account-observation";
        const auto device = f.client->deviceId();
        const QString path = "/v1/tasks/requests/" + device + QLatin1Char('/') + identifier;
        const QJsonObject old{{"id", "task-original-old"}, {"deviceId", device}, {"revision", 1},
            {"stateVersion", 1}, {"status", "preview"}, {"pendingSteps", QJsonArray()}};
        const QJsonObject response{{"receipt", QJsonObject{{"deviceId", device}, {"requestId", identifier},
            {"status", "recorded"}, {"taskId", "task-original-old"}, {"action","create"}}}, {"task", old}};
        f.app.overrides[path] = {FakeServer::Response{response, 200, 150}};
        QSignalSpy settled(f.client, &MusicTaskClient::requestSettled);
        f.client->observeRequest(identifier,{{"action","create"}}, false); QTRY_COMPARE(f.app.count(path), 1);
        QVERIFY(f.login("bob")); QTest::qWait(180);
        QCOMPARE(settled.count(), 0); QVERIFY(f.client->snapshot().isEmpty());
        QCOMPARE(f.music.requests.size(), 0);
    }
    void exactRestoreIsReadOnlyUntilExplicitResume() {
        Fixture f; QVERIFY(f.login());
        const QString id = QStringLiteral("task-exact-restore");
        f.current = {{"id", id}, {"deviceId", f.client->deviceId()}, {"revision", 1}, {"stateVersion", 1},
            {"status", "waiting_tools"}, {"pendingSteps", QJsonArray{f.scene()}}};
        const auto previous = f.app.handler;
        f.app.handler = [&f, previous, id](const FakeServer::Request &request) -> FakeServer::Response {
            if (request.path() == "/v1/tasks/" + id) return {{{"task", f.current}}};
            return previous(request);
        };
        QSignalSpy accepted(f.client, &MusicTaskClient::instructionAccepted);
        f.client->restore(id); QTRY_COMPARE(f.client->snapshot().value("id").toString(), id);
        QVERIFY(f.client->snapshot().value("clientObservationOnly").toBool());
        QCOMPARE(f.app.count("/v1/tasks/" + id), 1);
        QCOMPARE(f.app.count("/v1/tasks/latest/" + f.client->deviceId()), 0);
        QCOMPARE(f.app.count("/v1/tasks"), 0); QCOMPARE(f.app.count("/v1/tasks/" + id + "/actions"), 0);
        QCOMPARE(f.music.requests.size(), 0); QCOMPARE(f.results.size(), 0); QCOMPARE(accepted.count(), 0);
        auto *poll = f.client->findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly); QVERIFY(poll); poll->start(1);
        QTRY_COMPARE(f.app.count("/v1/tasks/" + id), 2);
        QCOMPARE(f.music.requests.size(), 0); QCOMPARE(f.results.size(), 0); QCOMPARE(accepted.count(), 0);
        f.client->action("resume");
        QTRY_COMPARE(f.results.size(), 1);
        QCOMPARE(f.app.count("/v1/tasks/" + id + "/actions"), 0);
        QVERIFY(!f.client->snapshot().value("clientObservationOnly").toBool());
        QVERIFY(f.music.count("/radio/search") >= 1); QCOMPARE(accepted.count(), 0);
    }
    void exactRestoreRejectsDifferentTaskResponse() {
        Fixture f; QVERIFY(f.login());
        const QString requested = QStringLiteral("task-requested-one");
        QJsonObject other{{"id", "task-returned-other"}, {"deviceId", f.client->deviceId()}, {"revision", 1},
            {"stateVersion", 1}, {"status", "preview"}, {"pendingSteps", QJsonArray()}};
        f.app.overrides["/v1/tasks/" + requested] = {{{{"task", other}}}};
        QSignalSpy errors(f.client, &MusicTaskClient::error);
        f.client->restore(requested); QTRY_VERIFY(!f.client->busy());
        QVERIFY(f.client->snapshot().value("id").toString().isEmpty()); QCOMPARE(errors.count(), 1);
        QCOMPARE(f.music.requests.size(), 0); QCOMPARE(f.app.count("/v1/tasks"), 0);
    }
    void exactRestoreRejectsInvalidIdsBeforeNetwork() {
        Fixture f; QVERIFY(f.login()); QSignalSpy errors(f.client, &MusicTaskClient::error);
        const QStringList invalid{QString(), "short", "bad/task-id", "task-id?x=1", QString(101, QChar('x'))};
        const int requests = f.app.requests.size();
        for (const auto &id : invalid) f.client->restore(id);
        QCOMPARE(errors.count(), invalid.size()); QCOMPARE(f.app.requests.size(), requests);
        QVERIFY(!f.client->busy());
    }
    void stableMessageIdCreatesTaskAndAcknowledgesAfterSavedResponse() {
        Fixture f; QVERIFY(f.login());
        QJsonObject saved{{"id", "task-1"}, {"deviceId", f.client->deviceId()}, {"revision", 1},
            {"stateVersion", 1}, {"status", "preview"}, {"pendingSteps", QJsonArray()}};
        f.app.overrides["/v1/tasks"] = {{{{"task", saved}}, 200, 120}};
        QSignalSpy accepted(f.client, &MusicTaskClient::instructionAccepted);
        const QString messageId = QStringLiteral("message-stable-create");
        f.client->submit("  睡前听歌  ", "喜欢爵士", {13}, messageId);
        QTRY_COMPARE(f.app.count("/v1/tasks"), 1); QCOMPARE(accepted.count(), 0);
        const auto body = f.app.last("/v1/tasks").json();
        QCOMPARE(body.value("requestId").toString(), messageId);
        QCOMPARE(body.value("instruction").toString(), QString("睡前听歌"));
        QTRY_COMPARE(accepted.count(), 1); QCOMPARE(accepted.first().first().toString(), QString("睡前听歌"));
        QCOMPARE(f.app.count("/v1/tasks"), 1); QCOMPARE(f.music.requests.size(), 0);
    }
    void stableMessageIdAmendsCurrentTask_data() {
        QTest::addColumn<bool>("directAction");
        QTest::newRow("submit-overload") << false;
        QTest::newRow("action-overload") << true;
    }
    void stableMessageIdAmendsCurrentTask() {
        QFETCH(bool, directAction);
        Fixture f; QVERIFY(f.login());
        f.current = {{"id", "task-1"}, {"deviceId", f.client->deviceId()}, {"revision", 3},
            {"stateVersion", 5}, {"status", "preview"}, {"pendingSteps", QJsonArray()}};
        f.client->restoreLatest(); QTRY_VERIFY(!f.client->busy());
        auto saved = f.current; saved.insert("revision", 4); saved.insert("stateVersion", 6);
        f.app.overrides["/v1/tasks/task-1/actions"] = {{{{"task", saved}}, 200, 120}};
        QSignalSpy accepted(f.client, &MusicTaskClient::instructionAccepted);
        const QString messageId = QStringLiteral("message-stable-turn");
        if (directAction) f.client->action("turn", "  少点人声  ", {{"preference", "喜欢爵士"}}, messageId);
        else f.client->submit("  少点人声  ", "喜欢爵士", {}, messageId);
        QTRY_COMPARE(f.app.count("/v1/tasks/task-1/actions"), 1); QCOMPARE(accepted.count(), 0);
        const auto body = f.app.last("/v1/tasks/task-1/actions").json();
        QCOMPARE(body.value("requestId").toString(), messageId); QCOMPARE(body.value("expectedRevision").toInt(), 3);
        QCOMPARE(body.value("action").toString(), QString("turn")); QCOMPARE(body.value("instruction").toString(), QString("少点人声"));
        QCOMPARE(body.value("preference").toString(), QString("喜欢爵士"));
        QTRY_COMPARE(accepted.count(), 1); QCOMPARE(accepted.first().first().toString(), QString("少点人声"));
        QCOMPARE(f.app.count("/v1/tasks"), 0); QCOMPARE(f.music.requests.size(), 0);
    }
    void stableMessageIdsRejectInvalidValuesAndUnconfirmedMutations() {
        Fixture f; QVERIFY(f.login()); QSignalSpy errors(f.client, &MusicTaskClient::error);
        const QStringList invalid{"short", "invalid/message", QString(101, QChar('x'))};
        for (const auto &id : invalid) f.client->submit("找歌", QString(), {}, id);
        QCOMPARE(errors.count(), invalid.size()); QCOMPARE(f.app.count("/v1/tasks"), 0);
        f.current = {{"id", "task-1"}, {"deviceId", f.client->deviceId()}, {"revision", 1},
            {"stateVersion", 1}, {"status", "preview"}, {"pendingSteps", QJsonArray()}};
        f.client->restoreLatest(); QTRY_VERIFY(!f.client->busy());
        for (const auto &id : invalid) f.client->action("turn", "找歌", {}, id);
        // 会话控制允许稳定消息 ID；采用及锁定仍只能走各自明确确认入口。
        f.client->action("adopt", QString(), {}, "message-valid-but-wrong-action");
        f.client->action("lock", QString(), {{"lockedIds",QJsonArray{11}}}, "message-valid-but-wrong-action");
        QCOMPARE(errors.count(), invalid.size() * 2 + 2); QCOMPARE(f.app.count("/v1/tasks/task-1/actions"), 0);
        QVERIFY(!f.client->busy());
    }
    void stableMessageIdControlsTaskOnlyAfterSavedResponse_data() {
        QTest::addColumn<QString>("actionName");
        for(const auto &name:QStringList{"resume","pause","cancel","undo"})
            QTest::newRow(name.toUtf8().constData()) << name;
    }
    void stableMessageIdControlsTaskOnlyAfterSavedResponse() {
        QFETCH(QString,actionName);
        Fixture f;QVERIFY(f.login());
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",3},
            {"stateVersion",5},{"status","paused"},{"pendingSteps",QJsonArray()}};
        f.client->restoreLatest();QTRY_VERIFY(!f.client->busy());
        auto saved=f.current;saved.insert("revision",4);saved.insert("stateVersion",6);
        saved.insert("status",actionName=="cancel"?"cancelled":"preview");
        f.app.overrides["/v1/tasks/task-1/actions"]={{{{"task",saved}},200,120}};
        QSignalSpy settled(f.client,&MusicTaskClient::requestSettled);
        QSignalSpy replanned(f.client,&MusicTaskClient::instructionAccepted);
        const QString messageId="message-stable-control-"+actionName;
        f.client->action(actionName,"控制操作不应转成模型指令",{},messageId);
        QTRY_COMPARE(f.app.count("/v1/tasks/task-1/actions"),1);QCOMPARE(settled.count(),0);
        const auto request=f.app.last("/v1/tasks/task-1/actions").json();
        QCOMPARE(request.value("requestId").toString(),messageId);
        QCOMPARE(request.value("action").toString(),actionName);
        QCOMPARE(request.value("expectedRevision").toInt(),3);
        QVERIFY(!request.contains("instruction"));
        QTRY_COMPARE(settled.count(),1);
        QCOMPARE(settled.first().at(0).toString(),messageId);QVERIFY(settled.first().at(1).toBool());
        QCOMPARE(replanned.count(),0);QCOMPARE(f.app.count("/v1/tasks"),0);QCOMPARE(f.music.requests.size(),0);
    }
    void rejectedOrInvalidSnapshotsNeverAcknowledgeInstruction_data() {
        QTest::addColumn<bool>("transportFailure");
        QTest::newRow("http-rejection") << true;
        QTest::newRow("invalid-device-success-body") << false;
    }
    void rejectedOrInvalidSnapshotsNeverAcknowledgeInstruction() {
        QFETCH(bool, transportFailure);
        Fixture f; QVERIFY(f.login());
        f.current = {{"id", "task-1"}, {"deviceId", f.client->deviceId()}, {"revision", 1},
            {"stateVersion", 1}, {"status", "preview"}, {"pendingSteps", QJsonArray()}};
        f.client->restoreLatest(); QTRY_VERIFY(!f.client->busy());
        auto invalid = f.current; invalid.insert("deviceId", "a-different-device");
        if (transportFailure) f.app.overrides["/v1/tasks/task-1/actions"] = {{{{"error", QJsonObject{{"message", "fixture rejected"}}}}, 503}};
        else f.app.overrides["/v1/tasks/task-1/actions"] = {{{{"task", invalid}}}};
        QSignalSpy errors(f.client, &MusicTaskClient::error);
        QSignalSpy accepted(f.client, &MusicTaskClient::instructionAccepted);
        f.client->action("turn", "修改但未被确认", {}, "message-unconfirmed");
        QTRY_COMPARE(errors.count(), 1); QCOMPARE(accepted.count(), 0);
        QCOMPARE(f.app.count("/v1/tasks/task-1/actions"), 1); QCOMPARE(f.music.requests.size(), 0);
    }
    void eventStreamParsesFragmentedCrLfAndHeartbeatWithAuthenticatedHeaders_data() {
        QTest::addColumn<bool>("numericAt");
        QTest::newRow("iso-timestamp") << false;
        QTest::newRow("backend-epoch-milliseconds") << true;
    }
    void eventStreamParsesFragmentedCrLfAndHeartbeatWithAuthenticatedHeaders() {
        QFETCH(bool, numericAt);
        Fixture f; QVERIFY(f.login());
        auto event = progressEvent(6);
        if (numericAt) event.insert("at", double(QDateTime::currentMSecsSinceEpoch()));
        auto json = QJsonDocument(event).toJson(QJsonDocument::Compact);
        FakeServer::Response response; response.stream = true;
        response.chunks = {": keepalive\r", "\n\r\nid: 6\r\nevent: progress\r\ndata: " + json.left(23),
            json.mid(23) + "\r\n\r\n"};
        f.app.overrides["/v1/tasks/task-1/events"] = {response};
        auto *context = new QObject(f.client); int received = 0, closed = 0;
        QJsonObject observed;
        f.backend.musicTaskEvents("task-1", 5, context,
            [&](const QJsonObject &value, const QString &) { ++received; observed = value; },
            [&](int, const QString &) { ++closed; });
        QTRY_COMPARE(received, 1); QCOMPARE(observed, event); QCOMPARE(closed, 0);
        const auto request = f.app.last("/v1/tasks/task-1/events");
        QVERIFY(request.headers.contains("Authorization: Bearer session-for-test-"));
        QVERIFY(request.headers.contains("Last-Event-ID: 5"));
        QVERIFY(!request.target.contains("session-for-test-"));
        QVERIFY(!request.headers.contains("isolated-token")); QVERIFY(!f.client->busy());
        delete context; QTRY_COMPARE(f.app.activeStreams(), 0); QCOMPARE(closed, 0);
    }
    void malformedOrWrongTaskProgressNeverReachesTheClient_data() {
        QTest::addColumn<QByteArray>("payload"); QTest::addColumn<QByteArray>("contentType");
        auto wrong = progressEvent(2); wrong.insert("taskId", "another-users-task");
        QTest::newRow("wrong-task") << eventFrame(wrong) << QByteArray("text/event-stream");
        QTest::newRow("invalid-json") << QByteArray("event: progress\ndata: {\n\n") << QByteArray("text/event-stream");
        QTest::newRow("oversized-frame") << QByteArray("event: progress\ndata: ") + QByteArray(17000, 'x') + "\n\n" << QByteArray("text/event-stream");
        auto fractional = progressEvent(2); fractional.insert("stateVersion", 2.5);
        QTest::newRow("fractional-version") << eventFrame(fractional) << QByteArray("text/event-stream");
        QTest::newRow("wrong-content-type") << eventFrame(progressEvent(2)) << QByteArray("text/html");
        QTest::newRow("mismatched-event-id") << QByteArray("id: 3\nevent: progress\ndata: ") +
            QJsonDocument(progressEvent(2)).toJson(QJsonDocument::Compact) + "\n\n" << QByteArray("text/event-stream");
    }
    void malformedOrWrongTaskProgressNeverReachesTheClient() {
        QFETCH(QByteArray, payload); QFETCH(QByteArray, contentType);
        Fixture f; QVERIFY(f.login());
        FakeServer::Response response; response.stream = true; response.chunks = {payload}; response.contentType = contentType;
        f.app.overrides["/v1/tasks/task-1/events"] = {response};
        QObject context; int received = 0, closed = 0; QString failure;
        f.backend.musicTaskEvents("task-1", 1, &context,
            [&](const QJsonObject &, const QString &) { ++received; },
            [&](int, const QString &error) { ++closed; failure = error; });
        QTRY_COMPARE(closed, 1); QCOMPARE(received, 0); QVERIFY(!failure.isEmpty());
        QTRY_COMPARE(f.app.activeStreams(), 0); QVERIFY(f.backend.authenticated());
    }
    void heartbeatKeepsStreamAliveAndSilenceTriggersReconnectSignal() {
        Fixture f; QVERIFY(f.login());
        FakeServer::Response response; response.stream = true;
        response.chunkDelay = 70; response.chunks = {": keepalive\n\n", ": keepalive\n\n", ": keepalive\n\n"};
        f.app.overrides["/v1/tasks/task-1/events"] = {response};
        QObject context; int received = 0, closed = 0;
        f.backend.musicTaskEvents("task-1", 0, &context,
            [&](const QJsonObject &, const QString &) { ++received; },
            [&](int, const QString &) { ++closed; }, 160);
        QTest::qWait(270); QCOMPARE(closed, 0); QCOMPARE(received, 0);
        QTRY_COMPARE(closed, 1); QTRY_COMPARE(f.app.activeStreams(), 0);
    }
    void progressBurstIsCoalescedAndNeverExecutesToolsFromAnEvent() {
        Fixture f; QVERIFY(f.login());
        f.current = {{"id", "task-1"}, {"deviceId", f.client->deviceId()}, {"revision", 1}, {"stateVersion", 1},
            {"status", "planning"}, {"pendingSteps", QJsonArray()}};
        FakeServer::Response response; response.stream = true; response.chunks = {eventFrame(progressEvent(1, "understanding", "planning"))};
        f.app.overrides["/v1/tasks/task-1/events"] = {response};
        f.client->restoreLatest(); QTRY_COMPARE(f.app.activeStreams(), 1); QVERIFY(!f.client->busy());
        f.current.insert("stateVersion", 4); f.current.insert("status", "composing");
        auto latest = progressEvent(4, "composing", "composing");
        latest.insert("pendingSteps", QJsonArray{f.scene()}); // Events cannot authorize local research.
        f.app.writeEvent(eventFrame(progressEvent(3)) + eventFrame(latest) + eventFrame(progressEvent(2)) + eventFrame(latest));
        QTRY_COMPARE(f.client->snapshot().value("progress").toObject().value("stateVersion").toInt(), 4);
        QTRY_COMPARE(f.client->snapshot().value("stateVersion").toInt(), 4);
        QCOMPARE(f.app.count("/v1/tasks/task-1"), 1); QCOMPARE(f.music.requests.size(), 0);
        QCOMPARE(f.results.size(), 0); QVERIFY(!f.client->busy());
        QCOMPARE(f.client->snapshot().value("progress").toObject().value("stage").toString(), QString("composing"));
        f.client->detach(); QTRY_COMPARE(f.app.activeStreams(), 0);
        f.app.writeEvent(eventFrame(progressEvent(99))); QTest::qWait(150); QVERIFY(f.client->snapshot().isEmpty());
    }
    void oldBackendFallsBackToPollingWithoutRepeatedUnsupportedRequests() {
        Fixture f; QVERIFY(f.login()); QSignalSpy errors(f.client, &MusicTaskClient::error);
        f.current = {{"id", "task-1"}, {"deviceId", f.client->deviceId()}, {"revision", 1}, {"stateVersion", 1},
            {"status", "planning"}, {"pendingSteps", QJsonArray()}};
        f.client->restoreLatest(); QTRY_COMPARE(f.app.count("/v1/tasks/task-1/events"), 1);
        f.current.insert("stateVersion", 2); f.current.insert("status", "preview");
        QTRY_COMPARE(f.client->snapshot().value("status").toString(), QString("preview"));
        QCOMPARE(f.app.count("/v1/tasks/task-1/events"), 1); QCOMPARE(errors.size(), 0); QCOMPARE(f.music.requests.size(), 0);
    }
    void repeatedProgressDoesNotRestartAnAlreadyLeasedCatalogTool() {
        Fixture f; QVERIFY(f.login());
        f.current = {{"id", "task-1"}, {"deviceId", f.client->deviceId()}, {"revision", 1}, {"stateVersion", 1},
            {"status", "waiting_tools"}, {"pendingSteps", QJsonArray{f.scene()}}};
        FakeServer::Response stream; stream.stream = true;
        stream.chunks = {eventFrame(progressEvent(1, "searching", "waiting_tools"))};
        f.app.overrides["/v1/tasks/task-1/events"] = {stream};
        f.music.overrides["/radio/search"] = {{{{"playlists", QJsonArray{QJsonObject{{"id",101},{"name","喝酒 R&B"}}}}}, 200, 450}};
        f.client->restoreLatest(); QTRY_COMPARE(f.music.count("/radio/search"), 1);
        f.current.insert("stateVersion", 2); f.sequence = 2;
        const auto progress = progressEvent(2, "searching", "waiting_tools");
        f.app.writeEvent(eventFrame(progress) + eventFrame(progress));
        QTRY_COMPARE(f.app.count("/v1/tasks/task-1"), 1);
        QTRY_COMPARE(f.results.size(), 1); QTRY_COMPARE(f.client->snapshot().value("status").toString(), QString("preview"));
        QCOMPARE(f.music.count("/radio/search"), 1); QCOMPARE(f.music.count("/radio/playlist"), 1);
        QTRY_COMPARE(f.app.activeStreams(), 0);
        QCOMPARE(f.results.first().toObject().value("catalogContext").toString(), NeteaseClient::catalogCacheContext());
    }
    void delayedSnapshotAndNewerEventsUseOneGetThenCatchUpWithoutLosingTheFinalVersion() {
        Fixture f; QVERIFY(f.login());
        f.current = {{"id", "task-1"}, {"deviceId", f.client->deviceId()}, {"revision", 1}, {"stateVersion", 1},
            {"status", "planning"}, {"pendingSteps", QJsonArray()}};
        FakeServer::Response stream; stream.stream = true;
        stream.chunks = {eventFrame(progressEvent(1, "understanding", "planning"))};
        f.app.overrides["/v1/tasks/task-1/events"] = {stream};
        f.client->restoreLatest();
        QTRY_COMPARE(f.client->snapshot().value("progress").toObject().value("stateVersion").toInt(), 1);
        f.current.insert("stateVersion", 2); f.current.insert("status", "researching");
        f.app.overrides["/v1/tasks/task-1"] = {{{{"task", f.current}}, 200, 400}};
        f.app.writeEvent(eventFrame(progressEvent(2)));
        QTRY_COMPARE(f.app.count("/v1/tasks/task-1"), 1);
        QSignalSpy busy(f.client, &MusicTaskClient::busyChanged);
        f.current.insert("stateVersion", 4); f.current.insert("status", "composing");
        f.app.writeEvent(eventFrame(progressEvent(3)) + eventFrame(progressEvent(4, "composing", "composing")));
        QTest::qWait(180); QCOMPARE(f.app.count("/v1/tasks/task-1"), 1);
        QTRY_COMPARE_WITH_TIMEOUT(f.client->snapshot().value("stateVersion").toInt(), 4, 2000);
        QCOMPARE(f.app.count("/v1/tasks/task-1"), 2); QCOMPARE(busy.size(), 0);
        QCOMPARE(f.client->snapshot().value("progress").toObject().value("stateVersion").toInt(), 4);
        // Healthy SSE reduces the old one-second GET load, while a dropped
        // stream restores the original quick polling fallback.
        QTest::qWait(1200); QCOMPARE(f.app.count("/v1/tasks/task-1"), 2);
        f.current.insert("stateVersion", 5);
        for (const auto &socket : f.app.eventSockets) if (socket) socket->disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(f.client->snapshot().value("stateVersion").toInt(), 5, 2000);
        QCOMPARE(f.music.requests.size(), 0);
    }
    void terminalProgressFetchesFinalSnapshotAndStopsReconnects() {
        Fixture f; QVERIFY(f.login());
        f.current = {{"id", "task-1"}, {"deviceId", f.client->deviceId()}, {"revision", 1}, {"stateVersion", 1},
            {"status", "composing"}, {"pendingSteps", QJsonArray()}};
        FakeServer::Response stream; stream.stream = true;
        f.app.overrides["/v1/tasks/task-1/events"] = {stream};
        f.client->restoreLatest(); QTRY_COMPARE(f.app.activeStreams(), 1);
        f.current.insert("stateVersion", 2); f.current.insert("status", "preview");
        f.app.writeEvent(eventFrame(progressEvent(2, "ready", "preview")));
        QTRY_COMPARE(f.client->snapshot().value("status").toString(), QString("preview"));
        QTRY_COMPARE(f.app.activeStreams(), 0); QVERIFY(!f.client->busy());
        const int observations = f.app.count("/v1/tasks/task-1");
        QTest::qWait(1200); QCOMPARE(f.app.count("/v1/tasks/task-1/events"), 1);
        QCOMPARE(f.app.count("/v1/tasks/task-1"), observations); QCOMPARE(f.music.requests.size(), 0);
    }
    void eventReconnectUsesLastObservedVersionWhilePollingContinues() {
        Fixture f; QVERIFY(f.login());
        f.current = {{"id", "task-1"}, {"deviceId", f.client->deviceId()}, {"revision", 1}, {"stateVersion", 1},
            {"status", "planning"}, {"pendingSteps", QJsonArray()}};
        FakeServer::Response first; first.stream = true; first.closeStream = true;
        first.chunks = {eventFrame(progressEvent(2))};
        FakeServer::Response second; second.stream = true; second.chunks = {": keepalive\n\n"};
        f.app.overrides["/v1/tasks/task-1/events"] = {first, second};
        f.client->restoreLatest(); QTRY_COMPARE(f.client->snapshot().value("progress").toObject().value("stateVersion").toInt(), 2);
        f.current.insert("stateVersion", 2); f.current.insert("status", "researching");
        QTRY_COMPARE(f.app.count("/v1/tasks/task-1/events"), 2);
        QVERIFY(f.app.last("/v1/tasks/task-1/events").headers.contains("Last-Event-ID: 2"));
        QVERIFY(f.app.count("/v1/tasks/task-1") >= 1); QVERIFY(!f.client->busy());
        f.client->action("pause"); QTRY_COMPARE(f.client->snapshot().value("status").toString(), QString("paused"));
        QTRY_COMPARE(f.app.activeStreams(), 0);
        const int count = f.app.count("/v1/tasks/task-1/events"); QTest::qWait(1200);
        QCOMPARE(f.app.count("/v1/tasks/task-1/events"), count);
    }
    void accountSwitchAbortsEventsAndRejectsLateProgress() {
        Fixture f; QVERIFY(f.login());
        f.current = {{"id", "task-1"}, {"deviceId", f.client->deviceId()}, {"revision", 1}, {"stateVersion", 1},
            {"status", "planning"}, {"pendingSteps", QJsonArray()}};
        FakeServer::Response response; response.stream = true; response.chunks = {eventFrame(progressEvent(1))};
        f.app.overrides["/v1/tasks/task-1/events"] = {response};
        f.client->restoreLatest(); QTRY_COMPARE(f.app.activeStreams(), 1);
        QVERIFY(f.login("bob")); QTRY_COMPARE(f.app.activeStreams(), 0); QVERIFY(f.client->snapshot().isEmpty());
        f.app.writeEvent(eventFrame(progressEvent(20))); QTest::qWait(150); QVERIFY(f.client->snapshot().isEmpty());
    }
    void authenticationExpiryEventImmediatelyClosesLocalSession() {
        Fixture f; QVERIFY(f.login());
        FakeServer::Response response; response.stream = true;
        response.chunks = {"event: auth_expired\ndata: {\"code\":\"AUTH_REQUIRED\"}\n\n"};
        f.app.overrides["/v1/tasks/task-1/events"] = {response};
        QObject context; int received = 0;
        f.backend.musicTaskEvents("task-1", 0, &context,
            [&](const QJsonObject &, const QString &) { ++received; }, [](int, const QString &) {});
        QTRY_VERIFY(!f.backend.authenticated()); QTRY_COMPARE(f.app.activeStreams(), 0); QCOMPARE(received, 0);
    }
    void unauthenticatedDoesNotContactAnyService() {
        Fixture f; QSignalSpy errors(f.client,&MusicTaskClient::error);
        f.client->submit("test");f.client->restoreLatest();
        QCOMPARE(errors.size(),2);QCOMPARE(f.app.requests.size(),0);QCOMPARE(f.music.requests.size(),0);
    }
    void twoLanesUseIndependentContextsAndVerifiedMetadata() {
        Fixture f;QVERIFY(f.login());
        // Override with exactly the frozen publication timestamp.
        f.music.overrides["/assistant/releases"]={{{{"artist",QJsonObject{{"id",7763}}},{"checkedAt",QDateTime::currentMSecsSinceEpoch()},
            {"albums",QJsonArray{QJsonObject{{"id",201},{"name","新作"},{"publishTime",f.published}}}}}}};
        f.client->submit("喝酒，带上邓紫棋的新歌");
        QTRY_COMPARE(f.music.count("/radio/search"),1);QTRY_COMPARE(f.music.count("/assistant/artists"),1);
        QTRY_COMPARE(f.results.size(),2);QTRY_COMPARE(f.client->snapshot().value("status").toString(),QString("preview"));
        QJsonArray tracks;
        bool verifiedArtist=false;
        for(const auto &result:f.results) {
            const auto metadata=result.toObject().value("result").toObject();
            if(metadata.contains("resolvedArtist")) {
                verifiedArtist=true;
                QCOMPARE(metadata.value("resolvedArtist").toObject().value("id").toInt(),7763);
            }
            for(const auto &song:metadata.value("tracks").toArray())tracks.append(song);
        }
        QVERIFY(verifiedArtist);
        QCOMPARE(tracks.size(),2);QSet<qint64> ids;for(const auto&song:tracks)ids.insert(qint64(song.toObject().value("id").toDouble()));
        QVERIFY(ids.contains(11));QVERIFY(ids.contains(21));
        for(const auto&request:f.app.requests) {
            QVERIFY(!request.body.contains("isolated-token"));QVERIFY(!request.body.contains("Cookie"));
            QVERIFY(!request.headers.contains("X-MusicPlayer-Token"));
        }
        QCOMPARE(f.music.count("/radio/playlist"),2);
    }
    void ambiguousArtistReturnsQuestionWithoutSelectingFirst() {
        Fixture f;QVERIFY(f.login());
        f.music.overrides["/assistant/artists"]={{{{"artists",QJsonArray{QJsonObject{{"id",1},{"name","邓紫棋"}},QJsonObject{{"id",2},{"name","邓紫棋"}}}}}}};
        f.client->submit("查新作");QTRY_COMPARE(f.results.size(),2);
        bool question=false;
        for(const auto&body:f.results){const auto result=body.toObject().value("result").toObject();
            if(!result.value("question").toString().isEmpty()){question=true;QCOMPARE(result.value("artists").toArray().size(),2);QVERIFY(!result.contains("resolvedArtist"));}}
        QVERIFY(question);QCOMPARE(f.music.count("/assistant/releases"),0);
    }
    void uniqueExactArtistOrAliasAmongOtherResultsContinues_data() {
        QTest::addColumn<QString>("canonicalName");
        QTest::addColumn<QString>("alias");
        QTest::newRow("unique-exact-name") << QString("邓紫棋") << QString();
        QTest::newRow("unique-catalog-alias") << QString("G.E.M.邓紫棋") << QString("邓紫棋");
    }
    void uniqueExactArtistOrAliasAmongOtherResultsContinues() {
        QFETCH(QString,canonicalName);QFETCH(QString,alias);
        Fixture f;QVERIFY(f.login());
        QJsonArray aliases;if(!alias.isEmpty())aliases.append(alias);
        f.music.overrides["/assistant/artists"]={{{{"artists",QJsonArray{
            QJsonObject{{"id",9999},{"name","邓紫棋歌迷"},{"aliases",QJsonArray{}}},
            QJsonObject{{"id",7763},{"name",canonicalName},{"aliases",aliases}}}}}}};
        f.music.overrides["/assistant/releases"]={{{{"artist",QJsonObject{{"id",7763},{"name",canonicalName}}},
            {"checkedAt",QDateTime::currentMSecsSinceEpoch()},
            {"albums",QJsonArray{QJsonObject{{"id",201},{"name","新作"},{"publishTime",f.published}}}}}}};
        f.client->submit("查询邓紫棋新歌");QTRY_COMPARE(f.results.size(),2);
        QCOMPARE(f.music.count("/assistant/releases"),1);
        bool verified=false;
        for(const auto &body:f.results) {
            const auto result=body.toObject().value("result").toObject();
            QVERIFY(result.value("question").toString().isEmpty());
            if(result.contains("resolvedArtist")) {
                verified=true;
                QCOMPARE(result.value("resolvedArtist").toObject().value("id").toInt(),7763);
                QCOMPARE(result.value("resolvedArtist").toObject().value("name").toString(),canonicalName);
            }
        }
        QVERIFY(verified);
    }
    void conflictingExactCatalogAliasesStillRequireChoice() {
        Fixture f;QVERIFY(f.login());
        f.music.overrides["/assistant/artists"]={{{{"artists",QJsonArray{
            QJsonObject{{"id",1},{"name","G.E.M.邓紫棋"},{"aliases",QJsonArray{"邓紫棋"}}},
            QJsonObject{{"id",2},{"name","另一位歌手"},{"aliases",QJsonArray{"邓紫棋"}}}}}}}};
        f.client->submit("查询邓紫棋新歌");QTRY_COMPARE(f.results.size(),2);
        QCOMPARE(f.music.count("/assistant/releases"),0);
        bool question=false;
        for(const auto &body:f.results) {
            const auto result=body.toObject().value("result").toObject();
            if(!result.value("question").toString().isEmpty()) {
                question=true;QCOMPARE(result.value("artists").toArray().size(),2);
                QVERIFY(!result.contains("resolvedArtist"));
            }
        }
        QVERIFY(question);
    }
    void continuousChineseSearchKeepsActualSourceCandidatesForObserver_data() {
        QTest::addColumn<QString>("query");QTest::addColumn<QString>("sourceName");
        QTest::newRow("reading-artist-no-spaces") << QString("适合读书的陶喆歌曲") << QString("陶喆｜不打扰阅读的温柔");
        QTest::newRow("drinking-synonym") << QString("微醺华语R&B") << QString("深夜小酌，节奏蓝调");
    }
    void continuousChineseSearchKeepsActualSourceCandidatesForObserver() {
        QFETCH(QString,query);QFETCH(QString,sourceName);
        Fixture f;QVERIFY(f.login());auto step=f.scene();step.insert("input",QJsonObject{{"queries",QJsonArray{query}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        f.music.overrides["/radio/search"]={{{{"playlists",QJsonArray{QJsonObject{{"id",101},{"name",sourceName}}}}}}};
        f.music.overrides["/radio/playlist"]={{{{"playlist",QJsonObject{{"id",101},{"name",sourceName},{"tags",QJsonArray{"华语","R&B"}}}},
            {"songs",QJsonArray{track(31,"陶喆")}}}}};
        f.client->submit(query);QTRY_COMPARE(f.results.size(),1);
        const auto result=f.results.first().toObject().value("result").toObject();
        QCOMPARE(result.value("tracks").toArray().size(),1);
        QCOMPARE(result.value("sources").toArray().first().toObject().value("name").toString(),sourceName);
        QCOMPARE(result.value("tracks").toArray().first().toObject().value("sourceTags").toArray(),QJsonArray({"华语","R&B"}));
        QVERIFY(!result.contains("relevanceSatisfied"));
        QCOMPARE(f.music.count("/radio/search"),1);QCOMPARE(f.music.count("/radio/playlist"),1);
    }
    void requiredArtistUsesRealCreditsAndRejectsTitleOnlyImpersonation_data() {
        QTest::addColumn<bool>("nestedBrief");
        QTest::newRow("explicit-tool-input") << false;QTest::newRow("brief-fallback") << true;
    }
    void requiredArtistUsesRealCreditsAndRejectsTitleOnlyImpersonation() {
        QFETCH(bool,nestedBrief);
        Fixture f;QVERIFY(f.login());auto step=f.scene();
        QJsonObject input{{"queries",QJsonArray{"陶喆读书"}}};
        const QJsonObject required{{"requiredArtist","陶喆"},{"artistOnly",true}};
        if(nestedBrief)input.insert("brief",required);else for(auto it=required.begin();it!=required.end();++it)input.insert(it.key(),it.value());
        step.insert("input",input);
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        f.music.overrides["/radio/search"]={{{{"playlists",QJsonArray{QJsonObject{{"id",101},{"name","读书时光"}}}}}}};
        auto cover=track(34,"翻唱者");cover.insert("title","陶喆翻唱合集");
        f.music.overrides["/radio/playlist"]={{{{"playlist",QJsonObject{{"id",101},{"name","读书时光"},{"tags",QJsonArray{"华语"}}}},
            {"songs",QJsonArray{track(31,"陶喆"),track(32,"周杰伦"),track(33,"陶喆 / 周杰伦"),cover,
                track(35,"陶喆 feat. Guest"),track(36,"Guest、陶喆"),track(37,"陶-喆")}}}}};
        f.client->submit("读书听陶喆40分钟");QTRY_COMPARE(f.results.size(),1);
        const auto result=f.results.first().toObject().value("result").toObject();QSet<qint64> ids;
        for(const auto &song:result.value("tracks").toArray())ids.insert(qint64(song.toObject().value("id").toDouble()));
        QCOMPARE(ids,QSet<qint64>({31,33,35,36}));
        QCOMPARE(f.music.count("/radio/search"),1);QCOMPARE(f.music.count("/radio/playlist"),1);
    }
    void artistSongSearchFillsFortyMinutesWithoutDependingOnPlaylistTopics() {
        Fixture f;QVERIFY(f.login());auto step=f.scene();
        step.insert("input",QJsonObject{{"queries",QJsonArray{"读书陶喆"}},{"requiredArtist","陶喆"},{"artistOnly",true},
            {"brief",QJsonObject{{"targetMinutes",40}}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        QJsonArray rows{track(90,"周杰伦")};
        for(int i=0;i<40;++i)rows.append(track(100+i,"陶喆"));
        f.music.overrides["/artist/tracks"]={{{{"songs",rows},{"total",41}}}};
        f.client->submit("我最近在读书，想听陶喆，帮我安排40分钟");QTRY_COMPARE(f.results.size(),1);
        const auto result=f.results.first().toObject().value("result").toObject();
        const auto tracks=result.value("tracks").toArray();QCOMPARE(tracks.size(),29);
        for(const auto &song:tracks){QCOMPARE(song.toObject().value("artist").toString(),QString("陶喆"));
            QVERIFY(song.toObject().value("sourceTags").toArray().isEmpty());}
        QCOMPARE(result.value("sources").toArray().first().toObject().value("name").toString(),QString("歌手作品：陶喆"));
        QCOMPARE(f.music.count("/artist/tracks"),1);QCOMPARE(f.music.count("/radio/search"),0);QCOMPARE(f.music.count("/radio/playlist"),0);
        const auto query=f.music.last("/artist/tracks").query();QCOMPARE(query.queryItemValue("artistId"),QString("101"));
        QCOMPARE(query.queryItemValue("limit"),QString("30"));
    }
    void walkingArtistDurationReadsSceneSourcesAndChecksActualCredits() {
        Fixture f; QVERIFY(f.login()); auto step = f.scene();
        step.insert("input", QJsonObject{{"requiredArtist","邓紫棋"},{"artistOnly",true},
            {"artistId",99999},{"brief",QJsonObject{{"scene","散步"},{"targetMinutes",30}}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        const auto defaultHandler = f.music.handler;
        f.music.handler=[defaultHandler](const FakeServer::Request &r)->FakeServer::Response {
            if (r.path()=="/assistant/artists") return {{{"artists",QJsonArray{QJsonObject{
                {"id",7763},{"name","G.E.M.邓紫棋"},{"aliases",QJsonArray{"邓紫棋"}}}}}}};
            if (r.path()=="/radio/search") return {{{"playlists",QJsonArray{
                QJsonObject{{"id",101},{"name","经典作品"}},QJsonObject{{"id",102},{"name","散步时刻"}}}}}};
            if (r.path()=="/radio/playlist") {
                const int id=r.query().queryItemValue("id").toInt();
                if (r.query().queryItemValue("artistId")!="7763" || r.query().hasQueryItem("artist"))
                    return {{{"error","artist ID filter missing"}},400};
                QJsonArray rows;
                for (int i=0;i<12;++i) rows.append(track(500+i,"G.E.M.邓紫棋"));
                auto imposter=track(800,"G.E.M.邓紫棋");
                imposter.insert("artists",QJsonArray{QJsonObject{{"id",99999},{"name","G.E.M.邓紫棋"}}});
                auto missing=track(801,"G.E.M.邓紫棋"); missing.remove("artists");
                rows.prepend(missing); rows.prepend(imposter);
                return {{{"playlist",QJsonObject{{"id",id},{"name",id==102?"傍晚漫步":"经典作品"},
                    {"tags",id==102?QJsonArray{"散步"}:QJsonArray()}}},{"songs",rows}}};
            }
            return defaultHandler(r);
        };
        f.client->submit("我在散步，请你给我推荐邓紫棋的歌曲，给我形成一个30分钟的歌单。");
        QTRY_COMPARE(f.results.size(),1);
        const auto result=f.results.first().toObject().value("result").toObject();
        QCOMPARE(result.value("resolvedArtist").toObject().value("id").toInt(),7763);
        QCOMPARE(result.value("resolvedArtist").toObject().value("aliases").toArray(),QJsonArray({"邓紫棋"}));
        const auto rows=result.value("tracks").toArray(); QCOMPARE(rows.size(),12);
        for (const auto &value:rows) {
            const auto song=value.toObject(); QVERIFY(song.value("id").toInt()<800);
            QCOMPARE(song.value("artists").toArray().first().toObject().value("id").toInt(),7763);
            QCOMPARE(song.value("sourceId").toInt(),102);
            QCOMPARE(song.value("sceneEvidence").toObject().value("matchedScenes").toArray(),QJsonArray({"散步"}));
            QCOMPARE(song.value("sceneEvidence").toObject().value("status").toString(),QString("source_matched"));
        }
        QCOMPARE(f.music.count("/assistant/artists"),1); QCOMPARE(f.music.count("/radio/search"),1);
        QCOMPARE(f.music.count("/radio/playlist"),2); QCOMPARE(f.music.count("/artist/tracks"),0);
        QVERIFY(f.music.last("/radio/search").query().queryItemValue("keywords").contains("散步"));
    }
    void artistCatalogPaginationIsBoundedAndUnverifiedSceneIsPreserved() {
        Fixture f; QVERIFY(f.login()); auto step=f.scene();
        step.insert("input",QJsonObject{{"requiredArtist","陶喆"},{"artistOnly",true},
            {"brief",QJsonObject{{"scene","散步"},{"targetMinutes",30}}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        const auto defaultHandler=f.music.handler;
        f.music.handler=[defaultHandler](const FakeServer::Request &r)->FakeServer::Response {
            if (r.path()=="/radio/search") return {{{"playlists",QJsonArray()}}};
            if (r.path()=="/artist/tracks") {
                const int offset=r.query().queryItemValue("offset").toInt(); QJsonArray rows;
                for (int i=0;i<(offset==0?2:8);++i) {
                    auto song=track(600+offset+i,"陶喆"); song.insert("durationMs",180000); rows.append(song);
                }
                return {{{"songs",rows},{"total",100},{"more",true}}};
            }
            return defaultHandler(r);
        };
        f.client->submit("散步听陶喆30分钟"); QTRY_COMPARE(f.results.size(),1);
        const auto result=f.results.first().toObject().value("result").toObject();
        QCOMPARE(result.value("tracks").toArray().size(),10); QCOMPARE(f.music.count("/artist/tracks"),2);
        QCOMPARE(f.music.last("/artist/tracks").query().queryItemValue("offset"),QString("30"));
        QCOMPARE(f.music.count("/radio/search"),1);
        const auto source=result.value("sources").toArray().first().toObject();
        QCOMPARE(source.value("sourceKind").toString(),QString("artist_catalog"));
        QCOMPARE(source.value("sceneEvidence").toObject().value("status").toString(),QString("unverified"));
        QVERIFY(source.value("sceneEvidence").toObject().value("matchedScenes").toArray().isEmpty());
    }
    void sceneEvidenceNeverUsesQueryOrNegatedSourceAsProof_data() {
        QTest::addColumn<QString>("sourceName");
        for (const auto &name:QStringList{"不适合散步的音乐","散步不适用","不是散步歌单","仅供欣赏"})
            QTest::newRow(qPrintable(name)) << name;
    }
    void sceneEvidenceNeverUsesQueryOrNegatedSourceAsProof() {
        QFETCH(QString,sourceName); Fixture f; QVERIFY(f.login()); auto step=f.scene();
        step.insert("input",QJsonObject{{"queries",QJsonArray{"散步"}},
            {"brief",QJsonObject{{"scene","散步"},{"targetMinutes",3}}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        f.music.overrides["/radio/search"]={{{{"playlists",QJsonArray{QJsonObject{{"id",101},{"name",sourceName}}}}}}};
        f.music.overrides["/radio/playlist"]={{{{"playlist",QJsonObject{{"id",101},{"name",sourceName},{"tags",QJsonArray()}}},
            {"songs",QJsonArray{track(500)}}}}};
        f.client->submit("散步听3分钟音乐"); QTRY_COMPARE(f.results.size(),1);
        const auto result=f.results.first().toObject().value("result").toObject();
        const auto rows=result.value("tracks").toArray(); QCOMPARE(rows.size(),1);
        const auto evidence=rows.first().toObject().value("sceneEvidence").toObject();
        QCOMPARE(evidence.value("status").toString(),QString("unverified"));
        QVERIFY(evidence.value("matchedScenes").toArray().isEmpty());
    }
    void ambiguousSceneArtistNeverAssumesFirstSearchHit() {
        Fixture f; QVERIFY(f.login()); auto step=f.scene();
        step.insert("input",QJsonObject{{"requiredArtist","邓紫棋"},{"artistOnly",true},
            {"brief",QJsonObject{{"scene","散步"},{"targetMinutes",30}}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        f.music.overrides["/assistant/artists"]={{{{"artists",QJsonArray{
            QJsonObject{{"id",1},{"name","邓紫棋"}},QJsonObject{{"id",2},{"name","另一个艺人"},{"aliases",QJsonArray{"邓紫棋"}}}}}}}};
        f.client->submit("散步听邓紫棋30分钟"); QTRY_COMPARE(f.results.size(),1);
        const auto result=f.results.first().toObject().value("result").toObject();
        QVERIFY(!result.value("question").toString().isEmpty()); QVERIFY(!result.contains("resolvedArtist"));
        QCOMPARE(result.value("artists").toArray().size(),2); QCOMPARE(f.music.count("/artist/tracks"),0);
        QCOMPARE(f.music.count("/radio/search"),0); QCOMPARE(f.music.count("/radio/playlist"),0);
    }
    void lateArtistResolutionCannotContinueCancelledResearch() {
        Fixture f; QVERIFY(f.login()); auto step=f.scene();
        step.insert("input",QJsonObject{{"requiredArtist","陶喆"},{"artistOnly",true},
            {"brief",QJsonObject{{"scene","散步"},{"targetMinutes",30}}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        f.music.overrides["/assistant/artists"]={{{{"artists",QJsonArray{QJsonObject{{"id",101},{"name","陶喆"}}}}},200,250}};
        f.client->submit("散步听陶喆30分钟"); QTRY_COMPARE(f.music.count("/assistant/artists"),1);
        f.client->detach(); QTest::qWait(400);
        QCOMPARE(f.results.size(),0); QCOMPARE(f.music.count("/radio/search"),0); QCOMPARE(f.music.count("/artist/tracks"),0);
    }
    void artistCatalogRejectsMismatchedOrStalePageFacts_data() {
        QTest::addColumn<QString>("fault");
        for (const auto &fault:QStringList{"artistId","offset","limit","total","checkedAt"})
            QTest::newRow(qPrintable(fault)) << fault;
    }
    void artistCatalogRejectsMismatchedOrStalePageFacts() {
        QFETCH(QString,fault); Fixture f; QVERIFY(f.login()); auto step=f.scene();
        step.insert("input",QJsonObject{{"requiredArtist","陶喆"},{"artistOnly",true}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        QJsonObject page{{"artistId",101},{"offset",0},{"limit",30},{"total",1},{"more",false},
            {"checkedAt",QDateTime::currentMSecsSinceEpoch()},{"songs",QJsonArray{track(500,"陶喆")}}};
        if (fault=="artistId") page.insert(fault,99999);
        if (fault=="offset") page.insert(fault,0.5);
        if (fault=="limit") page.insert(fault,"30");
        if (fault=="total") page.insert(fault,-1);
        if (fault=="checkedAt") page.insert(fault,QDateTime::currentMSecsSinceEpoch()-600000);
        f.music.overrides["/artist/tracks"]={{{page}}};
        f.client->submit("推荐陶喆的歌"); QTRY_COMPARE(f.results.size(),1);
        const auto body=f.results.first().toObject(); QVERIFY(body.contains("error"));
        QCOMPARE(body.value("error").toObject().value("code").toString(),QString("local_tool_failed"));
        QVERIFY(!body.contains("result")); QCOMPARE(f.music.count("/artist/tracks"),1);
    }
    void thirtySongRequestContinuesBoundedResearchAndDoesNotInventMissingSongs_data() {
        QTest::addColumn<bool>("sufficient");
        QTest::newRow("enough-after-second-source")<<true;
        QTest::newRow("insufficient-candidates")<<false;
    }
    void thirtySongRequestContinuesBoundedResearchAndDoesNotInventMissingSongs() {
        QFETCH(bool,sufficient); Fixture f; QVERIFY(f.login()); auto step=f.scene();
        step.insert("input",QJsonObject{{"queries",QJsonArray{"陶喆音乐"}},{"requiredArtist","陶喆"},{"artistOnly",true},
            {"brief",QJsonObject{{"targetTrackCount",30},{"targetMinutes",0}}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        QJsonArray initial; for(int i=0;i<12;++i)initial.append(track(100+i,"陶喆"));
        f.music.overrides["/artist/tracks"]={{{{"songs",initial}}}};
        f.music.overrides["/radio/search"]={{{{"playlists",QJsonArray{QJsonObject{{"id",101},{"name","陶喆曲库"}}}}}}};
        QJsonArray extra=initial; if(sufficient){extra={};for(int i=0;i<20;++i)extra.append(track(200+i,"陶喆"));}
        f.music.overrides["/radio/playlist"]={{{{"playlist",QJsonObject{{"id",101},{"name","陶喆曲库"},{"tags",QJsonArray{}}}},{"songs",extra}}}};
        f.client->submit(QStringLiteral("推荐陶喆的30首歌")); QTRY_COMPARE(f.results.size(),1);
        const auto rows=f.results.first().toObject().value("result").toObject().value("tracks").toArray();
        QCOMPARE(rows.size(),sufficient?32:12); QCOMPARE(f.music.count("/artist/tracks"),1); QCOMPARE(f.music.count("/radio/playlist"),1);
        QVERIFY(rows.size()<=40); QVERIFY(f.music.count("/radio/search")<=2);
    }
    void followupStyleAdjustmentResearchesNewSourcesBeforeArtistFill() {
        Fixture f;QVERIFY(f.login());
        // The old preview is a sufficient artist-only pool. A new style turn
        // must still research sources rather than repeat that artist lookup.
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","preview"},{"pendingSteps",QJsonArray()},
            {"preview",QJsonObject{{"tracks",QJsonArray{track(100,"陶喆"),track(101,"陶喆"),track(102,"陶喆")}}}}};
        f.client->restoreLatest();QTRY_COMPARE(f.client->snapshot().value("status").toString(),QString("preview"));
        auto step=f.scene();step.insert("revision",2);
        step.insert("input",QJsonObject{{"queries",QJsonArray{"陶喆 华语 轻快","陶喆精选","第三条不应执行"}},
            {"requiredArtist","陶喆"},{"artistOnly",true},{"adjustmentStyles",QJsonArray{"轻快"}},
            {"brief",QJsonObject{{"targetMinutes",40},{"styles",QJsonArray{"华语","R&B","轻快"}},
                {"scene","下班通勤，后半段节奏更明快"}}}});
        f.current.insert("revision",2);f.current.insert("stateVersion",2);f.current.insert("status","waiting_tools");
        f.current.insert("pendingSteps",QJsonArray{step});
        f.app.overrides["/v1/tasks/task-1/actions"]={{{{"task",f.current}}}};
        const auto defaultHandler=f.music.handler;
        f.music.handler=[defaultHandler](const FakeServer::Request&r)->FakeServer::Response {
            if(r.path()=="/radio/search")return {{{"playlists",QJsonArray{
                QJsonObject{{"id",301},{"name","陶喆华语R&B歌曲精选"}},QJsonObject{{"id",302},{"name","陶喆轻快歌单"}}}}}};
            if(r.path()=="/radio/playlist") {
                if(r.query().queryItemValue("artistId")!="101")return {{{"error","artist filter missing"}},400};
                const int id=r.query().queryItemValue("id").toInt();
                return {{{"playlist",QJsonObject{{"id",id},{"name",id==302?"陶喆，欢快的午后":"陶喆华语R&B歌曲精选"},
                    {"tags",QJsonArray{"华语"}}}},
                    {"songs",id==302?QJsonArray{track(501,"陶喆"),track(502,"周杰伦"),track(503,"陶喆 / Guest")}
                        :QJsonArray{track(501,"陶喆"),track(504,"陶喆")}}}};
            }
            if(r.path()=="/artist/tracks") {
                QJsonArray rows;for(int i=0;i<15;++i)rows.append(track(600+i,"陶喆"));
                return {{{"songs",rows}}};
            }
            return defaultHandler(r);
        };
        f.client->submit("保留前三首，后半段轻快一点");QTRY_COMPARE(f.results.size(),1);
        const auto result=f.results.first().toObject().value("result").toObject();
        const auto rows=result.value("tracks").toArray();QVERIFY(rows.size()>3);QVERIFY(rows.size()<=40);
        QCOMPARE(f.music.count("/radio/search"),2);QCOMPARE(f.music.count("/radio/playlist"),2);QCOMPARE(f.music.count("/artist/tracks"),1);
        int firstPlaylistSearch=-1,artistSearch=-1;
        for(int i=0;i<f.music.requests.size();++i){const auto &r=f.music.requests.at(i);
            if(r.path()=="/radio/search"&&firstPlaylistSearch<0){firstPlaylistSearch=i;QVERIFY(r.query().queryItemValue("keywords").contains("轻快"));}
            if(r.path()=="/artist/tracks")artistSearch=i;}
        QVERIFY(firstPlaylistSearch>=0&&artistSearch>firstPlaylistSearch);
        QCOMPARE(rows.first().toObject().value("id").toInt(),501);
        QCOMPARE(rows.first().toObject().value("sourceId").toInt(),302);
        QCOMPARE(rows.first().toObject().value("sourceKind").toString(),QString("playlist"));
        QCOMPARE(rows.first().toObject().value("styleEvidence").toObject().value("status").toString(),QString("source_matched"));
        QCOMPARE(rows.first().toObject().value("styleEvidence").toObject().value("matchedStyles").toArray(),QJsonArray({"轻快"}));
        QVERIFY(rows.first().toObject().value("sourceQuery").toString().contains("轻快"));
        bool genericUnverified=false,fillUnverified=false;
        for(const auto &row:rows){const auto song=row.toObject();QVERIFY(song.value("id").toInt()!=502);
            QVERIFY(song.value("artist").toString().contains("陶喆"));
            if(song.value("id").toInt()==504)genericUnverified=song.value("styleEvidence").toObject().value("status")=="unverified";
            if(song.value("sourceKind")=="artist_catalog")fillUnverified=song.value("styleEvidence").toObject().value("status")=="unverified";}
        QVERIFY(genericUnverified);QVERIFY(fillUnverified);QVERIFY(result.value("sources").toArray().size()<=3);
    }
    void metadataMatchedCandidatesCannotBeCrowdedOutByEarlierGenericHits() {
        Fixture f;QVERIFY(f.login());auto step=f.scene();
        step.insert("input",QJsonObject{{"queries",QJsonArray{"陶喆精选"}},
            {"requiredArtist","陶喆"},{"artistOnly",true},
            {"brief",QJsonObject{{"targetMinutes",40},{"styles",QJsonArray{"轻快"}},{"adjustment","后半段轻快一点"}}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        const auto defaultHandler=f.music.handler;
        f.music.handler=[defaultHandler](const FakeServer::Request&r)->FakeServer::Response {
            if(r.path()=="/radio/search")return {{{"playlists",QJsonArray{
                QJsonObject{{"id",401},{"name","陶喆轻快（搜索摘要）"}},QJsonObject{{"id",402},{"name","陶喆精选"}}}}}};
            if(r.path()=="/radio/playlist") {
                const int id=r.query().queryItemValue("id").toInt();QJsonArray rows;
                for(int i=0;i<20;++i)rows.append(track(id==401?700+i:719+i,"陶喆"));
                return {{{"playlist",QJsonObject{{"id",id},{"name","陶喆精选"},
                    {"tags",id==402?QJsonArray{"欢快"}:QJsonArray{"华语"}}}},{"songs",rows}}};
            }
            return defaultHandler(r);
        };
        f.client->submit("后半段轻快一点");QTRY_COMPARE(f.results.size(),1);
        const auto result=f.results.first().toObject().value("result").toObject();const auto rows=result.value("tracks").toArray();
        QCOMPARE(rows.size(),39);QCOMPARE(f.music.count("/artist/tracks"),0);
        for(int i=0;i<20;++i){const auto song=rows.at(i).toObject();QCOMPARE(song.value("sourceId").toInt(),402);
            QCOMPARE(song.value("styleEvidence").toObject().value("status").toString(),QString("source_matched"));}
        // ID 719 appeared in the generic playlist first; its stronger verified
        // metadata provenance must survive deduplication.
        QCOMPARE(rows.first().toObject().value("id").toInt(),719);
        QCOMPARE(rows.last().toObject().value("sourceId").toInt(),401);
        QCOMPARE(rows.last().toObject().value("styleEvidence").toObject().value("status").toString(),QString("unverified"));
        QCOMPARE(f.music.count("/radio/playlist"),2);
    }
    void observedReadingCandidateWithinFiveDetails_data() {
        QTest::addColumn<bool>("credited");
        QTest::newRow("source-has-exact-credit")<<true;
        QTest::newRow("name-hint-is-not-credit")<<false;
    }
    void observedReadingCandidateWithinFiveDetails() {
        QFETCH(bool,credited);
        Fixture f;QVERIFY(f.login());auto step=f.scene();
        step.insert("input",QJsonObject{{"queries",QJsonArray{"读书 轻快","陶喆 轻快"}},
            {"requiredArtist","陶喆"},{"artistOnly",true},{"adjustmentStyles",QJsonArray{"轻快"}},
            {"brief",QJsonObject{{"targetMinutes",3},{"styles",QJsonArray{"轻快"}},{"scene","读书"}}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        const qint64 focusedId=8403072569LL;
        const QList<qint64> sceneIds{2429983112LL,7817361276LL,8173999160LL,65086173LL,415048320LL};
        const QList<qint64> artistIds{37924595LL,2271502281LL,323166565LL,2155148519LL,focusedId};
        const auto defaultHandler=f.music.handler;
        f.music.handler=[defaultHandler,credited,focusedId,sceneIds,artistIds](const FakeServer::Request&r)->FakeServer::Response {
            if(r.path()=="/radio/search"){
                const bool artistQuery=r.query().queryItemValue("keywords").contains("陶喆");
                const QStringList labels=artistQuery?QStringList{"轻快的音乐","轻快轻音乐","轻快小提琴","轻快钢琴曲","陶喆（轻快版）"}:
                    QStringList{"安静的读书轻音乐","【学习】读书，慢节奏无欢快轻音乐。","夜读｜读书轻音乐 书房","读书～轻音乐","适合读书听的轻音乐"};
                QJsonArray hits;for(int i=0;i<labels.size();++i)hits.append(QJsonObject{{"id",(artistQuery?artistIds:sceneIds).at(i)},{"name",labels.at(i)}});
                return {{{"playlists",hits}}};
            }
            if(r.path()=="/radio/playlist"){
                const auto id=r.query().queryItemValue("id").toLongLong();
                return {{{"playlist",QJsonObject{{"id",id},{"name",id==focusedId?"陶喆（轻快版）":"轻快的音乐"},{"tags",QJsonArray()}}},
                    {"songs",QJsonArray{track(id==focusedId?901:902,id==focusedId&&credited?"陶喆":"其他艺人")}}}};
            }
            if(r.path()=="/artist/tracks")return {{{"songs",QJsonArray{track(999,"陶喆")}}}};
            return defaultHandler(r);
        };
        f.client->submit("前三首保留，后半段轻快一点");QTRY_COMPARE(f.results.size(),1);
        QList<qint64> details;for(const auto&r:f.music.requests)if(r.path()=="/radio/playlist")details.append(r.query().queryItemValue("id").toLongLong());
        QCOMPARE(f.music.count("/radio/search"),2);QCOMPARE(details.size(),5);
        QCOMPARE(details.first(),focusedId);
        // Frozen v4 observed schedule read five generic/zero-credit sources and
        // omitted this search hit. These response contents are controlled test
        // fixtures, not a claim about the real playlist's songs.
        const QList<qint64> v4ObservedDetails{37924595,7817361276,2271502281,323166565,2155148519};
        // Replay the previous label-only admission on the SAME hit list. The
        // negative title used to match 欢快 and consumed one of the five slots.
        QList<QPair<qint64,bool>> legacyOrder{{sceneIds.at(0),false},{artistIds.at(0),true},{sceneIds.at(1),true},
            {sceneIds.at(2),false},{sceneIds.at(3),false},{sceneIds.at(4),false},
            {artistIds.at(1),true},{artistIds.at(2),true},{artistIds.at(3),true},{artistIds.at(4),true}};
        std::stable_sort(legacyOrder.begin(),legacyOrder.end(),[](const auto&a,const auto&b){return a.second>b.second;});
        QList<qint64> legacyDetails;for(int i=0;i<5;++i)legacyDetails.append(legacyOrder.at(i).first);
        QCOMPARE(legacyDetails,v4ObservedDetails);
        QVERIFY(!legacyDetails.contains(focusedId));QVERIFY(details.contains(focusedId));
        const auto result=f.results.first().toObject().value("result").toObject();
        const auto rows=result.value("tracks").toArray();QCOMPARE(rows.size(),1);QVERIFY(result.value("sources").toArray().size()<=3);
        const auto song=rows.first().toObject();QCOMPARE(song.value("artist").toString(),QString("陶喆"));
        if(credited){QCOMPARE(song.value("sourceId").toVariant().toLongLong(),focusedId);
            QCOMPARE(song.value("styleEvidence").toObject().value("status").toString(),QString("source_matched"));QCOMPARE(f.music.count("/artist/tracks"),0);}
        else{QCOMPARE(song.value("sourceKind").toString(),QString("artist_catalog"));
            QCOMPARE(song.value("styleEvidence").toObject().value("status").toString(),QString("unverified"));QCOMPARE(f.music.count("/artist/tracks"),1);}
    }
    void sourceStyleEvidenceUsesActualMetadataAndBoundedAsciiTerms_data() {
        QTest::addColumn<QString>("style");QTest::addColumn<QString>("label");QTest::addColumn<bool>("matched");
        QTest::newRow("urban-is-not-rb")<<QString("R&B")<<QString("Urban evening")<<false;
        QTest::newRow("rocket-is-not-rock")<<QString("摇滚")<<QString("Rocket memories")<<false;
        QTest::newRow("rb-spaces")<<QString("R&B")<<QString("华语 R & B")<<true;
        QTest::newRow("known-chinese-synonym")<<QString("轻快")<<QString("午后欢快精选")<<true;
        QTest::newRow("negative-observed-reading-source")<<QString("轻快")<<QString("【学习】读书，慢节奏无欢快轻音乐。")<<false;
        QTest::newRow("negative-bu")<<QString("轻快")<<QString("不轻快")<<false;
        QTest::newRow("negative-fei")<<QString("轻快")<<QString("非轻快")<<false;
        QTest::newRow("negative-missing")<<QString("轻快")<<QString("缺少轻快")<<false;
        QTest::newRow("negative-without")<<QString("轻快")<<QString("没有欢快的夜晚")<<false;
        QTest::newRow("double-negation-unknown")<<QString("轻快")<<QString("不是不轻快")<<false;
        QTest::newRow("double-negation-missing-unknown")<<QString("轻快")<<QString("并非没有轻快")<<false;
        QTest::newRow("contradictory-source-unknown")<<QString("轻快")<<QString("欢快精选；不轻快")<<false;
        QTest::newRow("question-unknown")<<QString("轻快")<<QString("轻快？")<<false;
        QTest::newRow("english-negation")<<QString("轻快")<<QString("not upbeat")<<false;
        QTest::newRow("english-double-negation")<<QString("轻快")<<QString("not not upbeat")<<false;
        QTest::newRow("english-non")<<QString("轻快")<<QString("non-upbeat music")<<false;
        QTest::newRow("english-modifier")<<QString("轻快")<<QString("not very upbeat")<<false;
        QTest::newRow("english-postfix")<<QString("轻快")<<QString("upbeat-free music")<<false;
        QTest::newRow("english-free-of")<<QString("摇滚")<<QString("free of rock")<<false;
        QTest::newRow("english-lack-of")<<QString("爵士")<<QString("lack of jazz")<<false;
        QTest::newRow("english-rb-negation")<<QString("R&B")<<QString("no R & B")<<false;
        QTest::newRow("question-prefix")<<QString("轻快")<<QString("是否轻快")<<false;
        QTest::newRow("possible-prefix")<<QString("轻快")<<QString("可能轻快")<<false;
        QTest::newRow("uncertain-prefix")<<QString("轻快")<<QString("不见得轻快")<<false;
        QTest::newRow("unclear-degree-suffix")<<QString("轻快")<<QString("轻快不明显")<<false;
        QTest::newRow("maybe-adverb")<<QString("轻快")<<QString("也许很欢快")<<false;
        QTest::newRow("weak-feeling-suffix")<<QString("轻快")<<QString("欢快感不强")<<false;
        QTest::newRow("english-possibly")<<QString("轻快")<<QString("possibly upbeat")<<false;
        QTest::newRow("english-might")<<QString("轻快")<<QString("might be upbeat")<<false;
        QTest::newRow("positive-idiom-wubi")<<QString("轻快")<<QString("无比欢快")<<true;
        QTest::newRow("positive-idiom-feichang")<<QString("轻快")<<QString("非常轻快")<<true;
        QTest::newRow("different-negated-style")<<QString("轻快")<<QString("不安静但很欢快")<<true;
        QTest::newRow("english-different-negated-style")<<QString("轻快")<<QString("not quiet but upbeat")<<true;
        QTest::newRow("english-whitespace-alias")<<QString("R&B")<<QString("rhythm   and blues essentials")<<true;
    }
    void sourceStyleEvidenceUsesActualMetadataAndBoundedAsciiTerms() {
        QFETCH(QString,style);QFETCH(QString,label);QFETCH(bool,matched);
        Fixture f;QVERIFY(f.login());auto step=f.scene();
        step.insert("input",QJsonObject{{"brief",QJsonObject{{"styles",QJsonArray{style}},{"adjustment","调整后半段"}}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        f.music.overrides["/radio/search"]={{{{"playlists",QJsonArray{QJsonObject{{"id",101},{"name",label}}}}}}};
        f.music.overrides["/radio/playlist"]={{{{"playlist",QJsonObject{{"id",101},{"name",label},{"tags",QJsonArray()}}},
            {"songs",QJsonArray{track(81)}}}}};
        f.client->submit("调整后半段");QTRY_COMPARE(f.results.size(),1);
        const auto rows=f.results.first().toObject().value("result").toObject().value("tracks").toArray();QCOMPARE(rows.size(),1);
        const auto evidence=rows.first().toObject().value("styleEvidence").toObject();
        QCOMPARE(evidence.value("status").toString(),matched?QString("source_matched"):QString("unverified"));
        QCOMPARE(evidence.value("matchedStyles").toArray(),matched?QJsonArray{style}:QJsonArray());
    }
    void explicitStyleDeltaWithoutAdjustmentTextDoesNotSearchWholeProseScene() {
        Fixture f;QVERIFY(f.login());auto step=f.scene();
        step.insert("input",QJsonObject{{"adjustmentStyles",QJsonArray{"轻快"}},
            {"queries",QJsonArray{"轻快"}},{"brief",QJsonObject{{"styles",QJsonArray{"华语","轻快"}},
                {"scene","下班通勤，后半段节奏更明快"}}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        f.client->submit("后半段轻快一点");QTRY_COMPARE(f.results.size(),1);
        QCOMPARE(f.music.count("/radio/search"),1);
        QCOMPARE(f.music.last("/radio/search").query().queryItemValue("keywords"),QString("轻快"));
    }
    void explicitStyleDeltaKeepsBackendLanguageQueryAheadOfUnconstrainedStyle() {
        Fixture f;QVERIFY(f.login());auto step=f.scene();
        step.insert("input",QJsonObject{{"adjustmentStyles",QJsonArray{"轻快"}},
            {"queries",QJsonArray{"华语 轻快","华语 R&B 轻快","upbeat"}},
            {"brief",QJsonObject{{"catalogLanguage","zh"},{"styles",QJsonArray{"华语","轻快"}},
                {"scene","下班通勤，后半段节奏更明快"}}}});
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        f.client->submit("后半段轻快一点");QTRY_COMPARE(f.results.size(),1);
        QStringList queried;
        for(const auto&r:f.music.requests)if(r.path()=="/radio/search")queried.append(r.query().queryItemValue("keywords"));
        QCOMPARE(queried,QStringList({"华语 轻快","华语 R&B 轻快"}));
    }
    void uncertainCreateFindsSavedTaskWithGetWithoutRepeatingPost() {
        Fixture f;QVERIFY(f.login());QSignalSpy errors(f.client,&MusicTaskClient::error);
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","preview"},{"pendingSteps",QJsonArray()}};
        f.app.overrides["/v1/tasks"]={{{{"error",QJsonObject{{"message","acknowledgment lost"}}}},503}};
        f.client->submit("读书40分钟");QTRY_COMPARE(errors.size(),1);
        QCOMPARE(f.client->snapshot().value("clientConnection").toObject().value("status").toString(),QString("retrying"));
        f.client->submit("读书40分钟");QCOMPARE(errors.size(),2);QCOMPARE(f.app.count("/v1/tasks"),1);
        auto *retry=f.client->findChild<QTimer*>(QString(),Qt::FindDirectChildrenOnly);QVERIFY(retry);retry->start(1);
        QTRY_COMPARE(f.client->snapshot().value("id").toString(),QString("task-1"));
        QVERIFY(!f.client->snapshot().contains("clientConnection"));QVERIFY(!f.client->busy());
        QCOMPARE(f.app.count("/v1/tasks"),1);QCOMPARE(f.app.count("/v1/tasks/latest/"+f.client->deviceId()),1);
        QCOMPARE(f.app.count("/v1/tasks/task-1/actions"),0);QCOMPARE(f.music.requests.size(),0);
    }
    void uncertainCreateWithNoSavedTaskStopsAndAllowsExplicitResubmit() {
        Fixture f;QVERIFY(f.login());QSignalSpy errors(f.client,&MusicTaskClient::error);
        f.app.overrides["/v1/tasks"]={{{{"error",QJsonObject{{"message","submission outcome unknown"}}}},503}};
        f.client->submit("读书40分钟");QTRY_COMPARE(errors.size(),1);
        auto *retry=f.client->findChild<QTimer*>(QString(),Qt::FindDirectChildrenOnly);QVERIFY(retry);retry->start(1);
        QTRY_VERIFY(f.client->snapshot().isEmpty());QTRY_COMPARE(errors.size(),2);
        QVERIFY(errors.last().first().toString().contains("未找到已保存"));
        QCOMPARE(f.app.count("/v1/tasks"),1);QCOMPARE(f.app.count("/v1/tasks/latest/"+f.client->deviceId()),1);
        QVERIFY(!f.client->busy());QVERIFY(!retry->isActive());
    }
    void observationRetryExhaustionRetainsDurableStatusAndExplicitRestoreRecovers() {
        Fixture f;QVERIFY(f.login());QSignalSpy errors(f.client,&MusicTaskClient::error);
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",4},{"stateVersion",12},
            {"status","planning"},{"pendingSteps",QJsonArray()}};
        f.app.overrides["/v1/tasks"]={{{{"task",f.current}}}};
        for(int i=0;i<6;++i)f.app.overrides["/v1/tasks/task-1"].append({{{"error",QJsonObject{{"message","backend disconnected"}}}},503});
        f.client->submit("读书40分钟");QTRY_COMPARE(f.client->snapshot().value("status").toString(),QString("planning"));
        auto *retry=f.client->findChild<QTimer*>(QString(),Qt::FindDirectChildrenOnly);QVERIFY(retry);
        for(int i=0;i<6;++i){retry->start(1);QTRY_COMPARE(errors.size(),i+1);}
        const auto disconnected=f.client->snapshot();
        QCOMPARE(disconnected.value("status").toString(),QString("planning"));
        QCOMPARE(disconnected.value("revision").toInt(),4);QCOMPARE(disconnected.value("stateVersion").toInt(),12);
        QCOMPARE(disconnected.value("clientConnection").toObject().value("status").toString(),QString("interrupted"));
        QVERIFY(disconnected.value("clientConnection").toObject().value("message").toString().contains("后台任务状态尚未确认"));
        QVERIFY(!f.client->busy());QVERIFY(!retry->isActive());
        f.current.insert("status","preview");f.current.insert("stateVersion",13);f.client->restoreLatest();
        QTRY_COMPARE(f.client->snapshot().value("status").toString(),QString("preview"));
        QVERIFY(!f.client->snapshot().contains("clientConnection"));
        QCOMPARE(f.app.count("/v1/tasks"),1);QCOMPARE(f.app.count("/v1/tasks/task-1/actions"),0);
    }
    void unconfirmedCancelNeverClaimsDurableCancellationOrReplaysPost() {
        Fixture f;QVERIFY(f.login());QSignalSpy errors(f.client,&MusicTaskClient::error);
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",4},{"stateVersion",12},
            {"status","planning"},{"pendingSteps",QJsonArray()}};
        f.client->restoreLatest();QTRY_COMPARE(f.client->snapshot().value("status").toString(),QString("planning"));
        f.app.overrides["/v1/tasks/task-1/actions"]={{{{"error",QJsonObject{{"message","cancel acknowledgment lost"}}}},503}};
        f.client->action("cancel");QTRY_COMPARE(errors.size(),1);
        QCOMPARE(f.client->snapshot().value("status").toString(),QString("planning"));
        QVERIFY(f.client->snapshot().contains("clientConnection"));
        f.current.insert("status","cancelled");f.current.insert("revision",5);f.current.insert("stateVersion",13);
        auto *retry=f.client->findChild<QTimer*>(QString(),Qt::FindDirectChildrenOnly);QVERIFY(retry);retry->start(1);
        QTRY_COMPARE(f.client->snapshot().value("status").toString(),QString("cancelled"));
        QCOMPARE(f.app.count("/v1/tasks/task-1/actions"),1);QVERIFY(!f.client->snapshot().contains("clientConnection"));
    }
    void lostResultAcknowledgmentResendsIdenticalResultWithoutResearch() {
        Fixture f;QVERIFY(f.login());
        const QString path="/v1/tasks/task-1/steps/scene-1/result";
        f.app.overrides[path]={{{{"error",QJsonObject{{"message","temporary network failure"}}}},503}};
        f.client->submit("喝酒推荐");
        QTRY_COMPARE_WITH_TIMEOUT(f.app.count(path),2,5000);
        QTRY_COMPARE_WITH_TIMEOUT(f.client->snapshot().value("status").toString(),QString("preview"),5000);
        QVector<QByteArray> bodies;for(const auto&r:f.app.requests)if(r.path()==path)bodies.append(r.body);
        QCOMPARE(bodies.size(),2);QCOMPARE(bodies.at(0),bodies.at(1));
        QCOMPARE(f.music.count("/radio/search"),1);QCOMPARE(f.music.count("/radio/playlist"),2);
    }
    void expiredLeaseStopsLocalResearchAndReportsBoundedFailure() {
        Fixture f;QVERIFY(f.login());auto step=f.scene();step.insert("expiresAt",QDateTime::currentMSecsSinceEpoch()-1);
        f.app.overrides["/v1/tasks"]={{{{"task",QJsonObject{{"id","task-1"},{"deviceId",f.client->deviceId()},
            {"revision",1},{"stateVersion",1},{"status","waiting_tools"},{"pendingSteps",QJsonArray{step}}}}}}};
        f.client->submit("过期查询");QTRY_COMPARE(f.results.size(),1);
        QCOMPARE(f.results.first().toObject().value("error").toObject().value("code").toString(),QString("local_tool_failed"));
        QTest::qWait(200);QCOMPARE(f.results.size(),1);QCOMPARE(f.music.count("/radio/playlist"),0);
    }
    void accountSwitchDropsOldSnapshotAndCancelsTools() {
        Fixture f;QVERIFY(f.login());f.client->submit("查歌");
        QTRY_COMPARE(f.music.count("/radio/search"),1);
        QVERIFY(f.login("bob"));QVERIFY(f.client->snapshot().isEmpty());
        QTest::qWait(250);QCOMPARE(f.results.size(),0);QVERIFY(f.client->snapshot().isEmpty());
    }
    void pauseCancelsBothLocalToolsAndDropsLateResults() {
        Fixture f;QVERIFY(f.login());f.client->submit("查歌");
        QTRY_COMPARE(f.music.count("/radio/search"),1);QVERIFY(!f.client->busy());
        f.client->action("pause");QTRY_COMPARE(f.client->snapshot().value("status").toString(),QString("paused"));
        QTest::qWait(250);QCOMPARE(f.results.size(),0);
    }
    void mutationBusyRejectsOverlapAndRestoreDoesNotAbortCreate() {
        Fixture f;QVERIFY(f.login());QSignalSpy errors(f.client,&MusicTaskClient::error);
        f.app.overrides["/v1/tasks"]={{{{"task",QJsonObject{{"id","task-1"},{"deviceId",f.client->deviceId()},
            {"revision",1},{"status","preview"},{"pendingSteps",QJsonArray()}}}},200,150}};
        f.client->submit("first");f.client->submit("second");f.client->restoreLatest();
        QTRY_COMPARE(f.client->snapshot().value("status").toString(),QString("preview"));
        QCOMPARE(f.app.count("/v1/tasks"),1);QCOMPARE(errors.size(),1);
    }
    void restoreAdoptionDoesNotApplyAnythingAndDetailsAreAllowlisted() {
        Fixture f;QVERIFY(f.login());
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"status","preview"},{"pendingSteps",QJsonArray()}};
        f.client->restoreLatest();QTRY_COMPARE(f.client->snapshot().value("status").toString(),QString("preview"));
        f.client->action("adopt");QTRY_COMPARE(f.client->snapshot().value("status").toString(),QString("awaiting_apply"));
        f.client->restoreLatest();QTRY_VERIFY(!f.client->busy());
        QCOMPARE(f.app.count("/v1/tasks/task-1/actions"),1);QCOMPARE(f.music.requests.size(),0);
        f.client->action("applied",{},{{"adoptionId","adoption-1"},{"cookie","forbidden"}});
        QTRY_COMPARE(f.client->snapshot().value("status").toString(),QString("adopted"));
        const auto body=f.app.last("/v1/tasks/task-1/actions").json();
        QCOMPARE(body.value("adoptionId").toString(),QString("adoption-1"));QVERIFY(!body.contains("cookie"));
    }
    void installationIdentityIsStableAndScopedToDataDirectory() {
        Fixture f;const auto original=f.client->deviceId();
        MusicTaskClient another(nullptr,&f.backend);QCOMPARE(another.deviceId(),original);
        QTemporaryDir other;f.env.set("MUSICPLAYER_DATA_DIR",other.path().toUtf8());
        MusicTaskClient fresh(nullptr,&f.backend);QVERIFY(fresh.deviceId()!=original);
    }
    void submitAmendsCurrentTaskUntilExplicitNewTaskDetach() {
        Fixture f;QVERIFY(f.login());
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",3},{"stateVersion",10},
            {"status","preview"},{"pendingSteps",QJsonArray()}};
        f.client->restoreLatest();QTRY_VERIFY(!f.client->snapshot().isEmpty());
        f.client->submit("保留前三首，只调整后半段");
        QTRY_COMPARE(f.app.count("/v1/tasks/task-1/actions"),1);
        const auto body=f.app.last("/v1/tasks/task-1/actions").json();
        QCOMPARE(body.value("action").toString(),QString("turn"));QCOMPARE(body.value("expectedRevision").toInt(),3);
        QCOMPARE(body.value("instruction").toString(),QString("保留前三首，只调整后半段"));
        QCOMPARE(f.app.count("/v1/tasks"),0);
        QTRY_VERIFY(!f.client->busy());f.client->detach();f.client->submit("新的一份安排");
        QTRY_COMPARE(f.app.count("/v1/tasks"),1);
    }
    void explicitPreferenceUpdatesAndClearsOnTurnWhileArtistContinuationRetainsIt() {
        Fixture f;QVERIFY(f.login());
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",1},
            {"status","preview"},{"pendingSteps",QJsonArray()}};
        f.client->restoreLatest();QTRY_VERIFY(!f.client->snapshot().isEmpty());
        f.client->submit("改成舒缓一点",QString("喜欢爵士"));
        QTRY_COMPARE(f.app.count("/v1/tasks/task-1/actions"),1);QTRY_VERIFY(!f.client->busy());
        QCOMPARE(f.app.last("/v1/tasks/task-1/actions").json().value("preference").toString(),QString("喜欢爵士"));
        f.client->submit("继续安排",QString());
        QTRY_COMPARE(f.app.count("/v1/tasks/task-1/actions"),2);QTRY_VERIFY(!f.client->busy());
        const auto cleared=f.app.last("/v1/tasks/task-1/actions").json();
        QVERIFY(cleared.contains("preference"));QCOMPARE(cleared.value("preference").toString(),QString());
        f.client->submit("选择歌手 邓紫棋，歌手ID 7763，继续刚才的任务");
        QTRY_COMPARE(f.app.count("/v1/tasks/task-1/actions"),3);QTRY_VERIFY(!f.client->busy());
        QVERIFY(!f.app.last("/v1/tasks/task-1/actions").json().contains("preference"));
    }
    void selectedCatalogArtistUsesDeterministicActionWithoutNaturalLanguageTurn() {
        Fixture f;QVERIFY(f.login());
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",7},{"stateVersion",15},
            {"status","needs_clarification"},{"pendingSteps",QJsonArray()},
            {"providedArtists",QJsonArray{QJsonObject{{"id",7763},{"name","G.E.M.邓紫棋"}}}}};
        f.client->restoreLatest();QTRY_COMPARE(f.client->snapshot().value("revision").toInt(),7);
        QSignalSpy errors(f.client,&MusicTaskClient::error);
        f.client->action("select_artist","此选择不应交模型理解",{{"artistId",7763},{"artist","wrong"},
            {"preference","must not be forwarded"},{"cookie","must not be forwarded"}});
        QTRY_COMPARE(f.app.count("/v1/tasks/task-1/actions"),1);QTRY_VERIFY(!f.client->busy());
        const auto body=f.app.last("/v1/tasks/task-1/actions").json();
        QCOMPARE(body.value("action").toString(),QString("select_artist"));
        QCOMPARE(body.value("artistId").toInt(),7763);QCOMPARE(body.value("expectedRevision").toInt(),7);
        QCOMPARE(body.value("deviceId").toString(),f.client->deviceId());
        QVERIFY(!body.value("requestId").toString().isEmpty());
        const auto keys=body.keys();
        QCOMPARE(QSet<QString>(keys.begin(),keys.end()),QSet<QString>({"action","artistId","deviceId","requestId","expectedRevision","catalogContext"}));
        QCOMPARE(body.value("catalogContext").toString(), NeteaseClient::catalogCacheContext());
        QCOMPARE(f.app.count("/v1/tasks"),0);QCOMPARE(f.app.count("/v1/assistant/plan"),0);QCOMPARE(f.music.requests.size(),0);
        QCOMPARE(errors.size(),0);
    }
    void deterministicArtistSelectionRejectsInvalidIdsBeforePosting() {
        Fixture f;QVERIFY(f.login());
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",2},{"stateVersion",5},
            {"status","needs_clarification"},{"pendingSteps",QJsonArray()}};
        f.client->restoreLatest();QTRY_VERIFY(!f.client->snapshot().isEmpty());
        QSignalSpy errors(f.client,&MusicTaskClient::error);
        const QJsonArray invalid{0,-1,1.5,9007199254740992.,"7763"};
        for(const auto &id:invalid)f.client->action("select_artist",{},{{"artistId",id}});
        QCOMPARE(errors.size(),invalid.size());QCOMPARE(f.app.count("/v1/tasks/task-1/actions"),0);QVERIFY(!f.client->busy());
    }
    void delayedOldObservationCannotOverwriteNewerPreview() {
        Fixture f;QVERIFY(f.login());
        f.current={{"id","task-1"},{"deviceId",f.client->deviceId()},{"revision",1},{"stateVersion",5},
            {"status","preview"},{"pendingSteps",QJsonArray()}};
        f.client->restoreLatest();QTRY_COMPARE(f.client->snapshot().value("stateVersion").toInt(),5);
        // restoreLatest is an explicit safe GET. It may observe an older cache or
        // delayed response; requirement revision alone cannot reject that.
        auto older=f.current;older.insert("stateVersion",4);older.insert("status","composing");
        f.app.overrides["/v1/tasks/latest/"+f.client->deviceId()]={{{{"task",older}}}};
        f.client->restoreLatest();QTRY_VERIFY(!f.client->busy());
        QCOMPARE(f.client->snapshot().value("status").toString(),QString("preview"));
        QCOMPARE(f.client->snapshot().value("stateVersion").toInt(),5);
    }
    void taskCapabilityDiscoveredAndClearedOnAccountChange() {
        Fixture f;QVERIFY(f.login());QVERIFY(!f.backend.taskRuntimeAvailable());
        bool done=false;f.backend.refreshAgentStatus(f.client,[&](const QString&error){QVERIFY(error.isEmpty());done=true;});
        QTRY_VERIFY(done);QVERIFY(f.backend.taskRuntimeAvailable());
        QVERIFY(f.login("bob"));QVERIFY(!f.backend.taskRuntimeAvailable());
    }
};
QTEST_GUILESS_MAIN(MusicTaskClientTest)
#include "musictaskclient_test.moc"
