#include "appbackendclient.h"
#include "mainwindow.h"
#include "musictaskclient.h"
#include "conversationclient.h"
#include "radiostore.h"
#include "radiodialog.h"
#include "neteaseclient.h"
#include "python_backend_fixture.h"
#include <QBuffer>
#include <QDataStream>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStackedWidget>
#include <QTableWidget>
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
        Response(QJsonObject value = {}, int code = 200, int wait = 0, bool pending = false)
            : body(std::move(value)), status(code), delay(wait), hang(pending) {}
        QJsonObject body;
        int status = 200;
        int delay = 0;
        bool hang = false;
        QByteArray raw;
        QByteArray contentType = "application/json";
        QByteArray extraHeaders;
        qint64 declaredLength = -1;
    };
    QTcpServer server;
    QVector<Request> requests;
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
                    if (response.hang) return;
                    QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(response.delay, this, [guard, response] {
                        if (!guard || guard->state() != QAbstractSocket::ConnectedState) return;
                        const QByteArray body = response.raw.isEmpty() ? QJsonDocument(response.body).toJson(QJsonDocument::Compact) : response.raw;
                        guard->write("HTTP/1.1 " + QByteArray::number(response.status) + " Test\r\nContent-Type: " + response.contentType + "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(response.declaredLength < 0 ? body.size() : response.declaredLength) + "\r\n" + response.extraHeaders + "\r\n" + body);
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
};

class OwnedBackend {
public:
    QProcess process;
    ~OwnedBackend() { stop(); }
    void stop() {
        if (process.state() == QProcess::NotRunning) return;
        process.terminate();
        if (!process.waitForFinished(3000)) { process.kill(); process.waitForFinished(3000); }
    }
};
QByteArray wave() {
    constexpr quint32 rate = 22050, size = rate * 60 * sizeof(qint16);
    QByteArray bytes;
    QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
    QDataStream stream(&buffer); stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF",4); stream << quint32(36+size); stream.writeRawData("WAVEfmt ",8);
    stream << quint32(16) << quint16(1) << quint16(1) << rate << quint32(rate*2) << quint16(2) << quint16(16);
    stream.writeRawData("data",4); stream << size; bytes.append(QByteArray(size,'\0'));
    return bytes;
}
QJsonObject song(int id, const QString &artist = QStringLiteral("测试歌手")) {
    return {{"id",id},{"title",QStringLiteral("录音室歌曲 %1").arg(id)},{"artist",artist},
        {"album","录音室专辑"},{"coverUrl",""},{"durationMs",240000}};
}
QJsonObject plan(const QJsonObject &input) {
    const bool adjusting = input.value("instruction").toString().contains(QStringLiteral("轻快"));
    if (!input.value("currentBrief").toObject().isEmpty()) {
        QJsonArray changes;
        if (adjusting) {
            const auto instruction = input.value("instruction").toString();
            const auto lockEvidence = instruction.contains(QStringLiteral("前三首保留"))
                ? QStringLiteral("前三首保留") : QStringLiteral("保留前三首");
            changes.append(QJsonObject{{"field","retainFirst"},{"op","set"},{"value",3},{"evidence",lockEvidence}});
            changes.append(QJsonObject{{"field","styles"},{"op","add"},{"value",QJsonArray{"轻快"}},{"evidence","轻快"}});
        }
        return {{"kind","research"},{"changes",changes},{"question",""},{"adjustment",adjusting?"轻快":""}};
    }
    return {{"kind","research"},{"title","微醺时刻"},{"scene","喝酒"},{"styles",QJsonArray{"华语 R&B"}},
        {"queries",QJsonArray{adjusting ? "轻快 微醺 R&B" : "微醺 R&B"}},{"artist","邓紫棋"},{"artistId",0},
        {"since",QDateTime::currentDateTimeUtc().addMonths(-6).toString(Qt::ISODate)},
        {"until",QDateTime::currentDateTimeUtc().addDays(1).toString(Qt::ISODate)},
        {"targetMinutes",40},{"excludeLive",true},{"excludedArtists",QJsonArray{}},
        {"requireReleases",true},{"retainFirst",adjusting?3:0},{"removeIds",QJsonArray{}},
        {"question",""},{"adjustment",adjusting?"后半段轻快":""}};
}
QJsonArray ids(const QJsonObject &snapshot) {
    QJsonArray value;
    for (const auto &track : snapshot.value("preview").toObject().value("tracks").toArray())
        value.append(track.toObject().value("id"));
    return value;
}
}

class MusicTaskFlowTest : public QObject {
    Q_OBJECT
private slots:
    void compositeTaskModificationRecoveryAndAccountIsolation();
    void chatOnlyDependentPreferenceDurationAndPausedRecovery();
    void expiredLocalLeasesPauseAndResumeWithoutServerRestart();
    void cleanup();
};

void MusicTaskFlowTest::compositeTaskModificationRecoveryAndAccountIsolation() {
    QTemporaryDir data;
    QVERIFY(data.isValid());
    ScopedEnvironment environment;
    environment.set("MUSICPLAYER_DATA_DIR",data.path().toUtf8());
    environment.set("MUSICPLAYER_REDIS_PORT","invalid-test-port");
    FakeServer qwen, music;
    QVERIFY(qwen.server.isListening()); QVERIFY(music.server.isListening());
    QVector<QString> researchRoles;
    int routingCalls = 0;
    qwen.handler = [&](const FakeServer::Request &request) -> FakeServer::Response {
        const auto messages = request.json().value("messages").toArray();
        const auto input = QJsonDocument::fromJson(messages.at(1).toObject().value("content").toString().toUtf8()).object();
        QJsonObject output;
        // 假上游按路由输入契约分流，避免提示词中文措辞变化被误报为产品失败。
        if (input.contains("conversationId") && input.contains("messageId") && input.contains("task")) {
            ++routingCalls;
            const auto instruction = input.value("instruction").toString();
            const bool memory = instruction.startsWith(QStringLiteral("记住"));
            const bool findMusic = !memory || instruction.contains(QStringLiteral("今晚喝酒"));
            QJsonArray intents, actions;
            if (memory) {
                intents.append("preference_proposal");
                const auto source = instruction.section(QChar(0xff1b), 0, 0);
                actions.append(QJsonObject{{"capability", "memory.propose"}, {"source", source}, {"dependsOn", QJsonArray()},
                    {"parameters", QJsonObject{{"operation", "set"}, {"preference", "喜欢粤语 R&B 音乐"}}}});
            }
            if (findMusic) {
                intents.append("scene_research");
                if (instruction.contains(QStringLiteral("邓紫棋"))) intents.append("release_research");
                actions.append(QJsonObject{{"capability", "music.task"}, {"source", instruction}, {"dependsOn", QJsonArray()},
                    {"parameters", QJsonObject{{"mode", input.value("task").isObject() ? "continue" : "new"}}}});
            }
            output = {{"intents", intents}, {"actions", actions}, {"missingSlots", QJsonArray()},
                {"slots", QJsonObject{{"artist", instruction.contains(QStringLiteral("邓紫棋")) ? "邓紫棋" : ""},
                    {"targetMinutes", instruction.contains(QStringLiteral("40分钟")) ? 40 : 0}}}};
        } else if (input.contains("currentBrief")) output = plan(input);
        else if (input.contains("scope") && input.contains("role") && !input.contains("result")) {
            researchRoles.append(input.value("role").toString());
            const auto scope = input.value("scope").toObject();
            output = {{"queries",scope.value("queries")},{"artist","邓紫棋"},{"artistId",0},
                {"since",scope.value("since")},{"until",scope.value("until")},{"note","按真实来源研究"}};
        } else if (input.contains("result") && input.contains("scope")) {
            output = {{"decision","done"},{"queries",QJsonArray{}},{"note","来源和候选可核验"}};
        } else if (input.contains("candidates") && input.contains("brief")) {
            QJsonArray ordered;
            const auto candidates = input.value("candidates").toArray();
            for (int i=candidates.size()-1; i>=qMax(0,candidates.size()-12); --i) ordered.append(candidates.at(i).toObject().value("index"));
            output = {{"order",ordered}};
        } else {
            return {{{"error",QJsonObject{{"message","unexpected model prompt"}}}},500};
        }
        return {{{"choices",QJsonArray{QJsonObject{{"finish_reason","stop"},{"message",QJsonObject{
            {"content",QString::fromUtf8(QJsonDocument(output).toJson(QJsonDocument::Compact))}}}}}},
            {"usage",QJsonObject{{"prompt_tokens",120},{"completion_tokens",60}}}},200,40};
    };
    const qint64 published = QDateTime::currentMSecsSinceEpoch()-7*86400000LL;
    const QByteArray audio = wave();
    music.handler = [&](const FakeServer::Request &request) -> FakeServer::Response {
        const auto path=request.path();
        if (path=="/health") return {{{"app","quiet-netease"},{"version",1},{"catalogVersion",2},{"profileIsolation",1}}};
        if (path=="/account/status") return {{{"loggedIn",false},{"nickname",""},{"avatarUrl",""},{"vipType",0}}};
        if (path=="/radio/search") {
            const bool adjusted=request.query().queryItemValue("keywords").contains(QStringLiteral("轻快"));
            return {{{"playlists",QJsonArray{QJsonObject{{"id",adjusted?102:101},{"name",adjusted?"轻快 微醺 R&B":"微醺 R&B"}}}},{"total",1}},200,150};
        }
        if (path=="/radio/playlist") {
            const bool adjusted=request.query().queryItemValue("id")=="102";
            QJsonArray songs;
            for(int i=1;i<=12;++i) songs.append(song((adjusted?3000:1000)+i));
            auto live=song(4000);live.insert("title","现场 Live");songs.append(live);
            return {{{"playlist",QJsonObject{{"id",adjusted?102:101},{"name",adjusted?"轻快 微醺 R&B":"微醺 R&B"},{"tags",QJsonArray{"华语","R&B","微醺"}}}},
                {"songs",songs}},200,100};
        }
        if (path=="/assistant/artists") {
            const QJsonObject artist{{"id",7763},{"name","邓紫棋"},{"aliases",QJsonArray{}}};
            return {{{"artists",QJsonArray{artist}}}};
        }
        if (path=="/assistant/releases") return {{{"artist",QJsonObject{{"id",7763},{"name","邓紫棋"}}},
            {"checkedAt",QDateTime::currentMSecsSinceEpoch()},{"albums",QJsonArray{QJsonObject{{"id",201},{"name","近期作品"},{"publishTime",published}}}}},200,150};
        if (path=="/assistant/album") return {{{"album",QJsonObject{{"id",201},{"name","近期作品"},{"artistId",7763},{"publishTime",published}}},
            {"songs",QJsonArray{song(9001,"邓紫棋"),song(9002,"邓紫棋")}}}};
        if (path=="/song/url") return {{{"id",request.query().queryItemValue("id").toInt()},
            {"url",QString::fromLatin1(music.base()+"/audio.wav?id="+request.query().queryItemValue("id").toLatin1())},{"trial",false},{"durationMs",60000},{"level","standard"}}};
        if (path=="/lyrics") return {{{"lyric","[00:00]测试歌词"},{"yrc",""}}};
        if (path=="/audio.wav") {
            qint64 start=0,end=audio.size()-1;bool ranged=false;
            for(const auto &line:request.headers.split('\n')) {
                const auto header=line.trimmed().toLower();
                if(!header.startsWith("range: bytes="))continue;
                const auto limits=header.mid(13).split('-');
                start=limits.value(0).toLongLong();
                if(limits.size()>1&&!limits.at(1).isEmpty())end=qMin(end,limits.at(1).toLongLong());
                ranged=true;
            }
            FakeServer::Response response;response.raw=audio.mid(start,end-start+1);response.contentType="audio/wav";
            response.status=ranged?206:200;response.declaredLength=end-start+1;response.extraHeaders="Accept-Ranges: bytes\r\n";
            if(ranged)response.extraHeaders+="Content-Range: bytes "+QByteArray::number(start)+"-"+QByteArray::number(end)+"/"+QByteArray::number(audio.size())+"\r\n";
            return response;
        }
        return {{{"error",QJsonObject{{"message","unexpected music fixture route"}}}},404};
    };
    environment.set("MUSICPLAYER_NETEASE_PORT",QByteArray::number(music.server.serverPort()));
    QDir(data.path()).mkpath("netease");
    QFile localToken(data.filePath("netease/token")); QVERIFY(localToken.open(QIODevice::WriteOnly));
    localToken.write(QByteArray(64,'f')); localToken.close();
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost,0));
    const auto port=reservation.serverPort();reservation.close();
    const QString serverPath=QFINDTESTDATA("../backend_py/tango_backend/server.py"); QVERIFY(!serverPath.isEmpty());
    const QString backendDirectory=QDir::cleanPath(QFileInfo(serverPath).absolutePath()+"/..");
    const QString python=QDir(backendDirectory).filePath(".venv/bin/python"); QVERIFY(QFileInfo(python).isExecutable());
    OwnedBackend backend;
    QProcessEnvironment child;
    child.insert("QUIET_REGISTRATION_MODE","open");child.insert("PATH","/usr/bin:/bin");child.insert("QUIET_BACKEND_HOST","127.0.0.1");
    child.insert("QUIET_BACKEND_PORT",QString::number(port));child.insert("QUIET_DB_PATH",data.filePath("backend.sqlite"));
    child.insert("QUIET_STORAGE_DRIVER","sqlite");
    child.insert("QWEN_API_KEY","fake-task-flow-key");child.insert("QWEN_BASE_URL",QString::fromLatin1(qwen.base()+"/v1"));
    child.insert("QWEN_MODEL","qwen-task-flow-fixture");child.insert("QUIET_AI_PER_MINUTE","1000");
    backend.process.setProcessEnvironment(child);backend.process.setProcessChannelMode(QProcess::MergedChannels);
    backend.process.setWorkingDirectory(backendDirectory);
    backend.process.start(python,{"-m","tango_backend.server","--no-env"});QVERIFY(backend.process.waitForStarted(3000));
    QTRY_VERIFY_WITH_TIMEOUT(ownedPythonBackendReady(backend.process,port),PythonBackendFixtureStartupTimeoutMs);
    environment.set("MUSICPLAYER_BACKEND_URL","http://127.0.0.1:"+QByteArray::number(port));
    auto *account=AppBackendClient::instance(); QVERIFY(!account->authenticated());
    auto window=std::make_unique<MainWindow>();window->show();window->p_PlayerObject->setMuted(true);
    bool authenticated=false;QString failure;
    account->registerUser("task-flow-owner","owned-fixture-password",window.get(),[&](const QString &error){failure=error;authenticated=true;});
    QTRY_VERIFY_WITH_TIMEOUT(authenticated,5000);QVERIFY2(failure.isEmpty(),qPrintable(failure));
    const QString ownerProfile=window->accountProfile();QVERIFY(ownerProfile.startsWith("server:"));
    bool ready=false;account->refreshAgentStatus(window.get(),[&](const QString &error){failure=error;ready=true;});
    QTRY_VERIFY_WITH_TIMEOUT(ready,3000);QVERIFY2(failure.isEmpty(),qPrintable(failure));QVERIFY(account->taskRuntimeAvailable()); QVERIFY(account->routingAvailable());
    QVERIFY(QMetaObject::invokeMethod(window.get(),"openRadio",Qt::DirectConnection));
    auto *dialog=window->findChild<RadioDialog *>();QVERIFY(dialog);
    auto *task=window->findChild<MusicTaskClient *>();QVERIFY(task);
    auto *conversation=window->findChild<ConversationClient *>();QVERIFY(conversation);
    auto errors=std::make_unique<QSignalSpy>(task,&MusicTaskClient::error);
    connect(task, &MusicTaskClient::snapshotChanged, window.get(), [](const QJsonObject &snapshot) {
        if (snapshot.value("status").toString() == "failed")
            qWarning().noquote() << "Owned fixture task failure:" << QJsonDocument(snapshot.value("error").toObject()).toJson(QJsonDocument::Compact);
    });
    QTRY_VERIFY_WITH_TIMEOUT(!task->busy()&&!conversation->busy(),3000);
    auto *input=dialog->findChild<QPlainTextEdit *>("edit_RadioInstruction");
    auto *generate=dialog->findChild<QPushButton *>("button_RadioGenerate");QVERIFY(input&&generate);
    const auto sendChat = [&](const QString &text) {
        if (!QTest::qWaitFor([&] { return !conversation->busy() && !task->busy(); }, 3000)) return false;
        input->setPlainText(text);
        if (!generate->isEnabled()) return false;
        QTest::keyClick(input, Qt::Key_Return);
        return QTest::qWaitFor([&] { return input->toPlainText().isEmpty(); }, 3000);
    };
    QVERIFY(sendChat(QStringLiteral("记住我喜欢粤语 R&B 音乐；今晚喝酒，做约40分钟华语 R&B 歌单，加入邓紫棋近半年的新作，不要现场版，先预览")));
    auto *proposal = dialog->findChild<QWidget *>("panel_RadioPreferenceProposal"); QVERIFY(proposal);
    auto *transcript = dialog->findChild<QPlainTextEdit *>("text_RadioConversation"); QVERIFY(transcript);
    QTRY_VERIFY_WITH_TIMEOUT(transcript->toPlainText().contains(QStringLiteral("确认保存口味")), 3000);
    QVERIFY(proposal->isHidden());
    QCOMPARE(RadioStore(QSqlDatabase::database()).preference(ownerProfile), QString());
    QVERIFY(sendChat(QStringLiteral("取消口味")));
    QVERIFY(proposal->isHidden());
    QCOMPARE(RadioStore(QSqlDatabase::database()).preference(ownerProfile), QString());
    QTRY_VERIFY_WITH_TIMEOUT(!task->snapshot().value("id").toString().isEmpty(),3000);
    auto *library=window->findChild<QPushButton *>("libraryButton");QVERIFY(library);library->click();QVERIFY(!dialog->isVisible());
    // 两个工具租约尚未完成时中断；重启必须保留已完成调用，不能重复发送模型请求。
    QTRY_COMPARE_WITH_TIMEOUT(task->snapshot().value("status").toString(),QString("waiting_tools"),5000);
    QCOMPARE(task->snapshot().value("pendingSteps").toArray().size(),2);
    QSet<QString> pendingSkills;
    for(const auto &row:task->snapshot().value("pendingSteps").toArray())pendingSkills.insert(row.toObject().value("skill").toString());
    QCOMPARE(pendingSkills,(QSet<QString>{"scene.research","releases.research"}));
    const QString interruptedId=task->snapshot().value("id").toString();
    QTRY_COMPARE_WITH_TIMEOUT(conversation->snapshot().value("latestTaskId").toString(),interruptedId,3000);
    const QString conversationId=conversation->snapshot().value("id").toString();QVERIFY(!conversationId.isEmpty());
    QVERIFY(QMetaObject::invokeMethod(dialog,"taskActionRequested",Qt::DirectConnection,Q_ARG(QString,QString("new_conversation"))));
    QCOMPARE(conversation->snapshot().value("id").toString(),conversationId);
    QCOMPARE(task->snapshot().value("id").toString(),interruptedId);
    QTRY_VERIFY_WITH_TIMEOUT(input->toPlainText().isEmpty(),3000);
    QCOMPARE(conversation->deviceId(),task->deviceId());
    QCOMPARE(conversation->snapshot().value("messages").toArray().first().toObject().value("role").toString(),QString("user"));
    const QString interruptedDevice=task->deviceId();
    const int callsBeforeInterruption=qwen.requests.size();
    QCOMPARE(callsBeforeInterruption,3); QCOMPARE(routingCalls,1);
    window.reset();
    bool detached=false;account->logout(this,[&](const QString &){detached=true;});
    QTRY_VERIFY_WITH_TIMEOUT(detached,3000);backend.stop();
    backend.process.start(python,{"-m","tango_backend.server","--no-env"});QVERIFY(backend.process.waitForStarted(3000));
    QTRY_VERIFY_WITH_TIMEOUT(ownedPythonBackendReady(backend.process,port),PythonBackendFixtureStartupTimeoutMs);
    window=std::make_unique<MainWindow>();window->show();window->p_PlayerObject->setMuted(true);
    authenticated=false;account->login("task-flow-owner","owned-fixture-password",window.get(),[&](const QString &error){failure=error;authenticated=true;});
    QTRY_VERIFY_WITH_TIMEOUT(authenticated,5000);QVERIFY2(failure.isEmpty(),qPrintable(failure));
    ready=false;account->refreshAgentStatus(window.get(),[&](const QString &error){failure=error;ready=true;});QTRY_VERIFY_WITH_TIMEOUT(ready,3000);
    QVERIFY(QMetaObject::invokeMethod(window.get(),"openRadio",Qt::DirectConnection));
    task=window->findChild<MusicTaskClient *>();dialog=window->findChild<RadioDialog *>();QVERIFY(task&&dialog);
    conversation=window->findChild<ConversationClient *>();QVERIFY(conversation);
    input=dialog->findChild<QPlainTextEdit *>("edit_RadioInstruction");generate=dialog->findChild<QPushButton *>("button_RadioGenerate");
    transcript=dialog->findChild<QPlainTextEdit *>("text_RadioConversation");QVERIFY(input&&generate&&transcript);
    errors=std::make_unique<QSignalSpy>(task,&MusicTaskClient::error);
    QTRY_COMPARE_WITH_TIMEOUT(task->snapshot().value("status").toString(),QString("paused"),4000);
    QCOMPARE(task->snapshot().value("id").toString(),interruptedId);QCOMPARE(task->deviceId(),interruptedDevice);
    QCOMPARE(conversation->snapshot().value("id").toString(),conversationId);
    QCOMPARE(qwen.requests.size(),callsBeforeInterruption);QCOMPARE(window->p_PlayerObject->state(),QMediaPlayer::StoppedState);
    auto *resume=dialog->findChild<QPushButton *>("button_TaskResume");QVERIFY(resume&&resume->isEnabled());
    QVERIFY(sendChat(QStringLiteral("继续任务")));
    library=window->findChild<QPushButton *>("libraryButton");QVERIFY(library);library->click();
    input=dialog->findChild<QPlainTextEdit *>("edit_RadioInstruction");generate=dialog->findChild<QPushButton *>("button_RadioGenerate");
    QTRY_VERIFY_WITH_TIMEOUT(task->snapshot().value("status").toString()=="preview"||task->snapshot().value("status").toString()=="failed"||task->snapshot().value("status").toString()=="budget_exhausted",12000);
    QCOMPARE(task->snapshot().value("status").toString(),QString("preview"));
    QCOMPARE(errors->count(),0);QCOMPARE(window->p_PlayerObject->state(),QMediaPlayer::StoppedState);
    const auto first=task->snapshot();const auto firstIds=ids(first);QCOMPARE(firstIds.size(),10);
    QCOMPARE(first.value("preview").toObject().value("totalDurationMs").toInt(),2400000);
    QCOMPARE(researchRoles,(QVector<QString>{"coordinator"}));
    QCOMPARE(first.value("strategy").toString(),QString("single_agent"));
    bool releaseSatisfied=false,durationSatisfied=false;
    for(const auto &row:first.value("preview").toObject().value("checks").toArray()) {
        const auto check=row.toObject();
        if(check.value("key")=="releases")releaseSatisfied=check.value("status")=="satisfied";
        if(check.value("key")=="duration")durationSatisfied=check.value("status")=="satisfied";
    }
    QVERIFY(releaseSatisfied&&durationSatisfied);QVERIFY(!firstIds.contains(4000));
    QVERIFY(music.count("/radio/search") > 0); QVERIFY(music.count("/assistant/artists") > 0);
    QVERIFY(music.count("/assistant/releases") > 0); QVERIFY(music.count("/assistant/album") > 0);
    QVERIFY(QMetaObject::invokeMethod(window.get(),"openRadio",Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(!conversation->busy(),3000);
    // 提议与确认原话都先入会话；后续任务刷新不能覆盖已保存口味的回复。
    const QString remember=QStringLiteral("记住我喜欢粤语 R&B 音乐");
    QVERIFY(sendChat(remember));
    proposal = dialog->findChild<QWidget *>("panel_RadioPreferenceProposal"); QVERIFY(proposal);
    QTRY_VERIFY_WITH_TIMEOUT(transcript->toPlainText().contains(QStringLiteral("确认保存口味")),3000);
    QVERIFY(proposal->isHidden());
    QVERIFY(sendChat(QStringLiteral("确认保存口味")));
    QTRY_COMPARE_WITH_TIMEOUT(RadioStore(QSqlDatabase::database()).preference(ownerProfile),QStringLiteral("喜欢粤语 R&B 音乐"),5000);
    const auto hasSavedReply=[&] {
        for(const auto &row:conversation->snapshot().value("messages").toArray())
            if(row.toObject().value("content").toString().contains(QStringLiteral("已确认保存当前账号的本机长期口味")))return true;
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(hasSavedReply()&&!conversation->busy(),3000);
    QCOMPARE(task->snapshot().value("id").toString(),interruptedId);QCOMPARE(ids(task->snapshot()),firstIds);
    QCOMPARE(window->p_PlayerObject->state(),QMediaPlayer::StoppedState);
    QVERIFY(sendChat(QStringLiteral("保留前三首")));
    QTRY_COMPARE_WITH_TIMEOUT(task->snapshot().value("lockedIds").toArray().size(),3,3000);
    QTRY_VERIFY_WITH_TIMEOUT(!task->busy(),3000);
    QTRY_VERIFY_WITH_TIMEOUT(hasSavedReply()&&!conversation->busy(),3000);
    QVERIFY(sendChat(QStringLiteral("保留前三首，后半段轻快一点")));
    QTRY_VERIFY_WITH_TIMEOUT(task->snapshot().value("preview").toObject().value("version").toInt()>=2||task->snapshot().value("status")=="failed",12000);
    QCOMPARE(task->snapshot().value("status").toString(),QString("preview"));
    const auto revised=task->snapshot();QCOMPARE(revised.value("id"),first.value("id"));
    const int firstAdjustmentRevision=revised.value("revision").toInt();
    const auto hasTaskReply=[&](int revision) {
        for(const auto &row:conversation->snapshot().value("messages").toArray()) {
            const auto entry=row.toObject();
            if(entry.value("source")=="task"&&entry.value("taskRevision").toInt()==revision)return true;
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(hasTaskReply(firstAdjustmentRevision),3000);
    const auto revisedIds=ids(revised);for(int i=0;i<3;++i)QCOMPARE(revisedIds.at(i),firstIds.at(i));
    QVERIFY(revisedIds!=firstIds);QVERIFY(qAbs(revised.value("preview").toObject().value("totalDurationMs").toInt()-2400000)<=240000);
    QVERIFY(revised.value("brief").toObject().value("styles").toArray().contains("轻快"));
    const int callsBeforeUndo=qwen.requests.size();
    auto *undo=dialog->findChild<QPushButton *>("button_TaskUndo");QVERIFY(undo&&undo->isEnabled());
    QVERIFY(sendChat(QStringLiteral("撤回修改")));
    QTRY_COMPARE_WITH_TIMEOUT(ids(task->snapshot()),firstIds,3000);
    QCOMPARE(qwen.requests.size(),callsBeforeUndo);QTRY_VERIFY_WITH_TIMEOUT(!task->busy(),3000);
    QVERIFY(sendChat(QStringLiteral("保留前三首，后半段轻快一点")));
    QTRY_VERIFY_WITH_TIMEOUT(task->snapshot().value("preview").toObject().value("version").toInt()>=2||task->snapshot().value("status")=="failed",12000);
    QCOMPARE(task->snapshot().value("status").toString(),QString("preview"));QCOMPARE(ids(task->snapshot()),revisedIds);
    const int repeatedAdjustmentRevision=task->snapshot().value("revision").toInt();
    QTRY_VERIFY_WITH_TIMEOUT(hasTaskReply(repeatedAdjustmentRevision),3000);
    QSet<QString> adjustmentTurns,replyTurns;
    for(const auto &row:conversation->snapshot().value("messages").toArray()) {
        const auto entry=row.toObject();
        if(entry.value("role")=="user"&&entry.value("content")==QStringLiteral("保留前三首，后半段轻快一点"))
            adjustmentTurns.insert(entry.value("id").toString());
        if(entry.value("source")=="task"&&(entry.value("taskRevision").toInt()==firstAdjustmentRevision||
            entry.value("taskRevision").toInt()==repeatedAdjustmentRevision))replyTurns.insert(entry.value("turnId").toString());
    }
    QCOMPARE(adjustmentTurns.size(),2);QCOMPARE(replyTurns,adjustmentTurns);
    const auto stableRevision=task->snapshot().value("revision").toInt();
    bool staleRejected=false;
    account->musicTaskAction(task->snapshot().value("id").toString(),
        {{"deviceId",task->deviceId()},{"requestId",QUuid::createUuid().toString(QUuid::WithoutBraces)},
         {"expectedRevision",stableRevision-1},{"action","cancel"}},window.get(),
        [&](const QJsonObject &,const QString &error){failure=error;staleRejected=true;});
    QTRY_VERIFY_WITH_TIMEOUT(staleRejected,3000);QVERIFY(!failure.isEmpty());
    QCOMPARE(task->snapshot().value("revision").toInt(),stableRevision);QCOMPARE(ids(task->snapshot()),revisedIds);
    const QString taskId=revised.value("id").toString();
    QTRY_VERIFY_WITH_TIMEOUT(!conversation->busy(),3000);
    QVERIFY(hasSavedReply());
    auto *quotaLabel=dialog->findChild<QLabel *>("label_RadioQuota");QVERIFY(quotaLabel);
    const auto expectedQuota=QStringLiteral("今日剩余 %1/30 次 · 内测剩余 %2 次")
        .arg(30-qwen.requests.size()).arg(300-qwen.requests.size());
    QTRY_COMPARE_WITH_TIMEOUT(quotaLabel->text(),expectedQuota,3000);
    const auto screenshotPath = qEnvironmentVariable("MUSICPLAYER_TEST_SCREENSHOT");
    if (!screenshotPath.isEmpty()) QVERIFY(window->grab().save(screenshotPath));
    const QString deviceId=task->deviceId();const int callsBeforeRecovery=qwen.requests.size();
    // 真实 Python 进程持久保存模型和工具进度，重启界面与后台后核对。
    bool loggedOut=false;account->logout(window.get(),[&](const QString &){loggedOut=true;});
    QTRY_VERIFY_WITH_TIMEOUT(loggedOut,3000);window.reset();backend.stop();
    backend.process.start(python,{"-m","tango_backend.server","--no-env"});QVERIFY(backend.process.waitForStarted(3000));
    QTRY_VERIFY_WITH_TIMEOUT(ownedPythonBackendReady(backend.process,port),PythonBackendFixtureStartupTimeoutMs);
    window=std::make_unique<MainWindow>();window->show();window->p_PlayerObject->setMuted(true);
    authenticated=false;account->login("task-flow-owner","owned-fixture-password",window.get(),[&](const QString &error){failure=error;authenticated=true;});
    QTRY_VERIFY_WITH_TIMEOUT(authenticated,5000);QVERIFY2(failure.isEmpty(),qPrintable(failure));QCOMPARE(window->accountProfile(),ownerProfile);
    ready=false;account->refreshAgentStatus(window.get(),[&](const QString &error){failure=error;ready=true;});QTRY_VERIFY_WITH_TIMEOUT(ready,3000);
    QVERIFY(QMetaObject::invokeMethod(window.get(),"openRadio",Qt::DirectConnection));
    task=window->findChild<MusicTaskClient *>();dialog=window->findChild<RadioDialog *>();QVERIFY(task&&dialog);QCOMPARE(task->deviceId(),deviceId);
    conversation=window->findChild<ConversationClient *>();QVERIFY(conversation);
    input=dialog->findChild<QPlainTextEdit *>("edit_RadioInstruction");generate=dialog->findChild<QPushButton *>("button_RadioGenerate");QVERIFY(input&&generate);
    QTRY_COMPARE_WITH_TIMEOUT(task->snapshot().value("id").toString(),taskId,4000);
    QCOMPARE(ids(task->snapshot()),revisedIds);QCOMPARE(task->snapshot().value("status").toString(),QString("preview"));
    QCOMPARE(conversation->snapshot().value("id").toString(),conversationId);QVERIFY(hasSavedReply());
    QCOMPARE(qwen.requests.size(),callsBeforeRecovery);QCOMPARE(window->p_PlayerObject->state(),QMediaPlayer::StoppedState);
    QCOMPARE(window->p_PlayerList->mediaCount(),0);
    auto *adopt=dialog->findChild<QPushButton *>("button_RadioPlay");QVERIFY(adopt&&adopt->isEnabled());
    QVERIFY(sendChat(QStringLiteral("采用歌单")));
    QTRY_COMPARE_WITH_TIMEOUT(task->snapshot().value("status").toString(),QString("adopted"),5000);
    QTRY_COMPARE_WITH_TIMEOUT(window->p_PlayerObject->state(),QMediaPlayer::PlayingState,7000);
    QCOMPARE(window->p_PlayerList->mediaCount(),revisedIds.size());
    QTRY_VERIFY_WITH_TIMEOUT(window->p_PlayerObject->position()>50,5000);window->p_PlayerObject->pause();
    QTRY_COMPARE_WITH_TIMEOUT(window->p_PlayerObject->state(),QMediaPlayer::PausedState,3000);
    const int resolverCalls=music.count("/song/url");const int queueCount=window->p_PlayerList->mediaCount();
    QTRY_VERIFY_WITH_TIMEOUT(!task->busy(),3000);QVERIFY(sendChat(QStringLiteral("采用歌单")));QTest::qWait(200);
    QCOMPARE(music.count("/song/url"),resolverCalls);QCOMPARE(window->p_PlayerList->mediaCount(),queueCount);
    task->restoreLatest();QTRY_VERIFY_WITH_TIMEOUT(!task->busy(),3000);QTest::qWait(100);
    QCOMPARE(window->p_PlayerObject->state(),QMediaPlayer::PausedState);QCOMPARE(music.count("/song/url"),resolverCalls);
    QSqlQuery adoption;QVERIFY(adoption.exec("SELECT COUNT(*) FROM task_adoptions"));QVERIFY(adoption.next());QCOMPARE(adoption.value(0).toInt(),1);adoption.finish();
    authenticated=false;account->registerUser("task-flow-other","owned-fixture-password",window.get(),[&](const QString &error){failure=error;authenticated=true;});
    QTRY_VERIFY_WITH_TIMEOUT(authenticated,5000);QVERIFY2(failure.isEmpty(),qPrintable(failure));QVERIFY(window->accountProfile()!=ownerProfile);
    QTRY_VERIFY_WITH_TIMEOUT(task->snapshot().isEmpty()&&!task->busy(),5000);QVERIFY(dialog->recommendations().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!conversation->busy(),3000);QVERIFY(conversation->snapshot().value("messages").toArray().isEmpty());
    bool denied=false;account->musicTask(taskId,window.get(),[&](const QJsonObject &,const QString &error){failure=error;denied=true;});
    QTRY_VERIFY_WITH_TIMEOUT(denied,3000);QVERIFY(!failure.isEmpty());
    account->logout(window.get());QVERIFY(!account->authenticated());window.reset();
    QVERIFY(!backend.process.readAll().contains("fake-task-flow-key"));
    const auto expectedProfile=QCryptographicHash::hash(ownerProfile.toUtf8(),QCryptographicHash::Sha256).toHex();
    bool scopedRequest=false;
    for(const auto &request:music.requests) {
        QVERIFY(!request.headers.contains("fake-task-flow-key"));
        QVERIFY(!request.headers.contains(ownerProfile.toUtf8()));
        if(request.path()=="/radio/search")scopedRequest|=request.headers.contains("X-MusicPlayer-Profile: "+expectedProfile);
    }
    QVERIFY(scopedRequest);
}


void MusicTaskFlowTest::chatOnlyDependentPreferenceDurationAndPausedRecovery() {
    QTemporaryDir data; QVERIFY(data.isValid()); ScopedEnvironment environment;
    environment.set("MUSICPLAYER_DATA_DIR", data.path().toUtf8());
    environment.set("MUSICPLAYER_REDIS_PORT", "invalid-test-port");
    FakeServer qwen, music; QVERIFY(qwen.server.isListening() && music.server.isListening());
    QStringList planningPreferences;
    qwen.handler = [&](const FakeServer::Request &request) -> FakeServer::Response {
        const auto messages = request.json().value("messages").toArray();
        const auto input = QJsonDocument::fromJson(messages.at(1).toObject().value("content").toString().toUtf8()).object();
        const auto instruction = input.value("instruction").toString();
        QJsonObject output;
        // 与正式路由输入对应；不根据助手文案猜测模型角色。
        if (input.contains("conversationId") && input.contains("messageId") && input.contains("task")) {
            // 模拟上游只提供受控提案；真正的参数、依赖和授权仍由 Python 产品代码校验。
            QJsonArray actions, intents;
            if (instruction.startsWith(QStringLiteral("记住"))) {
                intents.append("preference_proposal");
                actions.append(QJsonObject{{"capability", "memory.propose"}, {"source", "记住我喜欢粤语"},
                    {"dependsOn", QJsonArray{}}, {"parameters", QJsonObject{{"operation", "set"}, {"preference", "喜欢粤语"}}}});
            }
            intents.append("scene_research");
            actions.append(QJsonObject{{"capability", "music.task"}, {"source", instruction},
                {"dependsOn", actions.isEmpty() ? QJsonArray{} : QJsonArray{0}},
                {"parameters", QJsonObject{{"mode", input.value("task").isObject() ? "continue" : "new"}}}});
            output = {{"intents", intents}, {"actions", actions}, {"missingSlots", QJsonArray{}},
                {"slots", QJsonObject{{"artist", ""}, {"targetMinutes", instruction.contains("40分钟") ? 40 : 20}}}};
        } else if (input.contains("currentBrief")) {
            planningPreferences.append(input.value("preference").toString());
            if (input.value("currentBrief").toObject().isEmpty()) {
                output = plan(input); output.insert("title", "跑步时刻"); output.insert("scene", "跑步");
                output.insert("styles", QJsonArray{"R&B"}); output.insert("queries", QJsonArray{"跑步 R&B"});
                output.insert("requireReleases", false); output.insert("artist", ""); output.insert("since", "");
                output.insert("until", ""); output.insert("targetMinutes", 20);
            } else {
                output = {{"kind", "research"}, {"changes", QJsonArray{QJsonObject{{"field", "targetMinutes"},
                    {"op", "set"}, {"value", 40}, {"evidence", "40分钟"}}}}, {"question", ""}, {"adjustment", "时长改为40分钟"}};
            }
        } else if (input.contains("scope") && input.contains("role") && !input.contains("result")) {
            output = {{"queries", input.value("scope").toObject().value("queries")}, {"artist", ""}, {"artistId", 0},
                {"since", ""}, {"until", ""}, {"note", "按跑步场景检索来源"}};
        } else if (input.contains("result") && input.contains("scope")) {
            output = {{"decision", "done"}, {"queries", QJsonArray{}}, {"note", "已有足够的场景候选"}};
        } else if (input.contains("candidates") && input.contains("brief")) {
            QJsonArray ordered;
            for (const auto &candidate : input.value("candidates").toArray()) ordered.append(candidate.toObject().value("index"));
            output = {{"order", ordered}};
        } else return {{{"error", "unexpected demo fixture prompt"}}, 500};
        return {{{"choices", QJsonArray{QJsonObject{{"finish_reason", "stop"}, {"message", QJsonObject{
            {"content", QString::fromUtf8(QJsonDocument(output).toJson(QJsonDocument::Compact))}}}}}},
            {"usage", QJsonObject{{"prompt_tokens", 100}, {"completion_tokens", 50}}}}};
    };
    music.handler = [](const FakeServer::Request &request) -> FakeServer::Response {
        if (request.path() == "/health") return {{{"app", "quiet-netease"}, {"version", 1}, {"catalogVersion", 2}, {"profileIsolation", 1}}};
        if (request.path() == "/account/status") return {{{"loggedIn", false}}};
        if (request.path() == "/radio/search") return {{{"playlists", QJsonArray{QJsonObject{{"id", 401}, {"name", "跑步 R&B"}}}}, {"total", 1}}};
        if (request.path() == "/radio/playlist") {
            QJsonArray tracks; for (int i = 1; i <= 12; ++i) tracks.append(song(5000 + i));
            return {{{"playlist", QJsonObject{{"id", 401}, {"name", "跑步 R&B"}, {"tags", QJsonArray{"跑步", "R&B"}}}}, {"songs", tracks}}};
        }
        return {{{"error", "unexpected demo catalog route"}}, 404};
    };
    environment.set("MUSICPLAYER_NETEASE_PORT", QByteArray::number(music.server.serverPort()));
    QVERIFY(QDir(data.path()).mkpath("netease")); QFile token(data.filePath("netease/token"));
    QVERIFY(token.open(QIODevice::WriteOnly)); token.write(QByteArray(64, 'a')); token.close();
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const auto port = reservation.serverPort(); reservation.close();
    const QString serverPath = QFINDTESTDATA("../backend_py/tango_backend/server.py"); QVERIFY(!serverPath.isEmpty());
    const QString backendDirectory = QDir::cleanPath(QFileInfo(serverPath).absolutePath() + "/..");
    const QString python = QDir(backendDirectory).filePath(".venv/bin/python"); QVERIFY(QFileInfo(python).isExecutable());
    OwnedBackend backend; QProcessEnvironment child;
    child.insert("QUIET_REGISTRATION_MODE", "open"); child.insert("PATH", "/usr/bin:/bin");
    child.insert("QUIET_BACKEND_HOST", "127.0.0.1"); child.insert("QUIET_BACKEND_PORT", QString::number(port));
    child.insert("QUIET_STORAGE_DRIVER", "sqlite"); child.insert("QUIET_DB_PATH", data.filePath("backend.sqlite"));
    child.insert("QWEN_API_KEY", "fake-demo-e2e-key"); child.insert("QWEN_BASE_URL", QString::fromLatin1(qwen.base() + "/v1"));
    child.insert("QWEN_MODEL", "qwen-demo-e2e-fixture"); child.insert("QUIET_AI_PER_MINUTE", "1000");
    backend.process.setProcessEnvironment(child); backend.process.setProcessChannelMode(QProcess::MergedChannels);
    backend.process.setWorkingDirectory(backendDirectory);
    const auto startBackend = [&] {
        backend.process.start(python, {"-m", "tango_backend.server", "--no-env"});
        return backend.process.waitForStarted(3000) && QTest::qWaitFor([&] {
            return ownedPythonBackendReady(backend.process, port);
        }, PythonBackendFixtureStartupTimeoutMs);
    };
    QVERIFY(startBackend()); environment.set("MUSICPLAYER_BACKEND_URL", "http://127.0.0.1:" + QByteArray::number(port));
    auto *account = AppBackendClient::instance(); QVERIFY(!account->authenticated());
    auto window = std::make_unique<MainWindow>(); window->show(); window->p_PlayerObject->setMuted(true);
    bool done = false; QString failure;
    account->registerUser("demo-chat-owner", "owned-fixture-password", window.get(), [&](const QString &error) { failure = error; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000); QVERIFY2(failure.isEmpty(), qPrintable(failure));
    const QString profile = window->accountProfile();
    const auto openChat = [&] {
        done = false; account->refreshAgentStatus(window.get(), [&](const QString &error) { failure = error; done = true; });
        if (!QTest::qWaitFor([&] { return done; }, 3000) || !failure.isEmpty()) return false;
        return QMetaObject::invokeMethod(window.get(), "openRadio", Qt::DirectConnection);
    };
    QVERIFY(openChat());
    auto *task = window->findChild<MusicTaskClient *>(); auto *conversation = window->findChild<ConversationClient *>();
    auto *dialog = window->findChild<RadioDialog *>(); QVERIFY(task && conversation && dialog);
    const auto sendChat = [&](const QString &text) {
        if (!QTest::qWaitFor([&] { return !conversation->busy() && !task->busy() && !conversation->snapshot().value("journalPending").toBool(); }, 3000)) return false;
        auto *input = dialog->findChild<QPlainTextEdit *>("edit_RadioInstruction");
        auto *send = dialog->findChild<QPushButton *>("button_RadioGenerate");
        if (!input || !send) return false;
        input->setPlainText(text);
        if (!send->isEnabled()) return false;
        QTest::keyClick(input, Qt::Key_Return);
        return QTest::qWaitFor([&] { return input->toPlainText().isEmpty(); }, 3000);
    };
    const auto chatContains = [&](const QString &text) {
        return dialog->findChild<QPlainTextEdit *>("text_RadioConversation")->toPlainText().contains(text);
    };
    const QString dependent = QStringLiteral("记住我喜欢粤语，确认保存后再推荐20分钟跑步歌单");
    QVERIFY(sendChat(dependent)); QTRY_VERIFY_WITH_TIMEOUT(chatContains("确认保存口味"), 3000);
    QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile), QString()); QVERIFY(task->snapshot().isEmpty());
    const int callsBeforeCancellation = qwen.requests.size();
    QCOMPARE(callsBeforeCancellation, 0);
    QVERIFY(sendChat("取消口味")); QTRY_VERIFY_WITH_TIMEOUT(chatContains("因此没有执行"), 3000);
    QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile), QString()); QVERIFY(task->snapshot().isEmpty());
    QCOMPARE(qwen.requests.size(), callsBeforeCancellation); QCOMPARE(music.count("/radio/search"), 0);
    QVERIFY(sendChat(dependent)); QTRY_VERIFY_WITH_TIMEOUT(chatContains("确认保存口味"), 3000);
    QVERIFY(task->snapshot().isEmpty()); QVERIFY(sendChat("确认保存口味"));
    QTRY_VERIFY_WITH_TIMEOUT(task->snapshot().value("status") == "preview" || task->snapshot().value("status") == "failed", 10000);
    QCOMPARE(task->snapshot().value("status").toString(), QString("preview"));
    QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile), QString("喜欢粤语"));
    QCOMPARE(planningPreferences, QStringList{QStringLiteral("喜欢粤语")});
    const auto first = task->snapshot(); const auto firstIds = ids(first); const QString taskId = first.value("id").toString();
    QCOMPARE(first.value("brief").toObject().value("targetMinutes").toInt(), 20); QCOMPARE(firstIds.size(), 5);
    QCOMPARE(first.value("preview").toObject().value("totalDurationMs").toInt(), 1200000);
    QCOMPARE(window->p_PlayerList->mediaCount(), 0);
    const int callsBeforeExplanation = qwen.requests.size(); const int lookupsBeforeExplanation = music.requests.size();
    QVERIFY(sendChat("解释当前歌单")); QTRY_VERIFY_WITH_TIMEOUT(chatContains("未分析音频"), 3000);
    QCOMPARE(qwen.requests.size(), callsBeforeExplanation); QCOMPARE(music.requests.size(), lookupsBeforeExplanation);
    QCOMPARE(task->snapshot().value("revision"), first.value("revision")); QCOMPARE(ids(task->snapshot()), firstIds);
    QVERIFY(sendChat("时长改成40分钟，其他不变"));
    QTRY_VERIFY_WITH_TIMEOUT(task->snapshot().value("preview").toObject().value("version").toInt() >= 2 || task->snapshot().value("status") == "failed", 10000);
    QCOMPARE(task->snapshot().value("status").toString(), QString("preview"));
    QCOMPARE(task->snapshot().value("brief").toObject().value("targetMinutes").toInt(), 40); QCOMPARE(ids(task->snapshot()).size(), 10);
    QCOMPARE(task->snapshot().value("brief").toObject().value("scene"), first.value("brief").toObject().value("scene"));
    QCOMPARE(task->snapshot().value("brief").toObject().value("styles"), first.value("brief").toObject().value("styles"));
    QCOMPARE(task->snapshot().value("preview").toObject().value("totalDurationMs").toInt(), 2400000);
    QCOMPARE(planningPreferences, (QStringList{QStringLiteral("喜欢粤语"), QStringLiteral("喜欢粤语")}));
    const int callsBeforeUndo = qwen.requests.size(); QVERIFY(sendChat("撤回修改"));
    QTRY_COMPARE_WITH_TIMEOUT(ids(task->snapshot()), firstIds, 3000);
    QCOMPARE(task->snapshot().value("brief").toObject().value("targetMinutes").toInt(), 20); QCOMPARE(qwen.requests.size(), callsBeforeUndo);
    QVERIFY(sendChat("暂停任务")); QTRY_COMPARE_WITH_TIMEOUT(task->snapshot().value("status").toString(), QString("paused"), 3000);
    const int callsBeforeRestart = qwen.requests.size();
    const int catalogBeforeRestart = music.count("/radio/search") + music.count("/radio/playlist");
    const auto conversationId = conversation->snapshot().value("id"); const QString deviceId = task->deviceId();
    done = false; account->logout(window.get(), [&](const QString &) { done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000); window.reset(); backend.stop(); QVERIFY(startBackend());
    window = std::make_unique<MainWindow>(); window->show(); window->p_PlayerObject->setMuted(true);
    done = false; account->login("demo-chat-owner", "owned-fixture-password", window.get(), [&](const QString &error) { failure = error; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000); QVERIFY2(failure.isEmpty(), qPrintable(failure)); QVERIFY(openChat());
    task = window->findChild<MusicTaskClient *>(); conversation = window->findChild<ConversationClient *>(); dialog = window->findChild<RadioDialog *>();
    QTRY_COMPARE_WITH_TIMEOUT(task->snapshot().value("status").toString(), QString("paused"), 4000);
    QCOMPARE(task->snapshot().value("id").toString(), taskId); QCOMPARE(task->deviceId(), deviceId);
    QCOMPARE(conversation->snapshot().value("id"), conversationId); QCOMPARE(qwen.requests.size(), callsBeforeRestart);
    QCOMPARE(music.count("/radio/search") + music.count("/radio/playlist"), catalogBeforeRestart);
    QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile), QString("喜欢粤语"));
    QVERIFY(sendChat("继续任务")); QTRY_COMPARE_WITH_TIMEOUT(task->snapshot().value("status").toString(), QString("preview"), 3000);
    QCOMPARE(ids(task->snapshot()), firstIds); QCOMPARE(qwen.requests.size(), callsBeforeRestart);
    QCOMPARE(music.count("/radio/search") + music.count("/radio/playlist"), catalogBeforeRestart);
    QCOMPARE(window->p_PlayerList->mediaCount(), 0);
    QVERIFY(chatContains("时长改成40分钟，其他不变")); QVERIFY(chatContains("确认保存口味"));
    done = false; account->logout(window.get(), [&](const QString &) { done = true; }); QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    window.reset(); backend.stop(); QVERIFY(!backend.process.readAll().contains("fake-demo-e2e-key"));
}

