#include "appbackendclient.h"
#include "mainwindow.h"
#include "musictaskclient.h"
#include "conversationclient.h"
#include "radiostore.h"
#include "radiodialog.h"
#include "neteaseclient.h"
#include "python_backend_fixture.h"
#include "songlistdelegate.h"
#include "music_identity_followup_helpers.h"
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextFragment>
#include <QTableView>
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
QJsonObject catalogSong(int id, const QString &title, qint64 artistId, const QString &artist, int duration=180000) {
    return {{"id",id},{"title",title},{"artist",artist},{"artists",QJsonArray{QJsonObject{{"id",artistId},{"name",artist}}}},
        {"album",QStringLiteral("散步作品集")},{"coverUrl",""},{"durationMs",duration},{"fee",0}};
}
QJsonArray songIds(const QVector<RadioRecommendation> &rows) {
    QJsonArray result; for(const auto &row:rows)result.append(row.song.id); return result;
}
QList<QUrl> songLinks(QTextBrowser *browser) {
    QList<QUrl> links;
    for(auto block=browser->document()->begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it){
        const auto fragment=it.fragment();const QUrl link(fragment.charFormat().anchorHref());
        if(fragment.isValid()&&link.scheme()=="tango-track"&&!links.contains(link))links.append(link);
    }
    return links;
}
QPoint songPoint(QTextBrowser *browser,const QUrl &link) {
    for(auto block=browser->document()->begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it){
        const auto fragment=it.fragment();if(fragment.isValid()&&fragment.charFormat().anchorHref()==link.toString()){
            QTextCursor cursor(browser->document());cursor.setPosition(fragment.position()+qMin(1,fragment.length()-1));
            browser->setTextCursor(cursor);browser->ensureCursorVisible();return browser->cursorRect(cursor).center();
        }
    }
    return {-1,-1};
}
}

