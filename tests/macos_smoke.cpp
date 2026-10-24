#include <QtTest>
#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHostAddress>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableView>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QtMath>

#include "mainwindow.h"
#include "lyricswidget.h"
#include "likesong.h"
#include "historysong.h"
#include "signin.h"
#include "signup.h"

namespace {

class ScopedEnvironment
{
public:
    ~ScopedEnvironment()
    {
        for (auto saved = previous.crbegin(); saved != previous.crend(); ++saved) {
            if (saved->existed)
                qputenv(saved->name.constData(), saved->value);
            else
                qunsetenv(saved->name.constData());
        }
    }

    void set(const QByteArray &name, const QByteArray &value)
    {
        previous.append({name, qgetenv(name.constData()),
                         qEnvironmentVariableIsSet(name.constData())});
        qputenv(name.constData(), value);
    }

private:
    struct SavedValue {
        QByteArray name;
        QByteArray value;
        bool existed;
    };
    QVector<SavedValue> previous;
};

class OwnedRedisServer
{
public:
    ~OwnedRedisServer() { stop(); }

    void stop()
    {
        if (process.state() == QProcess::NotRunning)
            return;
        // Signal only the child process created by this test, never a PID read
        // from a file or an existing account service on another port.
        process.terminate();
        if (!process.waitForFinished(3000)) {
            process.kill();
            process.waitForFinished(3000);
        }
    }

    QProcess process;
};

class ScopedApplicationAttribute
{
public:
    ScopedApplicationAttribute(Qt::ApplicationAttribute attribute, bool enabled)
        : attribute(attribute), previous(QCoreApplication::testAttribute(attribute))
    {
        QCoreApplication::setAttribute(attribute, enabled);
    }

    ~ScopedApplicationAttribute()
    {
        QCoreApplication::setAttribute(attribute, previous);
    }

private:
    Qt::ApplicationAttribute attribute;
    bool previous;
};

// Two real, quiet PCM files exercise the native macOS media backend. Keeping
// their basenames identical also catches local tracks being keyed by title/0.
bool writeWave(const QString &path, double frequency)
{
    constexpr quint32 sampleRate = 22050;
    constexpr quint32 sampleCount = sampleRate * 12;
    constexpr quint32 dataSize = sampleCount * sizeof(qint16);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + dataSize);
    stream.writeRawData("WAVE", 4);
    stream.writeRawData("fmt ", 4);
    stream << quint32(16) << quint16(1) << quint16(1) << sampleRate
           << quint32(sampleRate * sizeof(qint16)) << quint16(sizeof(qint16))
           << quint16(16);
    stream.writeRawData("data", 4);
    stream << dataSize;
    for (quint32 frame = 0; frame < sampleCount; ++frame) {
        const double phase = 2.0 * M_PI * frequency * frame / sampleRate;
        stream << qint16(500.0 * qSin(phase));
    }
    return stream.status() == QDataStream::Ok;
}

int rowsForUrl(const QSqlDatabase &database, const QString &table, const QUrl &url,
               QString *error)
{
    // Table names are fixed test literals; values always use bound parameters.
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE media_url = ?")
                      .arg(table));
    query.addBindValue(url.toString(QUrl::FullyEncoded));
    if (!query.exec() || !query.next()) {
        *error = query.lastError().text();
        return -1;
    }
    error->clear();
    return query.value(0).toInt();
}

} // namespace

class MacOSSmoke : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void localPlaybackControlsAndPersistence();
    void localLyricsAndFloatingControls();
    void queueDoubleClickDoesNotDuplicateTracks();
    void repeatedPagesReuseTheirChildren();
    void localAccountsRoundTrip();
    void manuallyImportedLyricsSurviveWindowRecreation();
    void cleanupTestCase();

private:
    QTemporaryDir isolatedData;
    QByteArray previousDataDir;
    bool hadDataDir = false;
    QStringList files;
};

