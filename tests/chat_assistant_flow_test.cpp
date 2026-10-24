#include "routing_fixtures.h"
#include "mainwindow.h"
#include "radiodialog.h"
#include "radiostore.h"
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLineEdit>
#include <QTableView>
#include <QSqlDatabase>
using namespace RoutingFixtures;

class ChatAssistantFlowTest : public QObject {
    Q_OBJECT
    QJsonObject song(int id = 101, const QString &title = QStringLiteral("十年")) {
        return {{"id", id}, {"title", title}, {"artist", "陈奕迅"}, {"durationMs", 205000}, {"album", "黑白灰"}};
    }
    void catalog(FakeServer &music, Fixture &fixture, const QJsonArray &songs, int delay = 0) {
        fixture.environment.set("MUSICPLAYER_NETEASE_PORT", QByteArray::number(music.server.serverPort()));
        QDir().mkpath(fixture.directory.filePath("netease"));
        QFile token(fixture.directory.filePath("netease/token"));
        if (token.open(QIODevice::WriteOnly)) token.write("owned-offline-fixture-token");
        token.close();
        music.handler = [songs, delay](const Request &request) -> Response {
            if (request.path == "/health") return {{{"app", "quiet-netease"}, {"version", 1}, {"catalogVersion", 2}, {"profileIsolation", 1}}};
            if (request.path == "/search") return {{{"songs", songs}, {"total", songs.size()}}, 200, delay};
            if (request.path == "/song/url") return {{{"message", "离线验收不提供音乐流"}}};
            return {{{"loggedIn", false}}};
        };
    }
    bool open(Fixture &fixture, MainWindow &window) {
        window.show();
        if (!fixture.login() || !QMetaObject::invokeMethod(&window, "openRadio", Qt::DirectConnection)) return false;
        auto *conversation = window.findChild<ConversationClient *>();
        return QTest::qWaitFor([&] { return !conversation->busy(); }, 1500);
    }
    bool send(Fixture &fixture, MainWindow &window, const QString &text) {
        auto *client = window.findChild<ConversationClient *>();
        if (!QTest::qWaitFor([&] { return !client->busy() && !client->snapshot().value("journalPending").toBool(); }, 2000)) return false;
        auto *dialog = window.findChild<RadioDialog *>();
        auto *button = dialog->findChild<QPushButton *>("button_RadioGenerate");
        dialog->findChild<QPlainTextEdit *>("edit_RadioInstruction")->setPlainText(text);
        if (!button->isEnabled()) return false;
        const auto oldId = fixture.messageId;
        button->click();
        return QTest::qWaitFor([&] { return fixture.messageId != oldId && !client->busy() && !client->snapshot().value("journalPending").toBool(); }, 2000);
    }
    QString transcript(MainWindow &window) {
        return window.findChild<QPlainTextEdit *>("text_RadioConversation")->toPlainText();
    }
    void lookup(Fixture &fixture, bool play) {
        fixture.makePlan = [play](const QString &id, const QString &text) {
            return Fixture::plan(id, {"song_lookup"}, {Fixture::action("lookup", "music.lookup", text,
                {{"artist", "陈奕迅"}, {"title", "十年"}, {"play", play}})});
        };
    }
private slots:
    void cleanup() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (QSqlDatabase::contains()) {
            QSqlDatabase::database(QSqlDatabase::defaultConnection, false).close();
            QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
        }
    }
    void searchShowsSongsAndChatSelectionIsJournalled() {
        Fixture fixture(true); FakeServer music; catalog(music, fixture, {song()});
        MainWindow window; QVERIFY(open(fixture, window)); lookup(fixture, false);
        QVERIFY(!window.findChild<QLineEdit *>("lineEdit_InputSong")->isVisible());
        QVERIFY(!window.findChild<QPushButton *>("pushButton_AddSong")->isVisible());
        QVERIFY(send(fixture, window, "帮我搜索陈奕迅的十年"));
        QTRY_COMPARE(music.count("GET", "/search"), 1);
        auto *dialog = window.findChild<RadioDialog *>(); QTRY_COMPARE(dialog->recommendations().size(), 1);
        QVERIFY(transcript(window).contains("黑白灰"));
        QCOMPARE(music.count("GET", "/song/url"), 0);
        QVERIFY(send(fixture, window, "播放第1首"));
        QTRY_COMPARE(music.count("GET", "/song/url"), 1);
        QVERIFY(transcript(window).contains("播放第1首"));
        QCOMPARE(fixture.server.count("POST", "/v1/tasks"), 0);
    }
    void focusedNavigationAndComposerKeepSpaceKey() {
        Fixture fixture(true); MainWindow window; window.show(); QVERIFY(fixture.login());
        auto *navigation = window.findChild<QPushButton *>("radioButton");
        navigation->setFocus(); QTest::keyClick(navigation, Qt::Key_Space);
        QTRY_VERIFY(window.findChild<RadioDialog *>() && window.findChild<RadioDialog *>()->isVisible());
        auto *composer = window.findChild<QPlainTextEdit *>("edit_RadioInstruction");
        composer->setFocus(); composer->setPlainText("running");
        QTest::keyClick(composer, Qt::Key_End); QTest::keyClick(composer, Qt::Key_Space);
        QCOMPARE(composer->toPlainText(), QString("running "));
    }
    void exactPlaybackRequestsStreamOnce() {
        Fixture fixture(true); FakeServer music; catalog(music, fixture, {song()});
        MainWindow window; QVERIFY(open(fixture, window)); lookup(fixture, true);
        QVERIFY(send(fixture, window, "帮我播放陈奕迅的十年"));
        QTRY_COMPARE(music.count("GET", "/song/url"), 1);
        QVERIFY(transcript(window).contains("正在请求播放"));
        window.findChild<ConversationClient *>()->restoreLatest();
        QTest::qWait(80); QCOMPARE(music.count("GET", "/song/url"), 1);
    }
    void versionAmbiguityNeverAutoplays() {
        Fixture fixture(true); FakeServer music; catalog(music, fixture, {song(101), song(102)});
        MainWindow window; QVERIFY(open(fixture, window)); lookup(fixture, true);
        QVERIFY(send(fixture, window, "帮我播放陈奕迅的十年"));
        QTRY_VERIFY(transcript(window).contains("找到多个版本"));
        QCOMPARE(music.count("GET", "/song/url"), 0);
    }
    void requestedStudioVersionRequiresPositiveMetadataEvidence() {
        Fixture fixture(true); FakeServer music;
        auto live=song(102,QStringLiteral("十年 (Live)")); live.insert("album",QStringLiteral("现场巡回"));
        auto studio=song(103); studio.insert("album",QStringLiteral("十年 录音室版"));
        catalog(music,fixture,{song(101),live,studio});
        MainWindow window; QVERIFY(open(fixture,window));
        fixture.makePlan=[](const QString &id,const QString &text) {return Fixture::plan(id,{"song_lookup"},{Fixture::action("lookup","music.lookup",text,
            {{"artist","陈奕迅"},{"title","十年"},{"play",true},{"version","studio"}})});};
        QVERIFY(send(fixture,window,QStringLiteral("就在这台电脑播放陈奕迅的十年，用录音室版")));
        QTRY_COMPARE(music.count("GET","/song/url"),1);
        QCOMPARE(window.findChild<RadioDialog*>()->recommendations().size(),1);
        QCOMPARE(window.findChild<RadioDialog*>()->recommendations().first().song.id,qint64(103));
    }
    void unknownVersionIsNotAssumedStudioAndLiveSuffixIsAmbiguous() {
        Fixture fixture(true); FakeServer music; catalog(music,fixture,{song()});
        MainWindow window; QVERIFY(open(fixture,window));
        fixture.makePlan=[](const QString &id,const QString &text) {return Fixture::plan(id,{"song_lookup"},{Fixture::action("lookup","music.lookup",text,
            {{"artist","陈奕迅"},{"title","十年"},{"play",true},{"version","studio"}})});};
        QVERIFY(send(fixture,window,QStringLiteral("播放陈奕迅的十年，用录音室版")));
        QTRY_VERIFY(transcript(window).contains(QStringLiteral("不足以确认你指定的版本")));
        QCOMPARE(music.count("GET","/song/url"),0);
        catalog(music,fixture,{song(),song(102,QStringLiteral("十年 (Live)"))}); lookup(fixture,true);
        QVERIFY(send(fixture,window,QStringLiteral("播放陈奕迅的十年")));
        QTRY_VERIFY(transcript(window).contains(QStringLiteral("找到多个版本")));
        QCOMPARE(music.count("GET","/song/url"),0);
    }
    void explicitVersionConflictingWithTitleDoesNotAutoplay() {
        Fixture fixture(true); FakeServer music; auto item=song(101,QStringLiteral("十年 (Live)")); item.insert("version",QStringLiteral("studio"));
        catalog(music,fixture,{item}); MainWindow window; QVERIFY(open(fixture,window));
        fixture.makePlan=[](const QString &id,const QString &text) {return Fixture::plan(id,{"song_lookup"},{Fixture::action("lookup","music.lookup",text,
            {{"artist","陈奕迅"},{"title","十年"},{"play",true},{"version","studio"}})});};
        QVERIFY(send(fixture,window,QStringLiteral("播放陈奕迅的十年，用录音室版")));
        QTRY_VERIFY(transcript(window).contains(QStringLiteral("不足以确认你指定的版本")));
        QCOMPARE(music.count("GET","/song/url"),0);
    }
    void newMessageDiscardsDelayedLookupBeforePlayback() {
        Fixture fixture(true); FakeServer music; catalog(music,fixture,{song()},250);
        MainWindow window; QVERIFY(open(fixture,window)); lookup(fixture,true);
        QVERIFY(send(fixture,window,QStringLiteral("播放陈奕迅的十年"))); QTRY_COMPARE(music.count("GET","/search"),1);
        fixture.makePlan=[](const QString &id,const QString &text) {return Fixture::plan(id,{"help"},{Fixture::action("help","assistant.reply",text,{{"kind","help"},{"text","可以找歌"}})});};
        QVERIFY(send(fixture,window,QStringLiteral("你能做什么"))); QTest::qWait(350);
        QCOMPARE(music.count("GET","/song/url"),0);
    }
    void exactMatchAfterTenUnrelatedHitsIsStillVerified() {
        Fixture fixture(true); FakeServer music; QJsonArray candidates;
        for (int i = 0; i < 10; ++i) candidates.append(song(200 + i, QString("其他歌曲%1").arg(i)));
        candidates.append(song()); catalog(music, fixture, candidates);
        MainWindow window; QVERIFY(open(fixture, window)); lookup(fixture, true);
        QVERIFY(send(fixture, window, "帮我播放陈奕迅的十年"));
        QTRY_COMPARE(music.count("GET", "/song/url"), 1);
        QCOMPARE(window.findChild<RadioDialog *>()->recommendations().first().song.id, qint64(101));
        QVERIFY(!transcript(window).contains("正在曲库查找"));
    }
    void failedCatalogIsExplicitAndDoesNotCreateTask() {
        Fixture fixture(true); FakeServer music; catalog(music, fixture, {});
        MainWindow window; QVERIFY(open(fixture, window)); lookup(fixture, false);
        QVERIFY(send(fixture, window, "帮我搜索陈奕迅的十年"));
        QTRY_VERIFY(transcript(window).contains("没有返回匹配"));
        QCOMPARE(music.count("GET", "/song/url"), 0);
        QCOMPARE(fixture.server.count("POST", "/v1/tasks"), 0);
    }
    void accountChangeDiscardsDelayedLookup() {
        Fixture fixture(true); FakeServer music; catalog(music, fixture, {song()}, 250);
        MainWindow window; QVERIFY(open(fixture, window)); lookup(fixture, true);
        QVERIFY(send(fixture, window, "帮我播放陈奕迅的十年"));
        QTRY_COMPARE(music.count("GET", "/search"), 1); QVERIFY(fixture.login());
        QTest::qWait(350); QCOMPARE(music.count("GET", "/song/url"), 0);
        QVERIFY(window.findChild<RadioDialog *>()->recommendations().isEmpty());
    }
    void deletionClarifiesAndRequiresCurrentQueueConfirmation() {
        Fixture fixture(true); FakeServer music; catalog(music, fixture, {song()});
        MainWindow window; QVERIFY(open(fixture, window)); lookup(fixture, true);
        QVERIFY(send(fixture, window, "帮我播放陈奕迅的十年")); QTRY_COMPARE(music.count("GET", "/song/url"), 1);
        auto *queue = window.findChild<QTableView *>("queueView");
        QVERIFY(queue); QCOMPARE(queue->model()->rowCount(), 1);
        QVERIFY(send(fixture, window, "帮我删除所有歌曲"));
        QTRY_VERIFY(transcript(window).contains("你想清空待播放队列")); QCOMPARE(queue->model()->rowCount(), 1);
        QVERIFY(send(fixture, window, "清空待播放队列"));
        QTRY_VERIFY(transcript(window).contains("队列中的 1 首歌")); QCOMPARE(queue->model()->rowCount(), 1);
        QVERIFY(send(fixture, window, "确认清空待播放队列"));
        QTRY_VERIFY2(transcript(window).contains("已清空当前待播放队列"), qPrintable(transcript(window)));
        QTRY_COMPARE(queue->model()->rowCount(), 0);
        QVERIFY(transcript(window).contains("磁盘文件均保留"));
    }
    void typedMemoryConfirmationContinuesOnlyImmediateDependentRequest() {
        Fixture fixture(true); MainWindow window; QVERIFY(open(fixture, window));
        fixture.makePlan = [](const QString &id, const QString &text) {
            return Fixture::plan(id, {"preference_proposal", "scene_research"}, {
                Fixture::action("memory", "memory.propose", text, {{"operation", "set"}, {"preference", "喜欢粤语"}}),
                Fixture::action("music", "music.task", text, {{"mode", "new"}}, {"memory"})});
        };
        QVERIFY(send(fixture, window, "先记住我喜欢粤语，再推荐歌曲"));
        QTRY_VERIFY(transcript(window).contains("确认保存口味"));
        QCOMPARE(fixture.server.count("POST", "/v1/tasks"), 0);
        QVERIFY(send(fixture, window, "确认保存口味"));
        QTRY_COMPARE(fixture.server.count("POST", "/v1/tasks"), 1);
        QCOMPARE(RadioStore(QSqlDatabase::database()).preference(window.accountProfile()), QString("喜欢粤语"));
        QCOMPARE(fixture.routeReplies.size(), 1);
        QVERIFY(transcript(window).contains("确认保存口味"));
    }
    void interveningTopicInvalidatesQueueConfirmation() {
        Fixture fixture(true); FakeServer music; catalog(music, fixture, {song()});
        MainWindow window; QVERIFY(open(fixture, window)); lookup(fixture, true);
        QVERIFY(send(fixture, window, "帮我播放陈奕迅的十年")); QTRY_COMPARE(music.count("GET", "/song/url"), 1);
        QVERIFY(send(fixture, window, "清空待播放队列"));
        QTRY_VERIFY(transcript(window).contains("队列中的 1 首歌"));
        fixture.makePlan = [](const QString &id, const QString &text) {
            return Fixture::plan(id, {"help"}, {Fixture::action("help", "assistant.reply", text, {{"kind", "help"}, {"text", "帮助"}})});
        };
        QVERIFY(send(fixture, window, "你能做什么"));
        QVERIFY(send(fixture, window, "确认清空待播放队列"));
        QTRY_VERIFY(transcript(window).contains("没有待确认的队列清空操作"));
        QCOMPARE(window.findChild<QTableView *>("queueView")->model()->rowCount(), 1);
    }
    void confirmedPreferenceReadAndOrphanCommandsNeverRoute() {
        Fixture fixture(true); FakeServer music; catalog(music, fixture, {});
        MainWindow window; QVERIFY(open(fixture, window));
        RadioStore store(QSqlDatabase::database());
        QVERIFY(store.save(window.accountProfile(), QStringLiteral("偏好粤语和爵士")));
        QVERIFY(store.save(QStringLiteral("another-profile"), QStringLiteral("其他账号私密口味")));
        QVERIFY(send(fixture, window, "查看我的口味"));
        QTRY_VERIFY(transcript(window).contains("偏好粤语和爵士"));
        QVERIFY(!transcript(window).contains("其他账号私密口味"));
        QVERIFY(send(fixture, window, "确认保存口味"));
        QTRY_VERIFY(transcript(window).contains("当前没有可确认或取消的口味提议"));
        QVERIFY(send(fixture, window, "播放第1首"));
        QTRY_VERIFY(transcript(window).contains("当前还没有可以采用或播放的歌曲"));
        QVERIFY(send(fixture, window, "保留前三首"));
        QTRY_VERIFY(transcript(window).contains("当前没有可锁定或解除锁定的任务预览"));
        QVERIFY(fixture.routeReplies.isEmpty());
        QCOMPARE(music.count("GET", "/search"), 0);
        QCOMPARE(store.preference(window.accountProfile()), QString("偏好粤语和爵士"));
    }
    void preferenceReadAlsoInvalidatesQueueConfirmation() {
        Fixture fixture(true); FakeServer music; catalog(music, fixture, {song()});
        MainWindow window; QVERIFY(open(fixture, window)); lookup(fixture, true);
        QVERIFY(send(fixture, window, "帮我播放陈奕迅的十年"));
        QTRY_COMPARE(music.count("GET", "/song/url"), 1);
        QVERIFY(send(fixture, window, "清空待播放队列"));
        QTRY_VERIFY(transcript(window).contains("队列中的 1 首歌"));
        QVERIFY(send(fixture, window, "查看我的口味"));
        QVERIFY(send(fixture, window, "确认清空待播放队列"));
        QTRY_VERIFY(transcript(window).contains("没有待确认的队列清空操作"));
        QCOMPARE(window.findChild<QTableView *>("queueView")->model()->rowCount(), 1);
    }
};
QTEST_MAIN(ChatAssistantFlowTest)
#include "chat_assistant_flow_test.moc"
