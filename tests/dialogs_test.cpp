#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>

#include "aboutdialog.h"
#include "historysong.h"
#include "likesong.h"
#include "ranklist.h"
#include "signin.h"
#include "signup.h"
#include "redisconnection.h"

namespace {

class ScopedEnvironment
{
public:
    ~ScopedEnvironment()
    {
        for (auto saved = previous.crbegin(); saved != previous.crend(); ++saved) {
            if (saved->existed) qputenv(saved->name.constData(), saved->value);
            else qunsetenv(saved->name.constData());
        }
    }

    void set(const QByteArray &name, const QByteArray &value)
    {
        previous.append({name, qgetenv(name.constData()), qEnvironmentVariableIsSet(name.constData())});
        qputenv(name.constData(), value);
    }

private:
    struct SavedValue { QByteArray name; QByteArray value; bool existed; };
    QVector<SavedValue> previous;
};

class OwnedRedisServer
{
public:
    ~OwnedRedisServer()
    {
        if (process.state() == QProcess::NotRunning) return;
        // Stop only the child created by this test, never a normal account server.
        process.terminate();
        if (!process.waitForFinished(3000)) {
            process.kill();
            process.waitForFinished(3000);
        }
    }
    QProcess process;
};

QString songName(int index)
{
    return QStringLiteral("测试歌曲 %1").arg(index);
}

qint64 rowCount(const QString &table)
{
    // Callers supply fixed test table names, never user input.
    QSqlQuery query(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table));
    return query.next() ? query.value(0).toLongLong() : -1;
}

QString redisExecutable()
{
    const QString configured = qEnvironmentVariable("MUSICPLAYER_TEST_REDIS_SERVER");
    if (!configured.isEmpty()) return configured;
    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("redis-server"));
    if (!fromPath.isEmpty()) return fromPath;
    for (const QString &path : {QStringLiteral("/opt/homebrew/opt/redis/bin/redis-server"),
                               QStringLiteral("/usr/local/opt/redis/bin/redis-server")})
        if (QFileInfo(path).isExecutable()) return path;
    return {};
}

} // namespace

class DialogsTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void historyPaginationAndClear();
    void historyDoubleClickUsesStoredSourceForDuplicateTitles();
    void favoritesPaginationAndLastPageRemoval();
    void favoriteSourceUsesUrlFor64BitSongIds();
    void accountScanLoadsAtMost500WithoutChangingData();
    void localRegistrationAndLoginRequireStoredCredentials();
    void nativeDialogFramesAndLazyRankNetwork();
    void cleanupTestCase();

private:
    QTemporaryDir isolatedData;
    ScopedEnvironment environment;
    QSqlDatabase database;
};

void DialogsTest::initTestCase()
{
    QVERIFY(isolatedData.isValid());
    environment.set("MUSICPLAYER_DATA_DIR", isolatedData.path().toUtf8());
    environment.set("MUSICPLAYER_BACKEND_URL", {});
    QVERIFY2(!QSqlDatabase::contains(), "Refusing to replace an existing default database");
    database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
    database.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY2(database.open(), qPrintable(database.lastError().text()));
    QCOMPARE(database.databaseName(), QStringLiteral(":memory:"));
    QSqlQuery query(database);
    QVERIFY(query.exec("CREATE TABLE play_history (id INTEGER PRIMARY KEY, song_name TEXT, play_time TEXT, song_id INTEGER, artist_name TEXT, media_url TEXT)"));
    QVERIFY(query.exec("CREATE TABLE favorite_songs (id INTEGER PRIMARY KEY, song_id INTEGER, song_name TEXT, artist_name TEXT, media_url TEXT UNIQUE)"));
}