void MacOSSmoke::initTestCase()
{
    QVERIFY2(isolatedData.isValid(), "Cannot create isolated test directory");
    hadDataDir = qEnvironmentVariableIsSet("MUSICPLAYER_DATA_DIR");
    previousDataDir = qgetenv("MUSICPLAYER_DATA_DIR");
    qputenv("MUSICPLAYER_DATA_DIR", isolatedData.path().toUtf8());

    for (int index = 0; index < 2; ++index) {
        const QString folder = isolatedData.path()
            + QStringLiteral("/local music %1").arg(index + 1);
        QVERIFY(QDir().mkpath(folder));
        const QString path = folder + QStringLiteral("/same-title.wav");
        QVERIFY2(writeWave(path, index ? 330.0 : 220.0), qPrintable(path));
        files.append(path);
    }

    // Only the first audio file gets a sidecar; the identical basename in the
    // second directory must not accidentally inherit the first song's lyrics.
    QFile lyrics(QFileInfo(files.first()).dir().filePath("same-title.lrc"));
    QVERIFY(lyrics.open(QIODevice::WriteOnly));
    const QByteArray lrc = QStringLiteral(
        "[ti:本地同步测试]\n[00:01.000]第一句歌词\n"
        "[00:04.000]第二句歌词\n[00:07.000]第三句歌词\n").toUtf8();
    QCOMPARE(lyrics.write(lrc), qint64(lrc.size()));
}