void MusicTaskFlowTest::expiredLocalLeasesPauseAndResumeWithoutServerRestart() {
    QTemporaryDir data;QVERIFY(data.isValid());ScopedEnvironment environment;
    environment.set("MUSICPLAYER_DATA_DIR",data.path().toUtf8());
    FakeServer qwen,music;bool hangTools=true;
    qwen.handler=[&](const FakeServer::Request &request)->FakeServer::Response {
        const auto messages=request.json().value("messages").toArray();
        const auto input=QJsonDocument::fromJson(messages.at(1).toObject().value("content").toString().toUtf8()).object();
        QJsonObject output;
        if(input.contains("currentBrief")) {
            output=plan(input);output.insert("requireReleases",false);output.insert("artist","");
        } else if(input.contains("scope")&&input.contains("role")&&!input.contains("result")) {
            output={{"queries",QJsonArray{"微醺 R&B"}},{"artist",""},{"artistId",0},{"since",""},{"until",""},{"note","场景来源研究"}};
        } else if(input.contains("result")&&input.contains("scope")) {
            output={{"decision","done"},{"queries",QJsonArray{}},{"note","候选足够"}};
        } else if(input.contains("candidates")&&input.contains("brief")) {
            QJsonArray ranked;for(const auto &row:input.value("candidates").toArray()){if(ranked.size()>=12)break;ranked.append(row.toObject().value("index"));}
            output={{"order",ranked}};
        } else return {{{"error","unexpected prompt"}},500};
        return {{{"choices",QJsonArray{QJsonObject{{"finish_reason","stop"},{"message",QJsonObject{
            {"content",QString::fromUtf8(QJsonDocument(output).toJson(QJsonDocument::Compact))}}}}}}}};
    };
    music.handler=[&](const FakeServer::Request &request)->FakeServer::Response {
        if(request.path()=="/health")return {{{"app","quiet-netease"},{"version",1},{"catalogVersion",2},{"profileIsolation",1}}};
        if(request.path()=="/radio/search") {
            if(hangTools)return {{},200,0,true};
            return {{{"playlists",QJsonArray{QJsonObject{{"id",101},{"name","微醺 R&B"}}}},{"total",1}}};
        }
        if(request.path()=="/radio/playlist") {
            QJsonArray tracks;for(int i=1;i<=12;++i)tracks.append(song(1000+i));
            return {{{"playlist",QJsonObject{{"id",101},{"name","微醺 R&B"},{"tags",QJsonArray{"微醺","R&B"}}}},{"songs",tracks}}};
        }
        return {{{"error","unexpected music route"}},404};
    };
    QDir(data.path()).mkpath("netease");QFile token(data.filePath("netease/token"));
    QVERIFY(token.open(QIODevice::WriteOnly));token.write(QByteArray(64,'e'));token.close();
    environment.set("MUSICPLAYER_NETEASE_PORT",QByteArray::number(music.server.serverPort()));
    QTcpServer reservation;QVERIFY(reservation.listen(QHostAddress::LocalHost,0));const auto port=reservation.serverPort();reservation.close();
    const QString serverPath=QFINDTESTDATA("../backend_py/tango_backend/server.py");QVERIFY(!serverPath.isEmpty());
    const QString backendDirectory=QDir::cleanPath(QFileInfo(serverPath).absolutePath()+"/..");
    const QString python=QDir(backendDirectory).filePath(".venv/bin/python");QVERIFY(QFileInfo(python).isExecutable());
    const QString database=data.filePath("backend.sqlite");OwnedBackend backend;QProcessEnvironment child;
    child.insert("QUIET_REGISTRATION_MODE","open");child.insert("PATH","/usr/bin:/bin");child.insert("QUIET_BACKEND_HOST","127.0.0.1");child.insert("QUIET_BACKEND_PORT",QString::number(port));
    child.insert("QUIET_DB_PATH",database);child.insert("QWEN_API_KEY","fake-lease-flow-key");
    child.insert("QUIET_STORAGE_DRIVER","sqlite");
    child.insert("QWEN_BASE_URL",QString::fromLatin1(qwen.base()+"/v1"));child.insert("QWEN_MODEL","qwen-lease-flow-fixture");
    child.insert("QUIET_AI_PER_MINUTE","1000");backend.process.setProcessEnvironment(child);backend.process.setProcessChannelMode(QProcess::MergedChannels);
    backend.process.setWorkingDirectory(backendDirectory);
    backend.process.start(python,{"-m","tango_backend.server","--no-env"});QVERIFY(backend.process.waitForStarted(3000));
    QTRY_VERIFY_WITH_TIMEOUT(ownedPythonBackendReady(backend.process,port),PythonBackendFixtureStartupTimeoutMs);
    const auto serverPid=backend.process.processId();
    environment.set("MUSICPLAYER_BACKEND_URL","http://127.0.0.1:"+QByteArray::number(port));
    auto *account=AppBackendClient::instance();QVERIFY(!account->authenticated());QObject context;
    bool done=false;QString failure;
    account->registerUser("lease-flow-owner","owned-fixture-password",&context,[&](const QString &error){failure=error;done=true;});
    QTRY_VERIFY_WITH_TIMEOUT(done,3000);QVERIFY2(failure.isEmpty(),qPrintable(failure));
    MusicTaskClient task(&context);task.submit(QStringLiteral("今晚微醺，安排约40分钟 R&B 歌单"));
    QTRY_COMPARE_WITH_TIMEOUT(task.snapshot().value("status").toString(),QString("waiting_tools"),5000);
    QTRY_VERIFY_WITH_TIMEOUT(music.count("/radio/search")>0,3000);
    const QString id=task.snapshot().value("id").toString();const int originalCalls=qwen.requests.size();QCOMPARE(originalCalls,2);
    const QString firstLease=task.snapshot().value("pendingSteps").toArray().first().toObject().value("leaseId").toString();
    // 仅修改测试临时库的租约截止时间，由真实 GET/latest 触发生产代码的过期处理。
    // 不读取真实账号，不修改后台时钟，也不等待完整租约超时。
    const QString expireScript=QStringLiteral(
        "import json,sqlite3,sys,time\n"
        "db=sqlite3.connect(sys.argv[1])\n"
        "row=db.execute('SELECT snapshot_json FROM music_tasks WHERE id=?',(sys.argv[2],)).fetchone()\n"
        "task=json.loads(row[0])\n"
        "for step in task['pendingSteps']: step['expiresAt']=int(time.time()*1000)-1000\n"
        "db.execute('UPDATE music_tasks SET snapshot_json=? WHERE id=?',(json.dumps(task),task['id']))\n"
        "db.commit()\n"
        "db.close()\n");
    const auto expire=[&] {
        QProcess fault;fault.setProcessEnvironment(child);fault.start(python,{"-c",expireScript,database,id});
        return fault.waitForFinished(3000)&&fault.exitStatus()==QProcess::NormalExit&&fault.exitCode()==0;
    };
    QVERIFY(expire());
    QJsonObject observed;done=false;
    account->musicTask(id,&context,[&](const QJsonObject &result,const QString &error){observed=result.value("task").toObject();failure=error;done=true;});
    QTRY_VERIFY_WITH_TIMEOUT(done,3000);QVERIFY2(failure.isEmpty(),qPrintable(failure));
    QCOMPARE(observed.value("status").toString(),QString("paused"));QVERIFY(observed.value("pendingSteps").toArray().isEmpty());
    QCOMPARE(qwen.requests.size(),originalCalls);QCOMPARE(backend.process.processId(),serverPid);
    task.restoreLatest();QTRY_COMPARE_WITH_TIMEOUT(task.snapshot().value("status").toString(),QString("paused"),3000);
    QTRY_VERIFY_WITH_TIMEOUT(!task.busy(),3000);task.action("resume");
    QTRY_COMPARE_WITH_TIMEOUT(task.snapshot().value("status").toString(),QString("waiting_tools"),3000);
    const QString secondLease=task.snapshot().value("pendingSteps").toArray().first().toObject().value("leaseId").toString();
    QVERIFY(secondLease!=firstLease);QCOMPARE(qwen.requests.size(),originalCalls);
    QVERIFY(expire());done=false;
    account->latestMusicTask(task.deviceId(),&context,[&](const QJsonObject &result,const QString &error){observed=result.value("task").toObject();failure=error;done=true;});
    QTRY_VERIFY_WITH_TIMEOUT(done,3000);QVERIFY2(failure.isEmpty(),qPrintable(failure));
    QCOMPARE(observed.value("status").toString(),QString("paused"));QCOMPARE(qwen.requests.size(),originalCalls);
    task.restoreLatest();QTRY_COMPARE_WITH_TIMEOUT(task.snapshot().value("status").toString(),QString("paused"),3000);
    QTRY_VERIFY_WITH_TIMEOUT(!task.busy(),3000);hangTools=false;task.action("resume");
    QTRY_VERIFY_WITH_TIMEOUT(task.snapshot().value("status")=="preview"||task.snapshot().value("status")=="failed",7000);
    QCOMPARE(task.snapshot().value("status").toString(),QString("preview"));QCOMPARE(ids(task.snapshot()).size(),10);
    QCOMPARE(task.snapshot().value("preview").toObject().value("totalDurationMs").toInt(),2400000);
    QCOMPARE(qwen.requests.size(),originalCalls+2);QCOMPARE(backend.process.processId(),serverPid);
    task.detach();account->logout(&context);QVERIFY(!account->authenticated());
}

void MusicTaskFlowTest::cleanup() {
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    if(QSqlDatabase::contains()) {
        QSqlDatabase::database(QSqlDatabase::defaultConnection,false).close();
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }
    NeteaseClient::setAccountProfile("guest");
}
QTEST_MAIN(MusicTaskFlowTest)
#include "music_task_flow_test.moc"