void DialogsTest::init()
{
    QSqlQuery query(database);
    QVERIFY(query.exec("DELETE FROM play_history"));
    QVERIFY(query.exec("DELETE FROM favorite_songs"));
    QVERIFY(database.transaction());
    for (int index = 0; index < 600; ++index) {
        query.prepare("INSERT INTO play_history (id, song_name, play_time, song_id, artist_name, media_url) "
                      "VALUES (?, ?, '2000-01-01 00:00:00', ?, '测试歌手', ?)");
        query.addBindValue(index);
        query.addBindValue(songName(index));
        query.addBindValue(-index - 1);
        query.addBindValue(QStringLiteral("file:///isolated-dialog-test/%1.wav").arg(index));
        QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
        query.prepare("INSERT INTO favorite_songs VALUES (?, ?, ?, '测试歌手', ?)");
        query.addBindValue(index);
        query.addBindValue(-index - 1);
        query.addBindValue(songName(index));
        query.addBindValue(QStringLiteral("file:///isolated-dialog-test/%1.wav").arg(index));
        QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
    }
    QVERIFY(database.commit());
}

void DialogsTest::historyPaginationAndClear()
{
    HistorySong dialog;
    QSignalSpy cleared(&dialog, &HistorySong::historyCleared);
    auto *table = dialog.findChild<QTableWidget *>("table_HistorySong");
    auto *previous = dialog.findChild<QPushButton *>("pushButton_PreviousPage");
    auto *next = dialog.findChild<QPushButton *>("pushButton_NextPage");
    auto *page = dialog.findChild<QLabel *>("label_Page");
    QVERIFY(table && previous && next && page);
    QCOMPARE(table->editTriggers(), QAbstractItemView::NoEditTriggers);
    QCOMPARE(table->rowCount(), 500);
    QCOMPARE(table->item(0, 0)->text(), songName(599));
    QCOMPARE(table->item(499, 0)->text(), songName(100));
    QVERIFY(!previous->isEnabled());
    QVERIFY(next->isEnabled());

    next->click();
    QCOMPARE(table->rowCount(), 100);
    QCOMPARE(table->item(0, 0)->text(), songName(99));
    QCOMPARE(table->item(99, 0)->text(), songName(0));
    QVERIFY(previous->isEnabled());
    QVERIFY(!next->isEnabled());
    QVERIFY(page->text().contains(QStringLiteral("2 / 2")));
    QCOMPARE(rowCount("play_history"), qint64(600));
    previous->click();
    QCOMPARE(table->rowCount(), 500);
    next->click();

    // Confirm only the message owned by this dialog; no application-wide keys.
    QTimer messageCloser;
    connect(&messageCloser, &QTimer::timeout, &dialog, [&dialog] {
        for (auto *message : dialog.findChildren<QMessageBox *>())
            if (message->isVisible() && message->button(QMessageBox::Yes))
                message->button(QMessageBox::Yes)->click();
    });
    messageCloser.start(10);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "on_pushButton_DeleteHistory_clicked", Qt::DirectConnection));
    QCOMPARE(rowCount("play_history"), qint64(0));
    QCOMPARE(cleared.count(), 1);
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(!previous->isEnabled());
    QVERIFY(!next->isEnabled());
    QVERIFY(page->text().contains(QStringLiteral("1 / 1")));
}

void DialogsTest::historyDoubleClickUsesStoredSourceForDuplicateTitles()
{
    QSqlQuery query(database);
    QVERIFY(query.exec("DELETE FROM play_history"));
    const QString online = QStringLiteral("https://music.163.com/song?id=3000000123");
    const QString local = QStringLiteral("file:///isolated-dialog-test/同名歌曲.wav");
    for (const auto &entry : {qMakePair(qint64(3000000123LL), online),
                              qMakePair(qint64(-42), local)}) {
        query.prepare("INSERT INTO play_history (song_id,song_name,artist_name,media_url,play_time) "
                      "VALUES (?,'同名歌曲','测试歌手',?,'2000-01-01 00:00:00')");
        query.addBindValue(entry.first);
        query.addBindValue(entry.second);
        QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
    }
    HistorySong dialog;
    auto *table = dialog.findChild<QTableWidget *>("table_HistorySong");
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, 0)->text(), table->item(1, 0)->text());
    QSignalSpy selected(&dialog, &HistorySong::songSelected);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onSongDoubleClicked", Qt::DirectConnection,
                                     Q_ARG(int, 0), Q_ARG(int, 2)));
    QCOMPARE(selected.count(), 1);
    QCOMPARE(selected.takeFirst().first().toString(), local);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onSongDoubleClicked", Qt::DirectConnection,
                                     Q_ARG(int, 1), Q_ARG(int, 0)));
    QCOMPARE(selected.count(), 1);
    QCOMPARE(selected.takeFirst().first().toString(), online);
}