void MacOSSmoke::localPlaybackControlsAndPersistence()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    auto *player = window.p_PlayerObject;
    auto *playlist = window.p_PlayerList;
    auto *play = window.findChild<QPushButton *>("pushButton_PlaySong");
    auto *next = window.findChild<QPushButton *>("pushButton_PlayNextSong");
    auto *previous = window.findChild<QPushButton *>("pushButton_PreviousSong");
    auto *like = window.findChild<QPushButton *>("pushButton_AddLike");
    auto *volume = window.findChild<QSlider *>("horizontalSlider_Volume");
    auto *progress = window.findChild<QSlider *>("horizontalSlider_PlayProgress");
    QVERIFY(player);
    QVERIFY(playlist);
    QVERIFY(play);
    QVERIFY(next);
    QVERIFY(previous);
    QVERIFY(like);
    QVERIFY(volume);
    QVERIFY(progress);

    const QSqlDatabase database = QSqlDatabase::database(
        QSqlDatabase::defaultConnection, false);
    QVERIFY2(database.isOpen(), qPrintable(database.lastError().text()));
    // Fail before running any SQL if the application ignores our isolation.
    const QString databaseRelativePath = QDir(isolatedData.path())
        .relativeFilePath(QFileInfo(database.databaseName()).absoluteFilePath());
    QVERIFY2(!QDir::isAbsolutePath(databaseRelativePath)
                 && databaseRelativePath != QStringLiteral("..")
                 && !databaseRelativePath.startsWith(QStringLiteral("../")),
             qPrintable(QStringLiteral("Unsafe database path: ")
                        + database.databaseName()));

    // Only automatically dismiss QMessageBoxes belonging to this test window;
    // never send an application-wide Enter/Escape to another open application.
    int acknowledgedMessages = 0;
    QTimer messageCloser;
    connect(&messageCloser, &QTimer::timeout, &window, [&] {
        for (auto *message : window.findChildren<QMessageBox *>()) {
            if (message->isVisible()) {
                ++acknowledgedMessages;
                message->accept();
            }
        }
    });
    messageCloser.start(25);

    const QUrl first = QUrl::fromLocalFile(files.at(0));
    const QUrl second = QUrl::fromLocalFile(files.at(1));
    window.addLocalFiles(files);
    QCOMPARE(playlist->mediaCount(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(playlist->currentIndex(), 0, 5000);
    // Import begins playback; the same primary button toggles pause/resume.
    QTRY_COMPARE_WITH_TIMEOUT(player->state(), QMediaPlayer::PlayingState, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(player->duration() >= 11000, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(player->position() > 250, 10000);
    QCOMPARE(player->currentMedia().request().url(), first);
    QCOMPARE(player->error(), QMediaPlayer::NoError);

    play->click();
    QTRY_COMPARE_WITH_TIMEOUT(player->state(), QMediaPlayer::PausedState, 3000);
    const qint64 pausedPosition = player->position();
    QTest::qWait(350);
    QVERIFY2(qAbs(player->position() - pausedPosition) < 150,
             "Playback position continued advancing after pause");

    volume->setValue(17);
    QTRY_COMPARE_WITH_TIMEOUT(player->volume(), 17, 1000);
    progress->setSliderDown(true);
    progress->setValue(3500);
    progress->setSliderDown(false);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(player->position() - 3500) < 300, 5000);

    play->click();
    QTRY_COMPARE_WITH_TIMEOUT(player->state(), QMediaPlayer::PlayingState, 3000);
    const qint64 resumedPosition = player->position();
    QTRY_VERIFY_WITH_TIMEOUT(player->position() > resumedPosition + 200, 5000);

    QString sqlError;
    QTRY_VERIFY_WITH_TIMEOUT(rowsForUrl(database, "play_history", first,
                                      &sqlError) >= 1, 3000);
    QVERIFY2(sqlError.isEmpty(), qPrintable(sqlError));

    like->click();
    QCOMPARE(rowsForUrl(database, "favorite_songs", first, &sqlError), 1);
    QVERIFY2(sqlError.isEmpty(), qPrintable(sqlError));
    // Repeated favorites must remain one row even when song_id is shared/zero.
    like->click();
    QCOMPARE(rowsForUrl(database, "favorite_songs", first, &sqlError), 1);

    next->click();
    QTRY_COMPARE_WITH_TIMEOUT(playlist->currentIndex(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player->currentMedia().request().url(), second, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player->state(), QMediaPlayer::PlayingState, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(player->duration() >= 11000, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(player->position() > 100, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(rowsForUrl(database, "play_history", second,
                                      &sqlError) >= 1, 3000);
    like->click();
    QCOMPARE(rowsForUrl(database, "favorite_songs", second, &sqlError), 1);
    QCOMPARE(rowsForUrl(database, "favorite_songs", first, &sqlError), 1);
    QVERIFY2(sqlError.isEmpty(), qPrintable(sqlError));

    // Both boundaries must wrap while preserving real media playback.
    next->click();
    QTRY_COMPARE_WITH_TIMEOUT(playlist->currentIndex(), 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player->currentMedia().request().url(), first, 5000);
    previous->click();
    QTRY_COMPARE_WITH_TIMEOUT(playlist->currentIndex(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player->currentMedia().request().url(), second, 5000);
    previous->click();
    QTRY_COMPARE_WITH_TIMEOUT(playlist->currentIndex(), 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player->currentMedia().request().url(), first, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player->state(), QMediaPlayer::PlayingState, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(player->position() > 100, 5000);
    QCOMPARE(player->error(), QMediaPlayer::NoError);

    // Reload the persisted favorites into their real dialog and double-click
    // the second local URI. This covers decoding paths containing spaces and
    // the dialog -> MainWindow playback connection without online search.
    player->stop();
    LikeSong favorites(&window);
    connect(&favorites, &LikeSong::songSelected,
            &window, &MainWindow::onSongSelected);
    QSignalSpy selected(&favorites, &LikeSong::songSelected);
    auto *favoriteTable = favorites.findChild<QTableWidget *>("tableWidget_LikeSong");
    QVERIFY(favoriteTable);
    QCOMPARE(favoriteTable->rowCount(), 2);
    int restoredRow = -1;
    for (int row = 0; row < favoriteTable->rowCount(); ++row) {
        const auto *item = favoriteTable->item(row, 0);
        if (item && item->data(Qt::UserRole).toString()
                == second.toString(QUrl::FullyEncoded)) {
            restoredRow = row;
            break;
        }
    }
    QVERIFY(restoredRow >= 0);
    favorites.show();
    QVERIFY(QTest::qWaitForWindowExposed(&favorites, 5000));
    favoriteTable->selectRow(restoredRow);
    auto *restoredItem = favoriteTable->item(restoredRow, 1);
    QVERIFY(restoredItem);
    const QPoint restoredCell = favoriteTable->visualItemRect(restoredItem).center();
    QTest::mouseClick(favoriteTable->viewport(), Qt::LeftButton,
                      Qt::NoModifier, restoredCell);
    QTest::mouseDClick(favoriteTable->viewport(), Qt::LeftButton,
                     Qt::NoModifier, restoredCell);
    QTRY_COMPARE_WITH_TIMEOUT(selected.count(), 1, 3000);
    QCOMPARE(selected.at(0).at(0).toString(), second.toString(QUrl::FullyEncoded));
    QTRY_COMPARE_WITH_TIMEOUT(playlist->currentIndex(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player->currentMedia().request().url(), second, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player->state(), QMediaPlayer::PlayingState, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(player->position() > 100, 5000);
    QCOMPARE(playlist->mediaCount(), 2);

    player->stop();
    messageCloser.stop();
    window.close();
    Q_UNUSED(acknowledgedMessages);
}

void MacOSSmoke::localLyricsAndFloatingControls()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    auto *player = window.p_PlayerObject;
    auto *playlist = window.p_PlayerList;
    auto *play = window.findChild<QPushButton *>("pushButton_PlaySong");
    auto *next = window.findChild<QPushButton *>("pushButton_PlayNextSong");
    auto *previous = window.findChild<QPushButton *>("pushButton_PreviousSong");
    auto *progress = window.findChild<QSlider *>("horizontalSlider_PlayProgress");
    auto *toggle = window.findChild<QPushButton *>("floatingLyricsButton");
    auto *embedded = window.findChild<LyricsView *>("lyricsView");
    auto *floating = window.findChild<FloatingLyricsWidget *>("floatingLyrics");
    QVERIFY(player);
    QVERIFY(playlist);
    QVERIFY(play);
    QVERIFY(next);
    QVERIFY(previous);
    QVERIFY(progress);
    QVERIFY(toggle);
    QVERIFY(embedded);
    QVERIFY(floating);

    window.addLocalFiles(files);
    QTRY_COMPARE_WITH_TIMEOUT(player->state(), QMediaPlayer::PlayingState, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(player->duration() >= 11000, 10000);
    // Let the native audio clock start before pausing. Qt 5 AVFoundation
    // reuses currentTime's initial whole-second timescale when seeking, which
    // otherwise truncates fractional-second seeks made immediately at zero.
    QTRY_VERIFY_WITH_TIMEOUT(player->position() > 250, 10000);
    play->click();
    QTRY_COMPARE_WITH_TIMEOUT(player->state(), QMediaPlayer::PausedState, 3000);
    QCOMPARE(embedded->document().lines().size(), 3);
    QCOMPARE(floating->document().lines().size(), 3);
    QCOMPARE(embedded->document().lines().first().text, QStringLiteral("第一句歌词"));

    // A paused position makes boundary checks deterministic on the native
    // backend, and verifies that seek updates both visible and hidden widgets.
    player->setPosition(0);
    QTRY_VERIFY_WITH_TIMEOUT(player->position() < 200, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(embedded->currentLineIndex(), -1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(floating->currentLineIndex(), -1, 3000);
    QVERIFY(embedded->accessibleDescription().isEmpty());
    QVERIFY(floating->accessibleDescription().isEmpty());
    QVERIFY(!floating->isVisible());
    QVERIFY(!toggle->isChecked());

    toggle->click();
    QTRY_VERIFY_WITH_TIMEOUT(floating->isVisible(), 3000);
    QVERIFY(toggle->isChecked());
    progress->setValue(4500);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(player->position() - 4500) < 300, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(embedded->currentLineIndex(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(floating->currentLineIndex(), 1, 3000);
    QCOMPARE(embedded->accessibleDescription(), QStringLiteral("第二句歌词"));
    QCOMPARE(floating->accessibleDescription(), QStringLiteral("第二句歌词"));
    progress->setValue(1500);
    QTRY_COMPARE_WITH_TIMEOUT(embedded->currentLineIndex(), 0, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(floating->currentLineIndex(), 0, 3000);

    toggle->click();
    QTRY_VERIFY_WITH_TIMEOUT(!floating->isVisible(), 3000);
    QVERIFY(!toggle->isChecked());
    toggle->click();
    QTRY_VERIFY_WITH_TIMEOUT(floating->isVisible(), 3000);
    floating->close();
    QTRY_VERIFY_WITH_TIMEOUT(!floating->isVisible(), 3000);
    QVERIFY(!toggle->isChecked());
    toggle->click();
    QTRY_VERIFY_WITH_TIMEOUT(floating->isVisible(), 3000);
    QVERIFY(toggle->isChecked());

    // Switching to the other directory's identically named audio file must
    // clear both documents even though that track has no sidecar at all.
    next->click();
    QTRY_COMPARE_WITH_TIMEOUT(playlist->currentIndex(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(embedded->document().isEmpty(), 3000);
    QVERIFY(floating->document().isEmpty());
    QCOMPARE(embedded->currentLineIndex(), -1);
    QCOMPARE(floating->currentLineIndex(), -1);
    QVERIFY(embedded->accessibleDescription().isEmpty());
    QVERIFY(floating->accessibleDescription().isEmpty());

    previous->click();
    QTRY_COMPARE_WITH_TIMEOUT(playlist->currentIndex(), 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(embedded->document().lines().size(), 3, 3000);
    QCOMPARE(floating->document().lines().size(), 3);
    player->stop();
    floating->hide();
    window.close();
}

void MacOSSmoke::queueDoubleClickDoesNotDuplicateTracks()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    auto *player = window.p_PlayerObject;
    auto *playlist = window.p_PlayerList;
    auto *queue = window.findChild<QTableView *>("queueView");
    QVERIFY(player);
    QVERIFY(playlist);
    QVERIFY(queue);
    QVERIFY(queue->model());
    window.addLocalFiles(files);
    QTRY_COMPARE_WITH_TIMEOUT(player->state(), QMediaPlayer::PlayingState, 10000);
    QCOMPARE(playlist->mediaCount(), 2);
    QCOMPARE(queue->model()->rowCount(), 2);

    QSignalSpy doubleClicks(queue, &QTableView::doubleClicked);
    for (const int row : {1, 0, 1}) {
        player->pause();
        QTRY_COMPARE_WITH_TIMEOUT(player->state(), QMediaPlayer::PausedState, 3000);
        const QModelIndex index = queue->model()->index(row, 1);
        QVERIFY(index.isValid());
        queue->scrollTo(index);
        const QPoint cell = queue->visualRect(index).center();
        const int previousDoubleClicks = doubleClicks.count();
        QTest::mouseClick(queue->viewport(), Qt::LeftButton, Qt::NoModifier, cell);
        QTest::mouseDClick(queue->viewport(), Qt::LeftButton, Qt::NoModifier, cell);
        QTRY_VERIFY_WITH_TIMEOUT(doubleClicks.count() > previousDoubleClicks, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(playlist->currentIndex(), row, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(player->currentMedia().request().url(),
                                 QUrl::fromLocalFile(files.at(row)), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(player->state(), QMediaPlayer::PlayingState, 5000);
        QCOMPARE(playlist->mediaCount(), 2);
        QCOMPARE(queue->model()->rowCount(), 2);
    }
    // Re-importing existing local URLs also preserves the two-row queue.
    window.addLocalFiles(files);
    QCOMPARE(playlist->mediaCount(), 2);
    QCOMPARE(queue->model()->rowCount(), 2);
    player->stop();
    window.close();
}

void MacOSSmoke::repeatedPagesReuseTheirChildren()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    auto *favorites = window.findChild<QPushButton *>("pushButton_LikeSong");
    auto *history = window.findChild<QPushButton *>("pushButton_HistorySong");
    QVERIFY(favorites);
    QVERIFY(history);
    auto *stack = window.findChild<QStackedWidget *>("pageStack");
    auto *library = window.findChild<QPushButton *>("libraryButton");
    QVERIFY(stack);
    QVERIFY(library);
    QPointer<LikeSong> favoritePage;
    QPointer<HistorySong> historyPage;
    for (int repetition = 0; repetition < 3; ++repetition) {
        favorites->click();
        auto *currentFavorite = window.findChild<LikeSong *>("favoritesPage");
        QVERIFY(currentFavorite);
        QVERIFY(!currentFavorite->isWindow());
        QCOMPARE(stack->currentWidget(), currentFavorite);
        if (favoritePage) QCOMPARE(currentFavorite, favoritePage.data());
        favoritePage = currentFavorite;
        history->click();
        auto *currentHistory = window.findChild<HistorySong *>("historyPage");
        QVERIFY(currentHistory);
        QVERIFY(!currentHistory->isWindow());
        QCOMPARE(stack->currentWidget(), currentHistory);
        if (historyPage) QCOMPARE(currentHistory, historyPage.data());
        historyPage = currentHistory;
        QCOMPARE(window.findChildren<LikeSong *>().size(), 1);
        QCOMPARE(window.findChildren<HistorySong *>().size(), 1);
        QVERIFY(!favoritePage->isVisible());
        currentHistory->accept();
        QCOMPARE(stack->currentWidget()->objectName(), QStringLiteral("libraryPage"));
        QVERIFY(!currentHistory->isVisible());
        library->click();
    }
    window.close();
}

void MacOSSmoke::localAccountsRoundTrip()
{
    QTemporaryDir accountsData;
    QVERIFY(accountsData.isValid());
    const QString redisProgram = QStringLiteral("/opt/homebrew/opt/redis/bin/redis-server");
    QVERIFY2(QFileInfo(redisProgram).isExecutable(), "Local Redis executable is unavailable");

    // Refuse an occupied port. Never run credentials against an existing Redis
    // instance, including the launcher's separate service on port 16379.
    QTcpServer portReservation;
    QVERIFY2(portReservation.listen(QHostAddress::LocalHost, 16380),
             "Isolated account-test port 16380 is occupied; refusing to reuse it");
    portReservation.close();

    OwnedRedisServer server;
    server.process.setProcessChannelMode(QProcess::MergedChannels);
    server.process.setWorkingDirectory(accountsData.path());
    server.process.start(redisProgram, {
        "--bind", "127.0.0.1", "--port", "16380", "--protected-mode", "yes",
        "--daemonize", "no", "--save", "", "--appendonly", "no",
        "--dir", accountsData.path(), "--dbfilename", "smoke-accounts.rdb",
        "--pidfile", accountsData.filePath("smoke-accounts.pid"),
        "--requirepass", "", "--loglevel", "notice"
    });
    QVERIFY2(server.process.waitForStarted(3000),
             qPrintable(server.process.errorString()));

    QByteArray startupLog;
    QElapsedTimer startupTimeout;
    startupTimeout.start();
    while (startupTimeout.elapsed() < 5000) {
        startupLog += server.process.readAll();
        if (startupLog.contains("Ready to accept connections")
            || server.process.state() == QProcess::NotRunning)
            break;
        server.process.waitForReadyRead(100);
        QCoreApplication::processEvents();
    }
    startupLog += server.process.readAll();
    QVERIFY2(server.process.state() == QProcess::Running
                 && startupLog.contains("Ready to accept connections"),
             qPrintable(QStringLiteral("Our isolated Redis failed to start: ")
                        + QString::fromUtf8(startupLog)));

    ScopedEnvironment accountEnvironment;
    accountEnvironment.set("MUSICPLAYER_REDIS_HOST", "127.0.0.1");
    accountEnvironment.set("MUSICPLAYER_REDIS_PORT", "16380");
    accountEnvironment.set("MUSICPLAYER_REDIS_PASSWORD", "");

    SignUp signup;
    SignIn signin;
    QStringList messageTexts;
    QTimer messageCloser;
    const QList<QWidget *> ownedDialogs{&signup, &signin};
    connect(&messageCloser, &QTimer::timeout, &signup, [&] {
        for (QWidget *owner : ownedDialogs) {
            for (auto *message : owner->findChildren<QMessageBox *>()) {
                if (message->isVisible()) {
                    messageTexts.append(message->text());
                    message->accept();
                }
            }
        }
    });
    messageCloser.start(25);

    auto *signupId = signup.findChild<QLineEdit *>("lineEdit_Id");
    auto *signupPassword = signup.findChild<QLineEdit *>("lineEdit_Password");
    auto *gender = signup.findChild<QRadioButton *>("radioButton_woman");
    auto *registerButton = signup.findChild<QPushButton *>("pushButton_SignUp");
    QVERIFY(signupId);
    QVERIFY(signupPassword);
    QVERIFY(gender);
    QVERIFY(registerButton);

    const QString username = QStringLiteral("FAKE_QT_SMOKE_USER_")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString password = QStringLiteral("FAKE_TEST_ONLY_42!DoNotReuse");
    signup.show();
    QVERIFY(QTest::qWaitForWindowExposed(&signup, 5000));
    signupId->setText(username);
    signupPassword->setText(password);
    gender->click();
    QVERIFY(gender->isChecked());
    registerButton->click();
    QVERIFY2(!messageTexts.isEmpty(), "Registration displayed no result");
    QVERIFY2(messageTexts.last().contains(QStringLiteral("欢迎 ") + username),
             qPrintable(QStringLiteral("Unexpected registration result: ")
                        + messageTexts.last()));
    QVERIFY(!signup.isVisible());

    // Check only the fake account in the Redis process we started. Native macOS
    // message boxes can omit window titles, so the body plus persisted account
    // and subsequent login provide the business evidence for registration.
    {
        QString connectionError;
        RedisContextPtr context(connectLocalRedis(&connectionError), &redisFree);
        QVERIFY2(context != nullptr, qPrintable(connectionError));
        const QByteArray usernameData = username.toUtf8();
        RedisReplyPtr account(static_cast<redisReply *>(redisCommand(
                                  context.get(), "HMGET user:%b username password gender",
                                  usernameData.constData(),
                                  static_cast<size_t>(usernameData.size()))),
                              &freeReplyObject);
        QVERIFY2(account != nullptr, "Reading the isolated fake account failed");
        QCOMPARE(account->type, REDIS_REPLY_ARRAY);
        QCOMPARE(account->elements, size_t(3));
        for (size_t field = 0; field < account->elements; ++field) {
            QVERIFY(account->element[field]);
            QCOMPARE(account->element[field]->type, REDIS_REPLY_STRING);
            QVERIFY(account->element[field]->str);
        }
        QCOMPARE(QByteArray(account->element[0]->str,
                            static_cast<int>(account->element[0]->len)), usernameData);
        QCOMPARE(QByteArray(account->element[1]->str,
                            static_cast<int>(account->element[1]->len)),
                 QCryptographicHash::hash(password.toUtf8(),
                                          QCryptographicHash::Sha256).toHex());
        QCOMPARE(QString::fromUtf8(account->element[2]->str,
                                   static_cast<int>(account->element[2]->len)),
                 QStringLiteral("女"));
    }

    auto *signinId = signin.findChild<QLineEdit *>("lineEdit_Id");
    auto *signinPassword = signin.findChild<QLineEdit *>("lineEdit_Password");
    auto *loginButton = signin.findChild<QPushButton *>("pushButton_SignIn");
    QVERIFY(signinId);
    QVERIFY(signinPassword);
    QVERIFY(loginButton);
    QSignalSpy loggedIn(&signin, &SignIn::userLoggedIn);
    QVERIFY(loggedIn.isValid());
    signin.show();
    QVERIFY(QTest::qWaitForWindowExposed(&signin, 5000));
    signinId->setText(username);
    signinPassword->setText(QStringLiteral("DELIBERATELY_WRONG_FAKE_PASSWORD"));
    const int beforeWrongPassword = messageTexts.count();
    loginButton->click();
    QVERIFY(messageTexts.count() > beforeWrongPassword);
    QVERIFY2(messageTexts.last().contains(QStringLiteral("密码错误")),
             qPrintable(QStringLiteral("Unexpected wrong-password result: ")
                        + messageTexts.last()));
    QCOMPARE(loggedIn.count(), 0);
    QVERIFY(!signin.isSignIn);
    QVERIFY(signin.isVisible());

    signinPassword->setText(password);
    const int beforeCorrectPassword = messageTexts.count();
    loginButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(loggedIn.count(), 1, 3000);
    QCOMPARE(loggedIn.at(0).at(0).toString(), username);
    QVERIFY(messageTexts.count() > beforeCorrectPassword);
    QVERIFY2(messageTexts.last().contains(QStringLiteral("欢迎回来 ") + username),
             qPrintable(QStringLiteral("Unexpected successful-login result: ")
                        + messageTexts.last()));
    QVERIFY(signin.isSignIn);
    QVERIFY(!signin.isVisible());

    messageCloser.stop();
    server.stop();
    QCOMPARE(server.process.state(), QProcess::NotRunning);
    QVERIFY2(!QFileInfo::exists(accountsData.filePath("smoke-accounts.rdb")),
             "Account smoke test unexpectedly persisted Redis data");
}

void MacOSSmoke::manuallyImportedLyricsSurviveWindowRecreation()
{
    const QString audio = files.at(1);
    const QString sidecar = QFileInfo(audio).dir().filePath(QStringLiteral("same-title.lrc"));
    QVERIFY2(!QFileInfo::exists(sidecar), "The persistence test requires a track without a sidecar");
    const QString customPath = isolatedData.filePath(QStringLiteral("custom-selected-lyrics.lrc"));
    const QByteArray customLyrics = QStringLiteral(
        "[00:00.000]手动关联的独立歌词\n[00:04.000]重建窗口后应自动恢复\n").toUtf8();
    {
        QFile custom(customPath);
        QVERIFY(custom.open(QIODevice::WriteOnly));
        QCOMPARE(custom.write(customLyrics), qint64(customLyrics.size()));
    }

    const bool previousAttribute = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    {
        ScopedApplicationAttribute useOwnedQtDialog(Qt::AA_DontUseNativeDialogs, true);
        {
            MainWindow window;
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
            auto *embedded = window.findChild<LyricsView *>("lyricsView");
            auto *floating = window.findChild<FloatingLyricsWidget *>("floatingLyrics");
            auto *importButton = window.findChild<QPushButton *>("importLyricsButton");
            QVERIFY(embedded && floating && importButton);
            window.addLocalFiles({audio});
            QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerList->currentIndex(), 0, 5000);
            window.p_PlayerObject->stop();
            QVERIFY(embedded->document().isEmpty());
            QVERIFY(floating->document().isEmpty());

            bool openedOwnedFileDialog = false;
            bool invokedAccept = false;
            bool acceptedSelection = false;
            bool preparedDirectory = false;
            QStringList chosenFiles;
            QStringList chooserMessages;
            QTimer fileChooser;
            connect(&fileChooser, &QTimer::timeout, &window, [&] {
                for (auto *dialog : window.findChildren<QFileDialog *>()) {
                    if (!dialog->isVisible()) continue;
                    openedOwnedFileDialog = true;
                    if (!preparedDirectory) {
                        connect(dialog, &QFileDialog::accepted, &window, [&] {
                            acceptedSelection = true;
                        });
                        dialog->setDirectory(QFileInfo(customPath).absolutePath());
                        preparedDirectory = true;
                        return;
                    }
                    dialog->selectFile(QFileInfo(customPath).fileName());
                    // A visible Qt chooser may preserve the focused filename
                    // edit when selectFile() is called. Enter the same selection
                    // into that owned control before accepting the real dialog.
                    if (auto *filename = dialog->findChild<QLineEdit *>("fileNameEdit"))
                        filename->setText(QFileInfo(customPath).fileName());
                    chosenFiles = dialog->selectedFiles();
                    if (chosenFiles.size() != 1
                        || QFileInfo(chosenFiles.first()).canonicalFilePath()
                               != QFileInfo(customPath).canonicalFilePath()) return;
                    invokedAccept = QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
                }
            });
            QTimer ownedMessageCloser;
            connect(&ownedMessageCloser, &QTimer::timeout, &window, [&] {
                for (auto *message : window.findChildren<QMessageBox *>()) {
                    if (!message->isVisible()) continue;
                    chooserMessages.append(message->text());
                    message->accept();
                }
            });
            // If the chooser stops accepting files, fail within this window
            // instead of sending a desktop-wide key or hanging the test run.
            QTimer chooserDeadline;
            chooserDeadline.setSingleShot(true);
            connect(&chooserDeadline, &QTimer::timeout, &window, [&] {
                for (auto *dialog : window.findChildren<QFileDialog *>()) dialog->reject();
            });
            fileChooser.start(25);
            ownedMessageCloser.start(25);
            chooserDeadline.start(5000);
            importButton->click();
            fileChooser.stop();
            ownedMessageCloser.stop();
            chooserDeadline.stop();
            QVERIFY(openedOwnedFileDialog);
            QVERIFY(invokedAccept);
            QVERIFY(acceptedSelection);
            QVERIFY2(chooserMessages.isEmpty(), qPrintable(chooserMessages.join(QLatin1Char('\n'))));
            QCOMPARE(QFileInfo(chosenFiles.first()).canonicalFilePath(), QFileInfo(customPath).canonicalFilePath());
            QCOMPARE(embedded->document().lines().size(), 2);
            QCOMPARE(floating->document().lines().size(), 2);
            QCOMPARE(embedded->document().lines().first().text, QStringLiteral("手动关联的独立歌词"));
            QVERIFY2(!QFileInfo::exists(sidecar), "Import must retain the separately selected lyrics file");
            window.close();
        }

        // All per-window lyric hashes and widgets have now been destroyed.
        // The new window must recover the association from the isolated store.
        MainWindow restored;
        restored.show();
        QVERIFY(QTest::qWaitForWindowExposed(&restored, 5000));
        auto *embedded = restored.findChild<LyricsView *>("lyricsView");
        auto *floating = restored.findChild<FloatingLyricsWidget *>("floatingLyrics");
        QVERIFY(embedded && floating);
        restored.addLocalFiles({audio});
        QTRY_COMPARE_WITH_TIMEOUT(restored.p_PlayerList->currentIndex(), 0, 5000);
        restored.p_PlayerObject->stop();
        QCOMPARE(embedded->document().lines().size(), 2);
        QCOMPARE(floating->document().lines().size(), 2);
        QCOMPARE(embedded->document().lines().at(0).text, QStringLiteral("手动关联的独立歌词"));
        QCOMPARE(embedded->document().lines().at(1).text, QStringLiteral("重建窗口后应自动恢复"));
        QCOMPARE(floating->document().lines().at(1).text, QStringLiteral("重建窗口后应自动恢复"));
        restored.close();
    }
    QCOMPARE(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs), previousAttribute);
}

void MacOSSmoke::cleanupTestCase()
{
    if (hadDataDir)
        qputenv("MUSICPLAYER_DATA_DIR", previousDataDir);
    else
        qunsetenv("MUSICPLAYER_DATA_DIR");
}

QTEST_MAIN(MacOSSmoke)
#include "macos_smoke.moc"