// 预先固定的业务判据只验正式链路，固定模型提案不能作为语义准确率。
class MusicIdentityFollowupFlowTest : public QObject {
    Q_OBJECT
    QTemporaryDir data;
    ScopedEnvironment environment;
    FakeServer model,music;
    OwnedBackend backend;
    std::unique_ptr<MainWindow> window;
    AppBackendClient *account=nullptr;
    QVector<QJsonObject> modelInputs;
    bool shortCatalog=false,ambiguousArtists=false,identityFailure=false,identityWithoutMatch=false;
    int delayedArtistMs=0,delayedPlaylistMs=0,sequence=0;
    QString profile,accountName;
    QJsonArray artistRows(qint64 artistId) const {
        QJsonArray rows;const QString name=artistId==7763?QStringLiteral("G.E.M.邓紫棋"):QStringLiteral("周杰伦");const int base=artistId==7763?7000:8000;
        // 把错误署名放在最前面，首屏也必须真正执行ID过滤。
        rows.append(catalogSong(base+90,QStringLiteral("同名误收作品"),99999,name));
        rows.append(catalogSong(base+91,QStringLiteral("其他歌手作品"),111,QStringLiteral("其他歌手")));
        for(int i=1;i<=(shortCatalog?3:14);++i)rows.append(catalogSong(base+i,QStringLiteral("散步作品 %1").arg(i),artistId,name,200000));
        return rows;
    }
    QJsonObject modelProposal(const QJsonObject &input) {
        const auto text=input.value("instruction").toString();
        if(input.contains("conversationId")&&input.contains("messageId")&&input.contains("task")){
            if(text.contains(QStringLiteral("他说"))||text.startsWith(QStringLiteral("不要")))
                return {{"intents",QJsonArray{"clarification"}},{"slots",QJsonObject{{"artist",""},{"targetMinutes",0}}},{"missingSlots",QJsonArray{}},
                    {"actions",QJsonArray{QJsonObject{{"capability","assistant.reply"},{"source",text},{"dependsOn",QJsonArray{}},
                        {"parameters",QJsonObject{{"kind","clarify"},{"text",QStringLiteral("这里只转述或限制找歌，没有直接执行请求。")}}}}}}};
            return {{"intents",QJsonArray{"scene_research"}},{"slots",QJsonObject{{"artist","邓紫棋"},{"targetMinutes",30}}},
                {"missingSlots",QJsonArray{}},{"actions",QJsonArray{QJsonObject{{"capability","music.task"},{"source",text},{"dependsOn",QJsonArray{}},
                    {"parameters",QJsonObject{{"mode",input.value("task").isObject()?"continue":"new"}}}}}}};
        }
        if(input.contains("currentBrief")){
            if(!input.value("currentBrief").toObject().isEmpty()){
                QJsonArray changes;
                if(text.contains("20"))changes.append(QJsonObject{{"field","targetMinutes"},{"op","set"},{"value",20},{"evidence",QStringLiteral("20分钟")}});
                if(text.contains(QStringLiteral("周杰伦")))changes.append(QJsonObject{{"field","requiredArtist"},{"op","set"},{"value","周杰伦"},{"evidence",text}});
                return {{"kind","research"},{"changes",changes},{"question",""},{"adjustment",""}};
            }
            // 故意遗漏三个显式条件：统一需求编译不得静默接受这份错误规划。
            return {{"kind","research"},{"title","缺槽提案"},{"scene",""},{"styles",QJsonArray{}},{"queries",QJsonArray{"轻音乐"}},
                {"artist",""},{"requiredArtist",""},{"artistOnly",false},{"targetMinutes",0},{"excludeLive",false},{"excludedArtists",QJsonArray{}},
                {"requireReleases",false},{"retainFirst",0},{"removeIds",QJsonArray{}},{"question",""},{"adjustment",""}};
        }
        if(input.contains("scope")&&input.contains("role")&&!input.contains("result")){
            const auto scope=input.value("scope").toObject();return {{"queries",scope.value("queries")},{"artist",scope.value("requiredArtist").toString()},{"artistId",0},
                {"since",scope.value("since")},{"until",scope.value("until")},{"note",QStringLiteral("核对歌手作品和散步场景来源")}};
        }
        if(input.contains("result")&&input.contains("scope"))return {{"decision","done"},{"queries",QJsonArray{}},{"note",QStringLiteral("来源核验完成")}};
        if(input.contains("candidates")&&input.contains("brief")){QJsonArray order;for(const auto &row:input.value("candidates").toArray())order.append(row.toObject().value("index"));return {{"order",order}};}
        return {};
    }
    RadioDialog *dialog()const{return window->findChild<RadioDialog*>();}
    MusicTaskClient *task()const{return window->findChild<MusicTaskClient*>();}
    ConversationClient *conversation()const{return window->findChild<ConversationClient*>();}
    QString transcript()const{auto *view=window->findChild<QTextBrowser*>("browser_RadioConversation");return view?view->toPlainText():QString();}
    bool settled(int timeout=6000)const{return QTest::qWaitFor([&]{return conversation()&&task()&&!conversation()->busy()&&!conversation()->snapshot().value("journalPending").toBool()&&!task()->busy();},timeout);}
    bool send(const QString &text){
        if(!settled())return false;auto *input=dialog()->findChild<QPlainTextEdit*>("edit_RadioInstruction");auto *button=dialog()->findChild<QPushButton*>("button_RadioGenerate");
        if(!input||!button)return false;input->setPlainText(text);if(!button->isEnabled())return false;button->click();return QTest::qWaitFor([&]{return input->toPlainText().isEmpty();},3000);
    }
    bool waitPreview(int previousVersion=0){return QTest::qWaitFor([&]{const auto snap=task()->snapshot();return snap.value("status")=="preview"&&snap.value("preview").toObject().value("version").toInt()>previousVersion&&settled(100);},15000);}
    QJsonObject check(const QString &key)const{for(const auto &row:task()->snapshot().value("preview").toObject().value("checks").toArray())if(row.toObject().value("key")==key)return row.toObject();return {};}
    void noImplicitMutation(){
        auto *queue=window->findChild<QTableView*>("queueView");QVERIFY(queue);QCOMPARE(queue->model()->rowCount(),0);QCOMPARE(window->p_PlayerObject->state(),QMediaPlayer::StoppedState);
        QCOMPARE(music.count("/song/url"),0);QCOMPARE(RadioStore(QSqlDatabase::database()).preference(profile),QString());
    }
private slots:
    void initTestCase(){
        QVERIFY(data.isValid());QVERIFY(model.server.isListening());QVERIFY(music.server.isListening());
        environment.set("MUSICPLAYER_DATA_DIR",data.path().toUtf8());environment.set("MUSICPLAYER_REDIS_PORT","invalid-intent-test-port");environment.set("MUSICPLAYER_DEVELOPMENT_MODE","");environment.set("MUSICPLAYER_DEVELOPMENT_TOKEN_FILE","");
        model.handler=[&](const FakeServer::Request &request)->FakeServer::Response{
            const auto messages=request.json().value("messages").toArray();const auto input=QJsonDocument::fromJson(messages.at(1).toObject().value("content").toString().toUtf8()).object();
            modelInputs.append(input);const auto proposal=modelProposal(input);if(proposal.isEmpty())return {{{"error",QJsonObject{{"message","unrecognized-owned-fixture-input"}}}},500};
            return {{{"choices",QJsonArray{QJsonObject{{"finish_reason","stop"},{"message",QJsonObject{{"content",QString::fromUtf8(QJsonDocument(proposal).toJson(QJsonDocument::Compact))}}}}}},
                {"usage",QJsonObject{{"prompt_tokens",100},{"completion_tokens",50}}}},200,15};
        };
        const auto audio=wave();music.handler=[&,audio](const FakeServer::Request &request)->FakeServer::Response{
            const auto path=request.path(),keyword=request.query().queryItemValue("keywords");
            if(path=="/health")return {{{"app","quiet-netease"},{"version",1},{"catalogVersion",2},{"profileIsolation",1}}};
            if(path=="/account/status")return {{{"loggedIn",false},{"nickname",""},{"avatarUrl",""},{"vipType",0}}};
            if(path=="/assistant/artists"){
                if(identityFailure)return {{{"error",QJsonObject{{"message","owned-identity-service-unavailable"}}}},503};
                if(identityWithoutMatch)return {{{"artists",QJsonArray{}}}};
                // 两项都匹配原请求，真正GEM固定在第2项，不能自动采用榜一。
                const QJsonArray artists{
                    QJsonObject{{"id",99881},{"name","邓紫棋"},{"aliases",QJsonArray{"邓紫棋"}}},
                    QJsonObject{{"id",7763},{"name","G.E.M.邓紫棋"},{"aliases",QJsonArray{"邓紫棋"}}}};
                return {{{"artists",artists}},200,delayedArtistMs};
            }
            if(path=="/artist/tracks"){
                const auto id=request.query().queryItemValue("artistId").toLongLong();const auto allRows=artistRows(id);
                const int offset=request.query().queryItemValue("offset").toInt(),limit=request.query().queryItemValue("limit").toInt();
                // 故意在只读作品响应夹带两首错误ID并超出请求上限，验客户端仍只显示12首正确署名。
                // 正式分页契约由service专项覆盖，此夹具只提供可控的身份/显示上限对抗。
                QJsonArray rows;for(int i=offset;i<allRows.size();++i)rows.append(allRows.at(i));
                return {{{"artistId",id},{"artist",QJsonObject{{"id",id},{"name",id==7763?"G.E.M.邓紫棋":"周杰伦"}}},{"songs",rows},{"total",allRows.size()},{"offset",offset},{"limit",limit},{"more",offset+limit<allRows.size()},{"checkedAt",QDateTime::currentMSecsSinceEpoch()}}};
            }
            if(path=="/search"){
                if(keyword.contains(QStringLiteral("十年")))return {{{"songs",QJsonArray{catalogSong(1001,"十年",2116,"陈奕迅"),catalogSong(1002,"十年",999,"其他歌手")}},{"total",2}}};
                return {{{"songs",artistRows(keyword.contains(QStringLiteral("周杰伦"))?6452:7763)},{"total",16}}};
            }
            if(path=="/radio/search")return {{{"playlists",QJsonArray{QJsonObject{{"id",6001},{"name","邓紫棋 散步时刻"},{"tags",QJsonArray{"散步","轻松"}}}}},{"total",1}},200,delayedPlaylistMs};
            if(path=="/radio/playlist")return {{{"playlist",QJsonObject{{"id",6001},{"name","邓紫棋 散步时刻"},{"tags",QJsonArray{"散步","轻松"}}}},{"songs",artistRows(7763)}}};
            if(path=="/song/url")return {{{"id",request.query().queryItemValue("id").toInt()},{"url",QString::fromLatin1(music.base()+"/audio.wav")},{"trial",false},{"durationMs",60000},{"level","standard"}}};
            if(path=="/lyrics"||path=="/lyric")return {{{"lyric","[00:00]离线测试"}}};
            if(path=="/audio.wav"){FakeServer::Response response;response.raw=audio;response.contentType="audio/wav";return response;}
            return {{{"error",QJsonObject{{"message","unexpected-owned-catalog-route"}}}},404};
        };
        environment.set("MUSICPLAYER_NETEASE_PORT",QByteArray::number(music.server.serverPort()));QDir(data.path()).mkpath("netease");QFile token(data.filePath("netease/token"));QVERIFY(token.open(QIODevice::WriteOnly));token.write("owned-identity-followup-token");token.close();
        QTcpServer reserve;QVERIFY(reserve.listen(QHostAddress::LocalHost,0));const auto port=reserve.serverPort();reserve.close();
        const auto serverPath=QFINDTESTDATA("../backend_py/tango_backend/server.py");QVERIFY(!serverPath.isEmpty());const auto directory=QDir::cleanPath(QFileInfo(serverPath).absolutePath()+"/..");const auto python=QDir(directory).filePath(".venv/bin/python");QVERIFY(QFileInfo(python).isExecutable());
        QProcessEnvironment child;child.insert("PATH","/usr/bin:/bin");child.insert("QUIET_REGISTRATION_MODE","open");child.insert("QUIET_BACKEND_HOST","127.0.0.1");child.insert("QUIET_BACKEND_PORT",QString::number(port));child.insert("QUIET_DB_PATH",data.filePath("backend.sqlite"));child.insert("QUIET_STORAGE_DRIVER","sqlite");
        child.insert("QWEN_API_KEY","owned-not-a-real-api-key");child.insert("QWEN_BASE_URL",QString::fromLatin1(model.base()+"/v1"));child.insert("QWEN_MODEL","owned-identity-followup-fixture");child.insert("QUIET_AI_PER_MINUTE","1000");
        backend.process.setProcessEnvironment(child);backend.process.setProcessChannelMode(QProcess::MergedChannels);backend.process.setWorkingDirectory(directory);backend.process.start(python,{"-m","tango_backend.server","--no-env"});QVERIFY(backend.process.waitForStarted(3000));
        QVERIFY2(QTest::qWaitFor([&]{return ownedPythonBackendReady(backend.process,port);},PythonBackendFixtureStartupTimeoutMs),backend.process.readAll().constData());environment.set("MUSICPLAYER_BACKEND_URL","http://127.0.0.1:"+QByteArray::number(port));account=AppBackendClient::instance();
    }
    void init(){
        shortCatalog=false;ambiguousArtists=false;identityFailure=false;identityWithoutMatch=false;delayedArtistMs=0;delayedPlaylistMs=0;modelInputs.clear();model.requests.clear();music.requests.clear();window=std::make_unique<MainWindow>();window->resize(1220,860);window->show();window->p_PlayerObject->setMuted(true);
        bool done=false;QString failure;accountName=QStringLiteral("identity-owner-%1").arg(++sequence);account->registerUser(accountName,"owned-fixture-password",window.get(),[&](const QString &error){failure=error;done=true;});QTRY_VERIFY_WITH_TIMEOUT(done,5000);QVERIFY2(failure.isEmpty(),qPrintable(failure));profile=window->accountProfile();
        done=false;account->refreshAgentStatus(window.get(),[&](const QString &error){failure=error;done=true;});QTRY_VERIFY_WITH_TIMEOUT(done,3000);QVERIFY2(failure.isEmpty(),qPrintable(failure));QVERIFY(account->routingAvailable());QVERIFY(QMetaObject::invokeMethod(window.get(),"openRadio",Qt::DirectConnection));QVERIFY(settled());
    }
    void cleanup(){
        if(window&&QTest::currentTestFailed())qWarning().noquote()<<"隔离验收失败状态:"<<QJsonDocument(task()->snapshot()).toJson(QJsonDocument::Compact)<<"\n聊天:"<<transcript();
        if(window)window->p_PlayerObject->stop();if(account&&account->authenticated()){bool done=false;account->logout(this,[&](const QString &){done=true;});if(!QTest::qWaitFor([&]{return done;},3000))qWarning("隔离测试退出登录未完成");}window.reset();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        if(QSqlDatabase::contains()){QSqlDatabase::database(QSqlDatabase::defaultConnection,false).close();QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);}NeteaseClient::setAccountProfile("guest");
    }
    void verifiedCandidateInSecondPositionContinuesOriginalArtistCatalog(){
        QVERIFY(send(QStringLiteral("帮我找一些邓紫棋的歌曲")));
        QTRY_VERIFY_WITH_TIMEOUT(!window->property("tangoCatalogChoices").toJsonObject().isEmpty(),5000);
        const auto candidates=window->property("tangoCatalogChoices").toJsonObject().value("artists").toArray();
        QCOMPARE(candidates.size(),2);
        const int ordinal=IdentityFollowupProbe::verifiedArtistOrdinal(candidates);
        QCOMPARE(ordinal,2);
        QCOMPARE(candidates.at(ordinal-1).toObject().value("id").toInt(),7763);
        QCOMPARE(candidates.at(ordinal-1).toObject().value("name").toString(),QStringLiteral("G.E.M.邓紫棋"));
        // 错误ID或只有别名相似不能由测试探针授予选择权限。
        QCOMPARE(IdentityFollowupProbe::verifiedArtistOrdinal(QJsonArray{QJsonObject{{"id",99881},{"name","G.E.M.邓紫棋"}}}),0);
        QCOMPARE(IdentityFollowupProbe::verifiedArtistOrdinal(QJsonArray{QJsonObject{{"id",7763},{"name","其他歌手"},{"aliases",QJsonArray{"邓紫棋"}}}}),0);
        QCOMPARE(music.count("/artist/tracks"),0);QVERIFY(dialog()->recommendations().isEmpty());noImplicitMutation();
        QVERIFY(transcript().contains(QStringLiteral("1. 邓紫棋")));QVERIFY(transcript().contains(QStringLiteral("2. G.E.M.邓紫棋")));
        QVERIFY(send(QStringLiteral("选择歌手 %1").arg(ordinal)));
        QTRY_COMPARE_WITH_TIMEOUT(dialog()->recommendations().size(),12,5000);QVERIFY(settled());
        QCOMPARE(music.last("/artist/tracks").query().queryItemValue("artistId"),QString("7763"));
        QCOMPARE(music.last("/artist/tracks").query().queryItemValue("limit"),QString("12"));
        for(const auto &row:dialog()->recommendations()){
            QVERIFY(row.song.id>=7001&&row.song.id<=7014);
            QCOMPARE(row.song.artist,QStringLiteral("G.E.M.邓紫棋"));
        }
        QVERIFY(window->property("tangoCatalogChoices").toJsonObject().isEmpty());
        QVERIFY(task()->snapshot().value("id").toString().isEmpty());noImplicitMutation();
    }
    void compositeClarificationThenDurationRevisionPreservesTaskAndAllConstraints(){
        QVERIFY(send(QStringLiteral("我在散步，请你给我推荐邓紫棋的歌曲，给我形成一个30分钟的歌单。")));
        QTRY_COMPARE_WITH_TIMEOUT(task()->snapshot().value("status").toString(),QString("needs_clarification"),12000);
        QVERIFY(settled());const auto before=task()->snapshot();
        const QString taskId=before.value("id").toString();QVERIFY(!taskId.isEmpty());
        const auto candidates=before.value("providedArtists").toArray();QCOMPARE(candidates.size(),2);
        const int ordinal=IdentityFollowupProbe::verifiedArtistOrdinal(candidates);QCOMPARE(ordinal,2);
        QCOMPARE(candidates.at(ordinal-1).toObject().value("id").toInt(),7763);
        QCOMPARE(candidates.at(ordinal-1).toObject().value("name").toString(),QStringLiteral("G.E.M.邓紫棋"));
        QVERIFY(before.value("preview").toObject().isEmpty());QVERIFY(dialog()->recommendations().isEmpty());
        QCOMPARE(music.count("/artist/tracks"),0);QCOMPARE(music.count("/radio/playlist"),0);noImplicitMutation();
        QVERIFY(send(QStringLiteral("选择歌手 %1").arg(ordinal)));QVERIFY2(waitPreview(),qPrintable(transcript()));
        auto snapshot=task()->snapshot();auto brief=snapshot.value("brief").toObject();
        QCOMPARE(snapshot.value("id").toString(),taskId);QCOMPARE(brief.value("requiredArtistId").toInt(),7763);
        QCOMPARE(brief.value("requiredArtist").toString(),QStringLiteral("G.E.M.邓紫棋"));
        QCOMPARE(brief.value("scene").toString(),QStringLiteral("散步"));QCOMPARE(brief.value("targetMinutes").toInt(),30);
        QCOMPARE(snapshot.value("preview").toObject().value("totalDurationMs").toInt(),30*60000);
        QCOMPARE(check("duration").value("status").toString(),QString("satisfied"));
        QCOMPARE(check("scene").value("status").toString(),QString("satisfied"));
        for(const auto &row:dialog()->recommendations())QVERIFY(row.song.id>=7001&&row.song.id<=7014);
        const int version=snapshot.value("preview").toObject().value("version").toInt();
        QVERIFY(send(QStringLiteral("改成20分钟，其他不变")));QVERIFY2(waitPreview(version),qPrintable(transcript()));
        snapshot=task()->snapshot();brief=snapshot.value("brief").toObject();
        QCOMPARE(snapshot.value("id").toString(),taskId);QCOMPARE(brief.value("requiredArtistId").toInt(),7763);
        QCOMPARE(brief.value("requiredArtist").toString(),QStringLiteral("G.E.M.邓紫棋"));
        QCOMPARE(brief.value("scene").toString(),QStringLiteral("散步"));QCOMPARE(brief.value("targetMinutes").toInt(),20);
        QCOMPARE(snapshot.value("preview").toObject().value("totalDurationMs").toInt(),20*60000);
        QCOMPARE(check("duration").value("status").toString(),QString("satisfied"));
        for(const auto &row:dialog()->recommendations())QVERIFY(row.song.id>=7001&&row.song.id<=7014);
        noImplicitMutation();
    }
};
QTEST_MAIN(MusicIdentityFollowupFlowTest)
#include "music_identity_followup_flow_test.moc"