void DialogsTest::favoritesPaginationAndLastPageRemoval()
{
    LikeSong dialog;
    auto *table = dialog.findChild<QTableWidget *>("tableWidget_LikeSong");
    auto *previous = dialog.findChild<QPushButton *>("pushButton_PreviousPage");
    auto *next = dialog.findChild<QPushButton *>("pushButton_NextPage");
    auto *page = dialog.findChild<QLabel *>("label_Page");
    QVERIFY(table && previous && next && page);
    QCOMPARE(table->rowCount(), 500);
    QCOMPARE(table->item(0, 1)->text(), songName(599));
    QCOMPARE(table->item(499, 1)->text(), songName(100));
    QVERIFY(!previous->isEnabled());
    QVERIFY(next->isEnabled());
    next->click();
    QCOMPARE(table->rowCount(), 100);
    QCOMPARE(table->item(0, 1)->text(), songName(99));
    QCOMPARE(table->item(99, 1)->text(), songName(0));
    QVERIFY(previous->isEnabled());
    QVERIFY(!next->isEnabled());
    QCOMPARE(rowCount("favorite_songs"), qint64(600));
    previous->click();

    // A deletion on the first page must fill its vacant row from the next page.
    table->setCurrentCell(0, 0);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onRemoveSongTriggered", Qt::DirectConnection));
    QCOMPARE(rowCount("favorite_songs"), qint64(599));
    QCOMPARE(table->rowCount(), 500);
    QCOMPARE(table->item(0, 1)->text(), songName(598));
    QCOMPARE(table->item(499, 1)->text(), songName(99));
    next->click();
    QCOMPARE(table->rowCount(), 99);
    QCOMPARE(table->item(0, 1)->text(), songName(98));
    for (int removed = 0; removed < 99; ++removed) {
        table->setCurrentCell(0, 0);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onRemoveSongTriggered", Qt::DirectConnection));
    }
    // Removing the final row of the last page returns to the previous valid page.
    QCOMPARE(rowCount("favorite_songs"), qint64(500));
    QCOMPARE(table->rowCount(), 500);
    QCOMPARE(table->item(0, 1)->text(), songName(598));
    QVERIFY(!previous->isEnabled());
    QVERIFY(!next->isEnabled());
    QVERIFY(page->text().contains(QStringLiteral("1 / 1")));
}

void DialogsTest::favoriteSourceUsesUrlFor64BitSongIds()
{
    QSqlQuery query(database);
    QVERIFY(query.exec("DELETE FROM favorite_songs"));
    const QString onlineUrl = QStringLiteral("https://music.163.com/song?id=3000000000");
    const QString localUrl = QStringLiteral("file:///isolated-dialog-test/positive-id.wav");
    query.prepare("INSERT INTO favorite_songs (song_id, song_name, artist_name, media_url) VALUES (?, ?, ?, ?)");
    query.addBindValue(qint64(3000000000LL));
    query.addBindValue(QStringLiteral("64 位在线歌曲"));
    query.addBindValue(QStringLiteral("测试歌手"));
    query.addBindValue(onlineUrl);
    QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
    query.prepare("INSERT INTO favorite_songs (song_id, song_name, artist_name, media_url) VALUES (?, ?, ?, ?)");
    query.addBindValue(123);
    query.addBindValue(QStringLiteral("本地歌曲"));
    query.addBindValue(QStringLiteral("测试歌手"));
    query.addBindValue(localUrl);
    QVERIFY2(query.exec(), qPrintable(query.lastError().text()));

    LikeSong dialog;
    auto *table = dialog.findChild<QTableWidget *>("tableWidget_LikeSong");
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, 0)->data(Qt::UserRole).toString(), localUrl);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("本地"));
    QCOMPARE(table->item(1, 0)->data(Qt::UserRole).toString(), onlineUrl);
    QCOMPARE(table->item(1, 0)->text(), QStringLiteral("在线"));
}

