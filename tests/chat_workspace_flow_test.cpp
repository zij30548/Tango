#include "routing_fixtures.h"
#include "mainwindow.h"
#include "radiodialog.h"
#include "radiostore.h"
#include "songlistdelegate.h"
#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDataStream>
#include <QCryptographicHash>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTableView>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextTable>
#include <QUrlQuery>

using namespace RoutingFixtures;

namespace {
// 曲库与音频都由本测试持有的回环端口提供，不接真实网易云或用户授权。
class OwnedCatalog : public QObject {
public:
    QTcpServer server;
    QList<qint64> resolved;
    QJsonArray songs{QJsonObject{{"id",101},{"title",QStringLiteral("晚风来信")},{"artist",QStringLiteral("阔叶电台")},
        {"album",QStringLiteral("日落之后")},{"durationMs",180000},{"version","studio"}},
        QJsonObject{{"id",102},{"title",QStringLiteral("晚风来信")},{"artist",QStringLiteral("南渡")},
        {"album",QStringLiteral("日落之后")},{"durationMs",180000},{"version","studio"}}};
    explicit OwnedCatalog(Fixture &fixture) {
        server.listen(QHostAddress::LocalHost,0);
        fixture.environment.set("MUSICPLAYER_NETEASE_PORT",QByteArray::number(server.serverPort()));
        QDir().mkpath(fixture.directory.filePath("netease"));
        QFile token(fixture.directory.filePath("netease/token"));
        if(token.open(QIODevice::WriteOnly))token.write("owned-chat-workspace-fixture");
        connect(&server,&QTcpServer::newConnection,this,[this]{
            while(server.hasPendingConnections()){
                auto *socket=server.nextPendingConnection();
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                auto bytes=std::make_shared<QByteArray>();auto handled=std::make_shared<bool>(false);
                connect(socket,&QTcpSocket::readyRead,this,[this,socket,bytes,handled]{
                    bytes->append(socket->readAll());if(*handled || !bytes->contains("\r\n\r\n"))return;*handled=true;
                    const auto target=QUrl::fromEncoded(bytes->split(' ').value(1));QJsonObject body;QByteArray raw;QByteArray type="application/json";
                    if(target.path()=="/health")body={{"app","quiet-netease"},{"version",1},{"catalogVersion",2},{"profileIsolation",1}};
                    else if(target.path()=="/search")body={{"songs",songs},{"total",songs.size()}};
                    else if(target.path()=="/song/url"){
                        resolved.append(QUrlQuery(target).queryItemValue("id").toLongLong());
                        body={{"url",QString("http://127.0.0.1:%1/audio.wav").arg(server.serverPort())},{"trial",false}};
                    } else if(target.path()=="/audio.wav"){raw=wave();type="audio/wav";}
                    else if(target.path()=="/lyric")body={{"lyric",""}};
                    else body={{"loggedIn",false}};
                    if(raw.isEmpty())raw=QJsonDocument(body).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: "+type+"\r\nConnection: close\r\nContent-Length: "+QByteArray::number(raw.size())+"\r\n\r\n"+raw);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    static QByteArray wave(){
        constexpr quint32 rate=22050,size=rate*12*sizeof(qint16);QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);
        QDataStream stream(&buffer);stream.setByteOrder(QDataStream::LittleEndian);stream.writeRawData("RIFF",4);stream<<quint32(36+size);
        stream.writeRawData("WAVEfmt ",8);stream<<quint32(16)<<quint16(1)<<quint16(1)<<rate<<quint32(rate*2)<<quint16(2)<<quint16(16);
        stream.writeRawData("data",4);stream<<size;bytes.append(QByteArray(size,'\0'));return bytes;
    }
};

// 只模拟客户端可观察的删除契约；数据库事务和跨进程行为另由后台专项验证。
class ClearFixture {
public:
    Fixture backend{true};
    QHash<QString,QJsonObject> clearReceipts;
    int clearFailures=0;
    explicit ClearFixture(){
        const auto original=backend.server.handler;
        backend.server.handler=[this,original](const Request &request)->Response{
            if(request.method=="DELETE" && request.path=="/v1/conversations"){
                if(request.body.keys()!=QStringList{QStringLiteral("requestId")}||request.body.value("requestId").toString().isEmpty())
                    return {{{"error",QJsonObject{{"message",QStringLiteral("删除契约只能携带原请求标识")}}}},400};
                if(clearFailures>0){--clearFailures;return {{{"error",QJsonObject{{"message",QStringLiteral("隔离测试删除失败")}}}},503};}
                const auto key=backend.accountName+":"+request.body.value("requestId").toString();
                if(!clearReceipts.contains(key)){
                    const int count=backend.current.isEmpty()?0:1;
                    backend.current={};backend.accountConversations.remove(backend.accountName);
                    clearReceipts.insert(key,{{"cleared",true},{"requestId",request.body.value("requestId")},{"conversationCount",count},
                        {"clearedAt","2030-01-01T00:00:00Z"},{"cancelledTaskCount",0}});
                }
                return {clearReceipts.value(key)};
            }
            return original(request);
        };
    }
};
}

class ChatWorkspaceFlowTest : public QObject {
    Q_OBJECT
    bool open(Fixture &fixture,MainWindow &window,const QString &username=QStringLiteral("fixture")){
        window.resize(1220,860);window.show();if(!fixture.login(username)||!QMetaObject::invokeMethod(&window,"openRadio",Qt::DirectConnection))return false;
        return settled(window);
    }
    bool settled(MainWindow &window){
        auto *client=window.findChild<ConversationClient*>();auto *task=window.findChild<MusicTaskClient*>();
        return client&&task&&QTest::qWaitFor([&]{return !client->busy()&&!client->snapshot().value("journalPending").toBool()&&!task->busy();},2500);
    }
    bool send(Fixture &fixture,MainWindow &window,const QString &text){
        if(!settled(window))return false;const auto old=fixture.messageId;auto *dialog=window.findChild<RadioDialog*>();
        dialog->findChild<QPlainTextEdit*>("edit_RadioInstruction")->setPlainText(text);
        auto *button=dialog->findChild<QPushButton*>("button_RadioGenerate");if(!button->isEnabled())return false;button->click();
        return QTest::qWaitFor([&]{return fixture.messageId!=old&&settled(window);},2500);
    }
    QString plain(MainWindow &window){return window.findChild<QTextBrowser*>("browser_RadioConversation")->toPlainText();}
    void close(std::unique_ptr<MainWindow> &window){
        window.reset();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        if(QSqlDatabase::contains()){QSqlDatabase::database(QSqlDatabase::defaultConnection,false).close();QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);}
    }
    QList<QUrl> trackLinks(QTextBrowser *browser){
        QList<QUrl> result;for(auto block=browser->document()->begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it){
            const auto fragment=it.fragment();const auto format=fragment.charFormat();const QUrl link(format.anchorHref());
            if(fragment.isValid()&&format.isAnchor()&&link.scheme()=="tango-track"&&!result.contains(link))result.append(link);
        }return result;
    }
    QPoint anchorPoint(QTextBrowser *browser,const QUrl &url){
        for(auto block=browser->document()->begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it){
            const auto fragment=it.fragment();if(fragment.isValid()&&fragment.charFormat().anchorHref()==url.toString()){
                QTextCursor cursor(browser->document());cursor.setPosition(fragment.position()+qMin(1,fragment.length()-1));browser->setTextCursor(cursor);browser->ensureCursorVisible();
                return browser->cursorRect(cursor).center()+QPoint(3,0);
            }
        }return QPoint(-1,-1);
    }
    void lookup(Fixture &fixture){fixture.makePlan=[](const QString &mid,const QString &text){return Fixture::plan(mid,{"song_lookup"},
        {Fixture::action("lookup","music.lookup",text,{{"artist",""},{"title",QStringLiteral("晚风来信")},{"play",false}})});};}
    bool confirmClear(MainWindow &window,bool yes){
        auto *button=window.findChild<QPushButton*>("button_ConversationClearAll");if(!button||!button->isEnabled())return false;
        bool handled=false,safeDefault=false;QTimer::singleShot(0,&window,[&]{
            auto *box=window.findChild<QMessageBox*>("messageBox_ClearConversations");
            if(!box)for(auto *widget:QApplication::topLevelWidgets())if(widget->objectName()=="messageBox_ClearConversations")box=qobject_cast<QMessageBox*>(widget);
            if(box){handled=true;safeDefault=box->defaultButton()==box->button(QMessageBox::Cancel);box->button(yes?QMessageBox::Yes:QMessageBox::Cancel)->click();}
        });button->click();return handled&&safeDefault;
    }
private slots:
    void cleanup(){QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);if(QSqlDatabase::contains()){
        QSqlDatabase::database(QSqlDatabase::defaultConnection,false).close();QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);}}
    void userAndAssistantHaveVisibleSeparateBubbles();
    void recommendationSingleClickPlaysOnlySelectedSong();
    void taskPreviewClickPlaysOnlyOneWithoutAdoption();
    void oldRecommendationAnchorCannotPlayAfterReplacement();
    void clearingCanBeCancelledAndPreservesPreferencesAndQueue();
    void clearingSurvivesReopenAndKeepsOtherAccount();
    void clearFailureKeepsHistoryAndRetryUsesOriginalRequest();
    void clearingDropsLateRouteReply();
    void clearConfirmationCannotDeleteSwitchedAccount();
    void uncertainClearKeepsOriginalIdAcrossClientRestart();
    void clearingCancelsOldQuotaObservationAndAllowsFreshReads();
    void oldClearResponseCannotRemoveNewerClientsPendingId();
};

void ChatWorkspaceFlowTest::userAndAssistantHaveVisibleSeparateBubbles(){
    Fixture fixture(true);MainWindow window;QVERIFY(open(fixture,window));
    fixture.makePlan=[](const QString &mid,const QString &text){return Fixture::plan(mid,{"help"},{Fixture::action("reply","assistant.reply",text,
        {{"kind","help"},{"text",QStringLiteral("我可以帮你找歌、调整歌单，也能记住经过你确认的音乐偏好。")}})});};
    QVERIFY(send(fixture,window,QStringLiteral("今晚想听些轻松的音乐，你可以帮我吗？")));
    auto *browser=window.findChild<QTextBrowser*>("browser_RadioConversation");QVERIFY(browser);QVERIFY(browser->isVisible());
    QVERIFY(window.findChild<QPlainTextEdit*>("text_RadioConversation")->isHidden());
    QVERIFY(browser->toPlainText().contains(QStringLiteral("今晚想听")));QVERIFY(browser->toPlainText().contains(QStringLiteral("我可以帮你找歌")));
    const auto userCursor=browser->document()->find(QStringLiteral("今晚想听些轻松的音乐"));
    const auto assistantCursor=browser->document()->find(QStringLiteral("我可以帮你找歌"));
    QVERIFY(!userCursor.isNull());QVERIFY(!assistantCursor.isNull());auto *userBubble=userCursor.currentTable();auto *assistantBubble=assistantCursor.currentTable();
    QVERIFY(userBubble);QVERIFY(assistantBubble);QVERIFY(userBubble!=assistantBubble);
    const auto userRect=browser->document()->documentLayout()->frameBoundingRect(userBubble);
    const auto assistantRect=browser->document()->documentLayout()->frameBoundingRect(assistantBubble);
    QVERIFY2(userRect.left()>assistantRect.left(),"用户气泡必须实际位于助手气泡右侧");
    QVERIFY2(userRect.right()>assistantRect.right(),"用户气泡右边缘必须比助手靠右");
    for(auto *bubble:{userBubble,assistantBubble}){QVERIFY(bubble->format().border()>0);QVERIFY(bubble->format().background().style()!=Qt::NoBrush);}
    const auto screenshot=qEnvironmentVariable("TANGO_CHAT_SCREENSHOT");if(!screenshot.isEmpty())QVERIFY(window.grab().save(screenshot));
}

void ChatWorkspaceFlowTest::recommendationSingleClickPlaysOnlySelectedSong(){
    Fixture fixture(true);OwnedCatalog catalog(fixture);MainWindow window;QVERIFY(open(fixture,window));lookup(fixture);
    QVERIFY(send(fixture,window,QStringLiteral("查找晚风来信，先别播放")));auto *dialog=window.findChild<RadioDialog*>();QTRY_COMPARE(dialog->recommendations().size(),2);
    auto *queue=window.findChild<QTableView*>("queueView");QCOMPARE(queue->model()->rowCount(),0);QVERIFY(catalog.resolved.isEmpty());
    auto *browser=window.findChild<QTextBrowser*>("browser_RadioConversation");QTRY_COMPARE(trackLinks(browser).size(),2);
    const auto screenshot=qEnvironmentVariable("TANGO_CHAT_SCREENSHOT");if(!screenshot.isEmpty())QVERIFY(window.grab().save(screenshot+".songs.png"));
    QSignalSpy resolved(&window,&MainWindow::onlinePlaybackResolved);QSignalSpy activated(dialog,&RadioDialog::trackActivated);
    const auto target=trackLinks(browser).last();const auto point=anchorPoint(browser,target);QVERIFY(point.x()>=0);QTest::mouseClick(browser->viewport(),Qt::LeftButton,Qt::NoModifier,point);
    QTRY_COMPARE(activated.size(),1);QCOMPARE(qvariant_cast<OnlineSong>(activated.first().first()).id,qint64(102));
    QTRY_COMPARE(queue->model()->rowCount(),1);QTRY_COMPARE(catalog.resolved,QList<qint64>{102});QTRY_COMPARE(resolved.size(),1);
    QTRY_COMPARE(window.p_PlayerObject->state(),QMediaPlayer::PlayingState);
    QCOMPARE(fixture.server.count("POST","/v1/tasks/music-routing-task/actions"),0);
    QCOMPARE(queue->model()->index(0,1).data(SongListDelegate::SongIdRole).toLongLong(),qint64(102));
    window.p_PlayerObject->stop();
}

void ChatWorkspaceFlowTest::taskPreviewClickPlaysOnlyOneWithoutAdoption(){
    Fixture fixture(true);OwnedCatalog catalog(fixture);const auto original=fixture.server.handler;
    fixture.server.handler=[&fixture,&catalog,original](const Request &request)->Response{
        auto response=original(request);
        if(request.path=="/v1/tasks"&&request.method=="POST"){
            auto preview=fixture.task.value("preview").toObject();preview.insert("tracks",catalog.songs);fixture.task.insert("preview",preview);response.body.insert("task",fixture.task);
        }return response;
    };
    fixture.makePlan=[](const QString &mid,const QString &text){return Fixture::plan(mid,{"scene_research"},
        {Fixture::action("music","music.task",text,{{"mode","new"}})});};
    MainWindow window;QVERIFY(open(fixture,window));QVERIFY(send(fixture,window,QStringLiteral("给我一份晚间散步歌单，先预览")));
    auto *browser=window.findChild<QTextBrowser*>("browser_RadioConversation");QTRY_COMPARE(trackLinks(browser).size(),2);
    auto *queue=window.findChild<QTableView*>("queueView");QCOMPARE(queue->model()->rowCount(),0);QVERIFY(catalog.resolved.isEmpty());
    QTest::mouseClick(browser->viewport(),Qt::LeftButton,Qt::NoModifier,anchorPoint(browser,trackLinks(browser).last()));
    QTRY_COMPARE(queue->model()->rowCount(),1);QTRY_COMPARE(catalog.resolved,QList<qint64>{102});
    QTRY_COMPARE(window.p_PlayerObject->state(),QMediaPlayer::PlayingState);
    QCOMPARE(fixture.server.count("POST","/v1/tasks/music-routing-task/actions"),0);
    QCOMPARE(fixture.task.value("status").toString(),QString("preview"));
    QCOMPARE(queue->model()->index(0,1).data(SongListDelegate::SongIdRole).toLongLong(),qint64(102));window.p_PlayerObject->stop();
}

void ChatWorkspaceFlowTest::oldRecommendationAnchorCannotPlayAfterReplacement(){
    Fixture fixture(true);OwnedCatalog catalog(fixture);MainWindow window;QVERIFY(open(fixture,window));lookup(fixture);QVERIFY(send(fixture,window,QStringLiteral("查找晚风来信")));
    auto *browser=window.findChild<QTextBrowser*>("browser_RadioConversation");QTRY_VERIFY(!trackLinks(browser).isEmpty());const auto old=trackLinks(browser).first();
    auto *dialog=window.findChild<RadioDialog*>();const auto songs=dialog->recommendations();dialog->setRecommendations(QStringLiteral("新版结果"),{},songs);
    QSignalSpy activated(dialog,&RadioDialog::trackActivated);QVERIFY(QMetaObject::invokeMethod(browser,"anchorClicked",Q_ARG(QUrl,old)));QCOMPARE(activated.size(),0);QVERIFY(catalog.resolved.isEmpty());
    QTRY_VERIFY(!trackLinks(browser).isEmpty());const auto current=trackLinks(browser).first();QVERIFY(current!=old);
    dialog->setBusy(true,QStringLiteral("正在修改"));QVERIFY(QMetaObject::invokeMethod(browser,"anchorClicked",Q_ARG(QUrl,current)));QCOMPARE(activated.size(),0);
    dialog->setBusy(false);dialog->resetForAccount();QVERIFY(QMetaObject::invokeMethod(browser,"anchorClicked",Q_ARG(QUrl,current)));QCOMPARE(activated.size(),0);
}

void ChatWorkspaceFlowTest::clearingCanBeCancelledAndPreservesPreferencesAndQueue(){
    ClearFixture fixture;OwnedCatalog catalog(fixture.backend);MainWindow window;QVERIFY(open(fixture.backend,window));lookup(fixture.backend);
    QVERIFY(send(fixture.backend,window,QStringLiteral("查找晚风来信")));auto *browser=window.findChild<QTextBrowser*>("browser_RadioConversation");QTRY_VERIFY(!trackLinks(browser).isEmpty());
    QTest::mouseClick(browser->viewport(),Qt::LeftButton,Qt::NoModifier,anchorPoint(browser,trackLinks(browser).first()));
    auto *queue=window.findChild<QTableView*>("queueView");QTRY_COMPARE(queue->model()->rowCount(),1);QVERIFY(settled(window));
    QString error;QVERIFY2(RadioStore(QSqlDatabase::database()).save(window.accountProfile(),QStringLiteral("喜欢轻快的华语R&B"),&error),qPrintable(error));
    const auto before=plain(window);QVERIFY(confirmClear(window,false));QCOMPARE(fixture.backend.server.count("DELETE","/v1/conversations"),0);QCOMPARE(plain(window),before);
    QVERIFY(confirmClear(window,true));QTRY_COMPARE(fixture.backend.server.count("DELETE","/v1/conversations"),1);QTRY_VERIFY(!plain(window).contains(QStringLiteral("查找晚风来信")));
    QCOMPARE(queue->model()->rowCount(),1);QCOMPARE(RadioStore(QSqlDatabase::database()).preference(window.accountProfile()),QStringLiteral("喜欢轻快的华语R&B"));
    QCOMPARE(catalog.resolved.size(),1);QVERIFY(window.findChild<RadioDialog*>()->recommendations().isEmpty());
    window.p_PlayerObject->stop();
}

void ChatWorkspaceFlowTest::clearingSurvivesReopenAndKeepsOtherAccount(){
    ClearFixture fixture;auto window=std::make_unique<MainWindow>();QVERIFY(open(fixture.backend,*window));QVERIFY(send(fixture.backend,*window,QStringLiteral("甲账号的旧消息")));
    const auto profile=window->accountProfile();QVERIFY(fixture.backend.login("other-account"));QVERIFY(send(fixture.backend,*window,QStringLiteral("乙账号必须保留")));
    QVERIFY(fixture.backend.login());QVERIFY(settled(*window));QVERIFY(plain(*window).contains(QStringLiteral("甲账号的旧消息")));
    QVERIFY(confirmClear(*window,true));QTRY_VERIFY(!plain(*window).contains(QStringLiteral("甲账号的旧消息")));close(window);
    window=std::make_unique<MainWindow>();QVERIFY(open(fixture.backend,*window));QVERIFY(!plain(*window).contains(QStringLiteral("甲账号的旧消息")));
    QSqlQuery query;query.prepare("SELECT COUNT(*) FROM radio_route_journal WHERE profile=?");query.addBindValue(profile);QVERIFY(query.exec());QVERIFY(query.next());QCOMPARE(query.value(0).toInt(),0);query.finish();
    QVERIFY(fixture.backend.login("other-account"));QTRY_VERIFY(plain(*window).contains(QStringLiteral("乙账号必须保留")));close(window);
}

void ChatWorkspaceFlowTest::clearFailureKeepsHistoryAndRetryUsesOriginalRequest(){
    ClearFixture fixture;MainWindow window;QVERIFY(open(fixture.backend,window));QVERIFY(send(fixture.backend,window,QStringLiteral("删除失败时要保留这句话")));
    fixture.clearFailures=1;QVERIFY(confirmClear(window,true));QTRY_COMPARE(fixture.backend.server.count("DELETE","/v1/conversations"),1);QTRY_VERIFY(!window.findChild<ConversationClient*>()->busy());
    QVERIFY(plain(window).contains(QStringLiteral("删除失败时要保留这句话")));QVERIFY(confirmClear(window,true));QTRY_COMPARE(fixture.backend.server.count("DELETE","/v1/conversations"),2);
    QStringList ids;for(const auto &request:fixture.backend.server.requests)if(request.method=="DELETE")ids.append(request.body.value("requestId").toString());
    QCOMPARE(ids.size(),2);QVERIFY(!ids.first().isEmpty());QCOMPARE(ids.first(),ids.last());QTRY_VERIFY(!plain(window).contains(QStringLiteral("删除失败时要保留这句话")));
}

void ChatWorkspaceFlowTest::clearingDropsLateRouteReply(){
    ClearFixture fixture;MainWindow window;QVERIFY(open(fixture.backend,window));fixture.backend.routeDelay=500;
    fixture.backend.makePlan=[](const QString &mid,const QString &text){return Fixture::plan(mid,{"help"},{Fixture::action("reply","assistant.reply",text,{{"kind","help"},{"text",QStringLiteral("延迟的旧回复不能回来")}})});};
    auto *dialog=window.findChild<RadioDialog*>();dialog->findChild<QPlainTextEdit*>("edit_RadioInstruction")->setPlainText(QStringLiteral("等待中的旧问题"));
    dialog->findChild<QPushButton*>("button_RadioGenerate")->click();QTRY_COMPARE(fixture.backend.server.count("POST",fixture.backend.routePath()),1);
    QVERIFY(confirmClear(window,true));QTRY_COMPARE(fixture.backend.server.count("DELETE","/v1/conversations"),1);QTest::qWait(650);
    QVERIFY(!plain(window).contains(QStringLiteral("等待中的旧问题")));QVERIFY(!plain(window).contains(QStringLiteral("延迟的旧回复不能回来")));
    QCOMPARE(fixture.backend.server.count("POST","/v1/tasks"),0);QVERIFY(window.findChild<RadioDialog*>()->recommendations().isEmpty());
}

void ChatWorkspaceFlowTest::clearConfirmationCannotDeleteSwitchedAccount(){
    ClearFixture fixture;MainWindow window;QVERIFY(open(fixture.backend,window));QVERIFY(send(fixture.backend,window,QStringLiteral("先留着甲的聊天")));
    auto *button=window.findChild<QPushButton*>("button_ConversationClearAll");QVERIFY(button&&button->isEnabled());bool handled=false;
    QTimer::singleShot(0,&window,[&]{
        QPointer<QMessageBox> box=window.findChild<QMessageBox*>("messageBox_ClearConversations");if(!box)return;
        if(fixture.backend.login("other-account")){handled=true;if(box)box->button(QMessageBox::Yes)->click();}else if(box)box->reject();
    });button->click();QVERIFY(handled);QTest::qWait(100);QCOMPARE(fixture.backend.server.count("DELETE","/v1/conversations"),0);
}

void ChatWorkspaceFlowTest::uncertainClearKeepsOriginalIdAcrossClientRestart(){
    ClearFixture fixture;QVERIFY(fixture.backend.login());auto *client=fixture.backend.client;
    QSignalSpy accepted(client,&ConversationClient::messageAccepted);client->submit(QStringLiteral("需要清除的持久消息"));QTRY_COMPARE(accepted.size(),1);QTRY_VERIFY(!client->busy());
    fixture.clearFailures=1;QSignalSpy failed(client,&ConversationClient::clearFailed);client->clearAll();QTRY_COMPARE(failed.size(),1);
    QVERIFY(client->snapshot().value("clearPending").toBool());QCOMPARE(client->snapshot().value("messages").toArray().size(),1);
    delete fixture.backend.client;fixture.backend.client=new ConversationClient(nullptr,fixture.backend.backend);client=fixture.backend.client;
    QVERIFY(client->snapshot().value("clearPending").toBool());QSignalSpy cleared(client,&ConversationClient::conversationsCleared);
    client->submit(QStringLiteral("核对前不应发送新消息"));QTest::qWait(30);QCOMPARE(fixture.backend.server.count("POST",fixture.backend.messagesPath()),1);
    client->clearAll();QTRY_COMPARE(cleared.size(),1);QVERIFY(!client->snapshot().value("clearPending").toBool());
    QStringList ids;for(const auto &request:fixture.backend.server.requests)if(request.method=="DELETE")ids.append(request.body.value("requestId").toString());
    QCOMPARE(ids.size(),2);QVERIFY(!ids.first().isEmpty());QCOMPARE(ids.first(),ids.last());
    QCOMPARE(fixture.backend.server.count("POST",fixture.backend.routePath()),0);QVERIFY(fixture.backend.current.isEmpty());
}

void ChatWorkspaceFlowTest::clearingCancelsOldQuotaObservationAndAllowsFreshReads(){
    ClearFixture fixture;const auto original=fixture.backend.server.handler;int usageCalls=0;
    fixture.backend.server.handler=[original,&usageCalls](const Request &request)->Response{
        auto response=original(request);
        if(request.path=="/v1/account/usage"){
            ++usageCalls;auto daily=response.body.value("daily").toObject(),total=response.body.value("total").toObject();
            daily.insert("used",usageCalls);daily.insert("remaining",30-usageCalls);total.insert("used",usageCalls);total.insert("remaining",300-usageCalls);
            response.body.insert("daily",daily);response.body.insert("total",total);response.delay=usageCalls==1?2000:0;
        }return response;
    };
    MainWindow window;QVERIFY(open(fixture.backend,window));QTRY_COMPARE(usageCalls,1);
    auto *quota=window.findChild<QLabel*>("label_RadioQuota");QVERIFY(quota);QVERIFY(!quota->text().contains(QStringLiteral("今日剩余 29/30")));
    QVERIFY(confirmClear(window,true));QTRY_COMPARE(fixture.backend.server.count("DELETE","/v1/conversations"),1);
    QTRY_VERIFY(usageCalls>=2&&quota->text().contains(QStringLiteral("今日剩余 %1/30").arg(30-usageCalls)));
    // 首个响应比清除晚到，也不能占住观察指针或覆盖已经核验的新额度。
    QTest::qWait(2150);QVERIFY(quota->text().contains(QStringLiteral("今日剩余 %1/30").arg(30-usageCalls)));
    const int before=usageCalls;QVERIFY(QMetaObject::invokeMethod(&window,"openRadio",Qt::DirectConnection));
    QTRY_VERIFY(usageCalls>before&&quota->text().contains(QStringLiteral("今日剩余 %1/30").arg(30-usageCalls)));
}

void ChatWorkspaceFlowTest::oldClearResponseCannotRemoveNewerClientsPendingId(){
    ClearFixture fixture;QVERIFY(fixture.backend.login());auto *first=fixture.backend.client;
    auto second=std::make_unique<ConversationClient>(nullptr,fixture.backend.backend);
    QSignalSpy accepted(first,&ConversationClient::messageAccepted);first->submit(QStringLiteral("竞争前的旧聊天"));QTRY_COMPARE(accepted.size(),1);
    const auto original=fixture.backend.server.handler;QString sharedId,newerId;int sharedCalls=0,newerCalls=0;
    fixture.backend.server.handler=[&](const Request &request)->Response{
        auto response=original(request);
        if(request.method=="DELETE"&&request.path=="/v1/conversations"){
            const auto id=request.body.value("requestId").toString();if(sharedId.isEmpty())sharedId=id;
            if(id==sharedId){++sharedCalls;response.delay=sharedCalls==1?500:1500;}
            else{
                if(newerId.isEmpty())newerId=id;++newerCalls;
                // 新清除已在固定后台提交，但首个响应返回故障，客户端必须保留原编号。
                if(newerCalls==1)return {{{"error",QJsonObject{{"message",QStringLiteral("新清除已提交但响应未知")}}}},503};
            }
        }return response;
    };
    QSignalSpy firstCleared(first,&ConversationClient::conversationsCleared),firstFailed(first,&ConversationClient::clearFailed);
    QSignalSpy secondCleared(second.get(),&ConversationClient::conversationsCleared),secondFailed(second.get(),&ConversationClient::clearFailed);
    // 两个客户端先共享同一落盘编号；第二个客户端的旧回执故意更晚到达。
    first->clearAll();QTRY_COMPARE(sharedCalls,1);QVERIFY(first->clearing());
    second->clearAll();QTRY_COMPARE(sharedCalls,2);QTRY_COMPARE(firstCleared.size(),1);QCOMPARE(secondCleared.size(),0);
    first->clearAll();QTRY_COMPARE(newerCalls,1);QTRY_COMPARE(firstFailed.size(),1);QVERIFY(!newerId.isEmpty());QVERIFY(newerId!=sharedId);
    QTRY_COMPARE(secondFailed.size(),1);QCOMPARE(secondCleared.size(),0);
    const auto identity=fixture.backend.backend->serviceIdentity()+QLatin1Char(':')+fixture.backend.backend->userId();
    const auto key=QCryptographicHash::hash(identity.toUtf8(),QCryptographicHash::Sha256).toHex();
    QFile receipt(fixture.backend.directory.filePath(QStringLiteral("conversation-clear-%1.json").arg(QString::fromLatin1(key))));
    QVERIFY(receipt.open(QIODevice::ReadOnly));QCOMPARE(QJsonDocument::fromJson(receipt.readAll()).object().value("requestId").toString(),newerId);receipt.close();
    auto reopened=std::make_unique<ConversationClient>(nullptr,fixture.backend.backend);QVERIFY(reopened->snapshot().value("clearPending").toBool());
    const int messagesBefore=fixture.backend.server.count("POST",fixture.backend.messagesPath());QSignalSpy blocked(reopened.get(),&ConversationClient::submissionFailed);
    reopened->submit(QStringLiteral("核对新清除前不能开始新聊天"));QCOMPARE(blocked.size(),1);QTest::qWait(30);
    QCOMPARE(fixture.backend.server.count("POST",fixture.backend.messagesPath()),messagesBefore);
    QSignalSpy reconciled(reopened.get(),&ConversationClient::conversationsCleared);reopened->clearAll();QTRY_COMPARE(reconciled.size(),1);QCOMPARE(newerCalls,2);
    QCOMPARE(firstCleared.size(),1);QVERIFY(!receipt.exists());QCOMPARE(fixture.backend.server.count("POST",fixture.backend.routePath()),0);
    // 另一个客户端已核对并移除了文件，仍未知的原客户端只能重放旧编号，不能清除之后的新聊天。
    QSignalSpy newAccepted(reopened.get(),&ConversationClient::messageAccepted);reopened->submit(QStringLiteral("新窗口核对后创建的聊天应当保留"));QTRY_COMPARE(newAccepted.size(),1);
    first->clearAll();QTRY_COMPARE(firstCleared.size(),2);
    QString lastId;for(const auto &request:fixture.backend.server.requests)if(request.method=="DELETE")lastId=request.body.value("requestId").toString();
    QCOMPARE(lastId,newerId);QVERIFY(!fixture.backend.current.isEmpty());
    QVERIFY(QString::fromUtf8(QJsonDocument(fixture.backend.current).toJson()).contains(QStringLiteral("新窗口核对后创建的聊天应当保留")));
}

QTEST_MAIN(ChatWorkspaceFlowTest)
#include "chat_workspace_flow_test.moc"