void DialogsTest::accountScanLoadsAtMost500WithoutChangingData()
{
    const QString executable = redisExecutable();
    if (executable.isEmpty()) QSKIP("Install redis-server or set MUSICPLAYER_TEST_REDIS_SERVER");
    QTcpServer reservation;
    QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const quint16 port = reservation.serverPort();
    reservation.close();
    OwnedRedisServer redis;
    redis.process.setProcessChannelMode(QProcess::MergedChannels);
    redis.process.start(executable, {"--bind", "127.0.0.1", "--protected-mode", "yes",
        "--port", QString::number(port), "--save", "", "--appendonly", "no",
        "--dir", isolatedData.path()});
    QVERIFY2(redis.process.waitForStarted(3000), qPrintable(redis.process.errorString()));
    ScopedEnvironment redisEnvironment;
    redisEnvironment.set("MUSICPLAYER_REDIS_HOST", "127.0.0.1");
    redisEnvironment.set("MUSICPLAYER_REDIS_PORT", QByteArray::number(port));
    redisEnvironment.set("MUSICPLAYER_REDIS_PASSWORD", "");
    RedisContextPtr context(nullptr, &redisFree);
    QString error;
    const auto connectOwnedServer = [&] {
        if (redis.process.state() != QProcess::Running) return false;
        context.reset(connectLocalRedis(&error));
        if (!context) return false;
        RedisReplyPtr info(static_cast<redisReply *>(redisCommand(context.get(), "INFO server")), &freeReplyObject);
        const QByteArray expectedPid = QByteArray("process_id:") + QByteArray::number(redis.process.processId()) + "\r\n";
        const bool owned = info && info->type == REDIS_REPLY_STRING && info->str
            && QByteArray(info->str, static_cast<int>(info->len)).contains(expectedPid);
        if (!owned) context.reset();
        return owned;
    };
    // Verify the responding PID before issuing any mutation on the reserved port.
    QTRY_VERIFY_WITH_TIMEOUT(connectOwnedServer(), 5000);
    RedisReplyPtr before(static_cast<redisReply *>(redisCommand(context.get(), "DBSIZE")), &freeReplyObject);
    QVERIFY(before && before->type == REDIS_REPLY_INTEGER);
    QCOMPARE(before->integer, 0LL);
    for (int index = 0; index < 600; ++index) {
        const QByteArray name = QByteArray("dialog-test-user-") + QByteArray::number(index);
        RedisReplyPtr reply(static_cast<redisReply *>(redisCommand(context.get(),
            "HSET user:%s username %s gender male", name.constData(), name.constData())), &freeReplyObject);
        QVERIFY(reply && reply->type == REDIS_REPLY_INTEGER);
    }
    // An unrelated key must never appear in the account table.
    RedisReplyPtr unrelated(static_cast<redisReply *>(redisCommand(context.get(), "SET unrelated test")), &freeReplyObject);
    QVERIFY(unrelated && unrelated->type == REDIS_REPLY_STATUS);
    AboutDialog dialog;
    auto *table = dialog.findChild<QTableWidget *>("tableWidget_usersList");
    auto *hint = dialog.findChild<QLabel *>("label_UserHint");
    QVERIFY(table && hint);
    QCOMPARE(table->contextMenuPolicy(), Qt::NoContextMenu);
    QCOMPARE(table->editTriggers(), QAbstractItemView::NoEditTriggers);
    QVERIFY(hint->text().contains(QStringLiteral("只读")));
    QCOMPARE(table->rowCount(), 500);
    QVERIFY(hint->text().contains(QStringLiteral("500")));
    QSet<QString> usernames;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QString username = table->item(row, 0)->text();
        QVERIFY(username.startsWith(QStringLiteral("dialog-test-user-")));
        QVERIFY(!usernames.contains(username));
        usernames.insert(username);
    }
    RedisReplyPtr after(static_cast<redisReply *>(redisCommand(context.get(), "DBSIZE")), &freeReplyObject);
    QVERIFY(after && after->type == REDIS_REPLY_INTEGER);
    QCOMPARE(after->integer, 601LL);
    RedisReplyPtr commands(static_cast<redisReply *>(redisCommand(context.get(), "INFO commandstats")), &freeReplyObject);
    QVERIFY(commands && commands->type == REDIS_REPLY_STRING && commands->str);
    const QByteArray statistics(commands->str, static_cast<int>(commands->len));
    QVERIFY(statistics.contains("cmdstat_scan:"));
    QVERIFY(!statistics.contains("cmdstat_keys:"));
}

void DialogsTest::localRegistrationAndLoginRequireStoredCredentials()
{
    const QString executable = redisExecutable();
    if (executable.isEmpty()) QSKIP("Install redis-server or set MUSICPLAYER_TEST_REDIS_SERVER");
    QTcpServer reservation;
    QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const quint16 port = reservation.serverPort();
    reservation.close();
    OwnedRedisServer redis;
    redis.process.start(executable, {"--bind", "127.0.0.1", "--protected-mode", "yes",
        "--port", QString::number(port), "--save", "", "--appendonly", "no",
        "--dir", isolatedData.path()});
    QVERIFY2(redis.process.waitForStarted(3000), qPrintable(redis.process.errorString()));
    ScopedEnvironment redisEnvironment;
    redisEnvironment.set("MUSICPLAYER_REDIS_HOST", "127.0.0.1");
    redisEnvironment.set("MUSICPLAYER_REDIS_PORT", QByteArray::number(port));
    redisEnvironment.set("MUSICPLAYER_REDIS_PASSWORD", "");
    RedisContextPtr context(nullptr, &redisFree);
    const auto connectOwnedServer = [&] {
        if (redis.process.state() != QProcess::Running) return false;
        context.reset(connectLocalRedis());
        if (!context) return false;
        RedisReplyPtr info(static_cast<redisReply *>(redisCommand(context.get(), "INFO server")), &freeReplyObject);
        const QByteArray expectedPid = QByteArray("process_id:") + QByteArray::number(redis.process.processId()) + "\r\n";
        const bool owned = info && info->type == REDIS_REPLY_STRING && info->str
            && QByteArray(info->str, static_cast<int>(info->len)).contains(expectedPid);
        if (!owned) context.reset();
        return owned;
    };
    // The test may write credentials only after verifying this exact child PID.
    QTRY_VERIFY_WITH_TIMEOUT(connectOwnedServer(), 5000);
    RedisReplyPtr before(static_cast<redisReply *>(redisCommand(context.get(), "DBSIZE")), &freeReplyObject);
    QVERIFY(before && before->type == REDIS_REPLY_INTEGER);
    QCOMPARE(before->integer, 0LL);

    QDialog *activeDialog = nullptr;
    QStringList shownMessages;
    QTimer messageCloser;
    connect(&messageCloser, &QTimer::timeout, this, [&] {
        if (!activeDialog) return;
        for (auto *message : activeDialog->findChildren<QMessageBox *>()) {
            if (!message->isVisible()) continue;
            shownMessages.append(message->text());
            message->accept();
        }
        // A regression of the removed demo branch must fail, not hang in its modal list.
        for (auto *accounts : activeDialog->findChildren<AboutDialog *>())
            if (accounts->isVisible()) accounts->reject();
    });
    messageCloser.start(10);

    SignIn unknownAdmin;
    activeDialog = &unknownAdmin;
    unknownAdmin.findChild<QLineEdit *>("lineEdit_Id")->setText(QStringLiteral("admin"));
    unknownAdmin.findChild<QLineEdit *>("lineEdit_Password")->setText(QStringLiteral("admin"));
    QSignalSpy unknownLogin(&unknownAdmin, &SignIn::userLoggedIn);
    QVERIFY(QMetaObject::invokeMethod(&unknownAdmin, "on_pushButton_SignIn_clicked", Qt::DirectConnection));
    QCOMPARE(unknownAdmin.result(), int(QDialog::Rejected));
    QVERIFY(!unknownAdmin.isSignIn);
    QCOMPARE(unknownLogin.count(), 0);
    QVERIFY(shownMessages.last().contains(QStringLiteral("用户名不存在")));

    SignUp signup;
    activeDialog = &signup;
    const QString password = QStringLiteral("normal-local-test-password");
    signup.findChild<QLineEdit *>("lineEdit_Id")->setText(QStringLiteral("admin"));
    signup.findChild<QLineEdit *>("lineEdit_Password")->setText(password);
    signup.findChild<QLineEdit *>("lineEdit_ConfirmPassword")->setText(password);
    signup.findChild<QRadioButton *>("radioButton_man")->setChecked(true);
    QVERIFY(QMetaObject::invokeMethod(&signup, "on_pushButton_SignUp_clicked", Qt::DirectConnection));
    QCOMPARE(signup.result(), int(QDialog::Accepted));
    RedisReplyPtr stored(static_cast<redisReply *>(redisCommand(context.get(), "HGET user:admin password")), &freeReplyObject);
    QVERIFY(stored && stored->type == REDIS_REPLY_STRING && stored->str);
    QCOMPARE(QByteArray(stored->str, static_cast<int>(stored->len)),
             QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex());

    SignIn wrongPassword;
    activeDialog = &wrongPassword;
    wrongPassword.findChild<QLineEdit *>("lineEdit_Id")->setText(QStringLiteral("admin"));
    wrongPassword.findChild<QLineEdit *>("lineEdit_Password")->setText(QStringLiteral("admin"));
    QSignalSpy wrongLogin(&wrongPassword, &SignIn::userLoggedIn);
    QVERIFY(QMetaObject::invokeMethod(&wrongPassword, "on_pushButton_SignIn_clicked", Qt::DirectConnection));
    QCOMPARE(wrongPassword.result(), int(QDialog::Rejected));
    QVERIFY(!wrongPassword.isSignIn);
    QCOMPARE(wrongLogin.count(), 0);
    QVERIFY(shownMessages.last().contains(QStringLiteral("密码错误")));

    SignIn normalLogin;
    activeDialog = &normalLogin;
    normalLogin.findChild<QLineEdit *>("lineEdit_Id")->setText(QStringLiteral("admin"));
    normalLogin.findChild<QLineEdit *>("lineEdit_Password")->setText(password);
    QSignalSpy loggedIn(&normalLogin, &SignIn::userLoggedIn);
    QVERIFY(QMetaObject::invokeMethod(&normalLogin, "on_pushButton_SignIn_clicked", Qt::DirectConnection));
    QCOMPARE(normalLogin.result(), int(QDialog::Accepted));
    QVERIFY(normalLogin.isSignIn);
    QCOMPARE(loggedIn.count(), 1);
    QCOMPARE(loggedIn.at(0).at(0).toString(), QStringLiteral("admin"));
    activeDialog = nullptr;
}

void DialogsTest::nativeDialogFramesAndLazyRankNetwork()
{
    SignIn signin;
    SignUp signup;
    RankList rank;
    for (const QDialog *dialog : {static_cast<QDialog *>(&signin),
                                 static_cast<QDialog *>(&signup), static_cast<QDialog *>(&rank)})
        QVERIFY(!dialog->windowFlags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(rank.findChildren<QNetworkAccessManager *>().isEmpty());
}

void DialogsTest::cleanupTestCase()
{
    if (!database.isValid()) return;
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
}

QTEST_MAIN(DialogsTest)
#include "dialogs_test.moc"
