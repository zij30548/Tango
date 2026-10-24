#include <QtTest>
#include <QAction>
#include <QBuffer>
#include <QDataStream>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

#include "focuscore.h"
#include "focussession.h"
#include "mainwindow.h"

namespace {
class ScopedEnvironment
{
public:
    ~ScopedEnvironment()
    {
        for (auto it = saved.crbegin(); it != saved.crend(); ++it) {
            if (it->existed) qputenv(it->name.constData(), it->value);
            else qunsetenv(it->name.constData());
        }
    }
    void set(const QByteArray &name, const QByteArray &value)
    {
        saved.append({name, qgetenv(name.constData()), qEnvironmentVariableIsSet(name.constData())});
        qputenv(name.constData(), value);
    }
private:
    struct Value { QByteArray name, value; bool existed; };
    QVector<Value> saved;
};

QByteArray silentWave(int seconds = 12)
{
    constexpr quint32 rate = 22050;
    const quint32 size = rate * seconds * sizeof(qint16);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + size);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << rate
           << quint32(rate * sizeof(qint16)) << quint16(sizeof(qint16)) << quint16(16);
    stream.writeRawData("data", 4);
    stream << size;
    bytes.append(QByteArray(size, '\0'));
    return bytes;
}

FocusTrack candidate(const QString &key)
{
    FocusTrack track;
    track.key = key;
    track.title = key;
    track.artist = QStringLiteral("Fixture artist");
    track.url = QUrl::fromLocalFile(QStringLiteral("/isolated-fixture/") + key + QStringLiteral(".wav"));
    track.durationMs = 12000;
    return track;
}

FocusPlan sessionPlan(int count = 2)
{
    FocusPlan plan;
    plan.request.minutes = 1;
    for (int index = 0; index < count; ++index) plan.tracks.append(candidate(QString::number(index)));
    return plan;
}

int rowCount(const QString &table)
{
    QSqlQuery query;
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table) || !query.next()) return -1;
    return query.value(0).toInt();
}

bool insertRows(const QString &tag)
{
    const QStringList tables{QStringLiteral("favorite_songs"), QStringLiteral("play_history")};
    for (const auto &table : tables) {
        QSqlQuery query;
        query.prepare(QStringLiteral("INSERT INTO ") + table + QStringLiteral(" (song_id,song_name,artist_name,media_url) VALUES(?,?,?,?)"));
        query.addBindValue(-17);
        query.addBindValue(tag);
        query.addBindValue(QStringLiteral("Fixture artist"));
        query.addBindValue(QStringLiteral("file:///isolated/") + tag + QStringLiteral(".wav"));
        if (!query.exec()) return false;
    }
    return true;
}

FocusPlan preview(MainWindow &window)
{
    FocusRequest request;
    request.minutes = 1;
    FocusStore store(QSqlDatabase::database());
    return FocusPlanner::build(request, window.focusCandidates(request.source),
                               store.rules(window.accountProfile(), request.scene));
}
}

class FocusSessionTest : public QObject
{
    Q_OBJECT
private slots:
    void init();
    void cleanup();
    void injectedClockCountsPlayingTimeAndFinishesOnce();
    void explicitPauseSurvivesSessionNavigation();
    void validatesPlansAndBoundsFailures();
    void guestDataAndAccountPreferencesRemainIsolated();
    void failedAccountMigrationKeepsCurrentDatabase();
    void localPlaybackRunsPlanAndManualSelectionCancels();
    void pauseAndEndControlRealPlayback();
    void pausedSkipPreservesPlaybackIntent_data();
    void pausedSkipPreservesPlaybackIntent();
    void missingFilesExitWithoutUnboundedRetries();
    void replacedPlanIgnoresQueuedFailure();
    void changedRulesInvalidateStalePreview();
    void naturalCompletionPausesRealPlayback();
private:
    QStringList createMusic(int count);
    std::unique_ptr<QTemporaryDir> data;
    std::unique_ptr<ScopedEnvironment> environment;
};

void FocusSessionTest::init()
{
    data = std::make_unique<QTemporaryDir>();
    QVERIFY(data->isValid());
    environment = std::make_unique<ScopedEnvironment>();
    environment->set("MUSICPLAYER_DATA_DIR", data->path().toUtf8());
    // No test calls the cloud or authentication. An overridden port also
    // prevents accidental launch of the real user's local NetEase service.
    environment->set("MUSICPLAYER_NETEASE_PORT", "9");
    environment->set("MUSICPLAYER_REDIS_PORT", "9");
    qRegisterMetaType<QMediaPlayer::Error>();
    qRegisterMetaType<FocusPlan>();
}

void FocusSessionTest::cleanup()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (QSqlDatabase::contains()) {
        QSqlDatabase::database(QSqlDatabase::defaultConnection, false).close();
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }
    environment.reset();
    data.reset();
}

QStringList FocusSessionTest::createMusic(int count)
{
    QStringList files;
    const QByteArray bytes = silentWave();
    for (int index = 0; index < count; ++index) {
        const QString path = data->filePath(QStringLiteral("Quiet fixture %1.wav").arg(index));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return {};
        files.append(path);
    }
    return files;
}

void FocusSessionTest::injectedClockCountsPlayingTimeAndFinishesOnce()
{
    qint64 now = 1000;
    FocusSession session(nullptr, [&now] { return now; });
    QSignalSpy tracks(&session, &FocusSession::trackRequested);
    QSignalSpy finished(&session, &FocusSession::finished);
    QVERIFY(session.start(sessionPlan()));
    QCOMPARE(tracks.count(), 1);
    QCOMPARE(session.remainingSeconds(), 60);
    now += 100000;
    session.tick(); // Loading time is free.
    QCOMPARE(session.remainingSeconds(), 60);
    session.setPlaybackRunning(true);
    for (int second = 0; second < 17; ++second) { now += 1000; session.tick(); }
    QCOMPARE(session.remainingSeconds(), 43);
    session.setPlaybackRunning(false);
    now += 600000;
    session.tick();
    QCOMPARE(session.remainingSeconds(), 43);
    session.setPlaybackRunning(true);
    for (int second = 0; second < 43; ++second) { now += 1000; session.tick(); }
    QVERIFY(!session.isActive());
    QVERIFY(!session.isRunning());
    QCOMPARE(session.remainingSeconds(), 0);
    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.first().at(1).toBool());
    session.tick();
    session.next(true);
    session.cancel();
    QCOMPARE(finished.count(), 1);
}

void FocusSessionTest::validatesPlansAndBoundsFailures()
{
    qint64 now = 0;
    FocusSession session(nullptr, [&now] { return now; });
    QSignalSpy tracks(&session, &FocusSession::trackRequested);
    QSignalSpy finished(&session, &FocusSession::finished);
    auto invalid = sessionPlan();
    invalid.tracks.append(invalid.tracks.first());
    QVERIFY(!session.start(invalid));
    invalid = sessionPlan();
    invalid.error = QStringLiteral("Rejected fixture");
    QVERIFY(!session.start(invalid));
    QVERIFY(session.start(sessionPlan(3)));
    const auto initialGeneration = session.generation();
    session.next(true);
    QVERIFY(session.generation() > initialGeneration);
    QCOMPARE(session.currentKey(), QStringLiteral("1"));
    session.next(true);
    QCOMPARE(session.currentKey(), QStringLiteral("2"));
    session.previous(); // Failed tracks are skipped; the only good song remains.
    QCOMPARE(session.currentKey(), QStringLiteral("2"));
    session.next(true);
    QVERIFY(!session.isActive());
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.first().at(1).toBool());
    QCOMPARE(tracks.count(), 4);
    session.next(true);
    QCOMPARE(tracks.count(), 4);
    const auto endedGeneration = session.generation();
    QVERIFY(session.start(sessionPlan()));
    QVERIFY(session.generation() > endedGeneration);
    session.cancel();
    QVERIFY(session.generation() > endedGeneration + 1);
}

void FocusSessionTest::explicitPauseSurvivesSessionNavigation()
{
    qint64 now = 0;
    FocusSession session(nullptr, [&now] { return now; });
    QVERIFY(session.start(sessionPlan(3)));
    session.setPlaybackRunning(true);
    now += 1000;
    session.tick();
    QCOMPARE(session.remainingSeconds(), 59);
    session.setPaused(true);
    QVERIFY(session.isPaused());
    QVERIFY(!session.isRunning());
    session.next(true);
    QCOMPARE(session.currentKey(), QStringLiteral("1"));
    QVERIFY(session.isPaused());
    // A delayed backend state must not defeat the user's explicit pause.
    session.setPlaybackRunning(true);
    QVERIFY(!session.isRunning());
    now += 100000;
    session.tick();
    QCOMPARE(session.remainingSeconds(), 59);
    session.next();
    QCOMPARE(session.currentKey(), QStringLiteral("2"));
    QVERIFY(session.isPaused());
    session.previous();
    QCOMPARE(session.currentKey(), QStringLiteral("1"));
    QVERIFY(session.isPaused());
    session.setPaused(false);
    QVERIFY(!session.isPaused());
    session.setPlaybackRunning(true);
    now += 1000;
    session.tick();
    QCOMPARE(session.remainingSeconds(), 58);
    session.cancel();
}

void FocusSessionTest::guestDataAndAccountPreferencesRemainIsolated()
{
    // Populate the exact legacy guest database before constructing the new UI.
    {
        auto legacy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy_fixture"));
        legacy.setDatabaseName(data->filePath(QStringLiteral("history.db")));
        QVERIFY(legacy.open());
        QSqlQuery query(legacy);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE play_history (id INTEGER PRIMARY KEY AUTOINCREMENT,song_id INTEGER NOT NULL,song_name TEXT NOT NULL,artist_name TEXT NOT NULL DEFAULT '',media_url TEXT NOT NULL,play_time TEXT DEFAULT (datetime('now')))")));
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE favorite_songs (id INTEGER PRIMARY KEY AUTOINCREMENT,song_id INTEGER NOT NULL,song_name TEXT NOT NULL,artist_name TEXT NOT NULL DEFAULT '',media_url TEXT NOT NULL UNIQUE)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO favorite_songs(song_id,song_name,media_url) VALUES(-1,'Legacy guest','file:///legacy.wav')")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO play_history(song_id,song_name,media_url) VALUES(-1,'Legacy guest','file:///legacy.wav')")));
        legacy.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("legacy_fixture"));
    {
        MainWindow window;
        QCOMPARE(window.accountProfile(), QStringLiteral("guest"));
        QCOMPARE(rowCount(QStringLiteral("favorite_songs")), 1);
        QCOMPARE(rowCount(QStringLiteral("play_history")), 1);
        QVERIFY(FocusStore(QSqlDatabase::database()).addRule(QStringLiteral("guest"), QStringLiteral("work"), QStringLiteral("exclude_live")) > 0);
        // updateUsername is the existing successful-auth callback, not a test
        // of credentials or a replacement for the authentication service.
        window.updateUsername(QStringLiteral("fixture-alice"));
        QCOMPARE(window.accountProfile(), QStringLiteral("local:fixture-alice"));
        QVERIFY(window.isSign);
        QCOMPARE(rowCount(QStringLiteral("favorite_songs")), 0);
        QCOMPARE(rowCount(QStringLiteral("play_history")), 0);
        QVERIFY(insertRows(QStringLiteral("Alice")));
        QVERIFY(FocusStore(QSqlDatabase::database()).addRule(window.accountProfile(), QStringLiteral("study"), QStringLiteral("instrumental_only")) > 0);
        window.updateUsername(QStringLiteral("fixture-bob"));
        QCOMPARE(rowCount(QStringLiteral("favorite_songs")), 0);
        QCOMPARE(rowCount(QStringLiteral("play_history")), 0);
        QVERIFY(FocusStore(QSqlDatabase::database()).rules(window.accountProfile()).isEmpty());
        QVERIFY(FocusStore(QSqlDatabase::database()).rules(QStringLiteral("local:fixture-alice")).isEmpty());
        QVERIFY(insertRows(QStringLiteral("Bob")));
        window.updateUsername(QStringLiteral("fixture-alice"));
        QCOMPARE(rowCount(QStringLiteral("favorite_songs")), 1);
        QCOMPARE(rowCount(QStringLiteral("play_history")), 1);
        const auto rules = FocusStore(QSqlDatabase::database()).rules(window.accountProfile());
        QCOMPARE(rules.size(), 1);
        QCOMPARE(rules.first().kind, QStringLiteral("instrumental_only"));
        QCOMPARE(rules.first().scene, QStringLiteral("study"));
        QVERIFY(QMetaObject::invokeMethod(&window, "logoutLocalAccount", Qt::DirectConnection));
        QCOMPARE(window.accountProfile(), QStringLiteral("guest"));
        QVERIFY(!window.isSign);
        QCOMPARE(rowCount(QStringLiteral("favorite_songs")), 1);
        QCOMPARE(rowCount(QStringLiteral("play_history")), 1);
        const auto guestRules = FocusStore(QSqlDatabase::database()).rules(QStringLiteral("guest"));
        QCOMPARE(guestRules.size(), 1);
        QCOMPARE(guestRules.first().kind, QStringLiteral("exclude_live"));
    }
    MainWindow reopened;
    QCOMPARE(reopened.accountProfile(), QStringLiteral("guest"));
    QCOMPARE(rowCount(QStringLiteral("favorite_songs")), 1);
    QCOMPARE(FocusStore(QSqlDatabase::database()).rules(QStringLiteral("guest")).size(), 1);
}

void FocusSessionTest::localPlaybackRunsPlanAndManualSelectionCancels()
{
    const auto music = createMusic(3);
    QCOMPARE(music.size(), 3);
    MainWindow window;
    window.p_PlayerObject->setMuted(true);
    window.addLocalFiles(music);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    auto *session = window.findChild<FocusSession *>(QStringLiteral("focusSession"));
    QVERIFY(session);
    const auto candidates = window.focusCandidates(QStringLiteral("queue"));
    QCOMPARE(candidates.size(), 3);
    QVERIFY(FocusStore(QSqlDatabase::database()).addRule(window.accountProfile(), QStringLiteral("work"), QStringLiteral("exclude_track"), candidates.at(2).key) > 0);
    const auto plan = preview(window);
    QCOMPARE(plan.tracks.size(), 2);
    QVERIFY(window.startFocusPlan(plan));
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QTRY_VERIFY_WITH_TIMEOUT(session->isRunning(), 7000);
    QCOMPARE(session->currentKey(), plan.tracks.first().key);
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), plan.tracks.first().url);
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("shuffleButton"))->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_PlayNextSong_clicked", Qt::DirectConnection));
    QVERIFY(session->isActive());
    QCOMPARE(session->currentKey(), plan.tracks.at(1).key);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->currentMedia().request().url(), plan.tracks.at(1).url, 7000);
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_PreviousSong_clicked", Qt::DirectConnection));
    QVERIFY(session->isActive());
    QCOMPARE(session->currentKey(), plan.tracks.first().key);
    window.onSongSelected(QUrl::fromLocalFile(music.at(2)).toString());
    QVERIFY(!session->isActive());
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), QUrl::fromLocalFile(music.at(2)));
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QVERIFY(window.findChild<QPushButton *>(QStringLiteral("shuffleButton"))->isEnabled());
}

void FocusSessionTest::failedAccountMigrationKeepsCurrentDatabase()
{
    MainWindow window;
    window.updateUsername(QStringLiteral("fixture-alice"));
    QVERIFY(insertRows(QStringLiteral("Alice")));
    const QString originalPath = QSqlDatabase::database().databaseName();
    const QString targetProfile = QStringLiteral("local:fixture-broken");
    const QString identity = QString::fromLatin1(QCryptographicHash::hash(targetProfile.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QString targetPath = data->filePath(QStringLiteral("profiles/") + identity + QStringLiteral(".db"));
    {
        auto broken = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("broken_fixture"));
        broken.setDatabaseName(targetPath);
        QVERIFY(broken.open());
        QSqlQuery query(broken);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE favorite_songs (id INTEGER PRIMARY KEY AUTOINCREMENT,song_id INTEGER,song_name TEXT,artist_name TEXT,media_url TEXT)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO favorite_songs(song_id,song_name,media_url) VALUES(-1,'Broken A','file:///duplicate.wav'),(-2,'Broken B','file:///duplicate.wav')")));
        broken.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("broken_fixture"));
    bool dismissed = false;
    QTimer dismiss;
    dismiss.setInterval(10);
    connect(&dismiss, &QTimer::timeout, &window, [&dismissed] {
        for (auto *widget : QApplication::topLevelWidgets()) {
            if (auto *message = qobject_cast<QMessageBox *>(widget)) {
                dismissed = true;
                message->accept();
            }
        }
    });
    dismiss.start();
    window.updateUsername(QStringLiteral("fixture-broken"));
    dismiss.stop();
    QVERIFY(dismissed);
    QCOMPARE(window.accountProfile(), QStringLiteral("local:fixture-alice"));
    QCOMPARE(QSqlDatabase::database().databaseName(), originalPath);
    QVERIFY(QSqlDatabase::database().isOpen());
    QCOMPARE(rowCount(QStringLiteral("favorite_songs")), 1);
    QSqlQuery query;
    QVERIFY(query.exec(QStringLiteral("SELECT song_name FROM favorite_songs")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), QStringLiteral("Alice"));
}

void FocusSessionTest::pauseAndEndControlRealPlayback()
{
    const auto music = createMusic(1);
    QCOMPARE(music.size(), 1);
    MainWindow window;
    window.p_PlayerObject->setMuted(true);
    window.addLocalFiles(music);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    auto *session = window.findChild<FocusSession *>(QStringLiteral("focusSession"));
    QVERIFY(session);
    QVERIFY(window.startFocusPlan(preview(window)));
    QTRY_VERIFY_WITH_TIMEOUT(session->isRunning(), 7000);
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_PlaySong_clicked", Qt::DirectConnection));
    QTRY_COMPARE(window.p_PlayerObject->state(), QMediaPlayer::PausedState);
    QVERIFY(session->isActive());
    QVERIFY(!session->isRunning());
    const int remaining = session->remainingSeconds();
    QTest::qWait(1150);
    session->tick();
    QCOMPARE(session->remainingSeconds(), remaining);
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_PlaySong_clicked", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(session->isRunning(), 7000);
    session->cancel(QStringLiteral("Fixture end"));
    QVERIFY(!session->isActive());
    QCOMPARE(window.p_PlayerObject->state(), QMediaPlayer::PausedState);
    QVERIFY(window.findChild<QPushButton *>(QStringLiteral("repeatButton"))->isEnabled());
}

void FocusSessionTest::missingFilesExitWithoutUnboundedRetries()
{
    const auto music = createMusic(3);
    QCOMPARE(music.size(), 3);
    MainWindow window;
    window.p_PlayerObject->setMuted(true);
    window.addLocalFiles(music);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_StopPlaySong_clicked", Qt::DirectConnection));
    const auto plan = preview(window);
    QCOMPARE(plan.tracks.size(), 3);
    for (const auto &path : music) QVERIFY(QFile::remove(path));
    auto *session = window.findChild<FocusSession *>(QStringLiteral("focusSession"));
    QVERIFY(session);
    QSignalSpy tracks(session, &FocusSession::trackRequested);
    QSignalSpy finished(session, &FocusSession::finished);
    QVERIFY(window.startFocusPlan(plan));
    QTRY_VERIFY_WITH_TIMEOUT(!session->isActive(), 3000);
    QCOMPARE(tracks.count(), 3);
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.first().at(1).toBool());
    QTest::qWait(50);
    QCOMPARE(tracks.count(), 3);
}

void FocusSessionTest::pausedSkipPreservesPlaybackIntent_data()
{
    QTest::addColumn<bool>("backendError");
    QTest::newRow("failed-track-navigation") << false;
    QTest::newRow("queued-media-error") << true;
}

void FocusSessionTest::pausedSkipPreservesPlaybackIntent()
{
    QFETCH(bool, backendError);
    const auto music = createMusic(2);
    QCOMPARE(music.size(), 2);
    MainWindow window;
    window.p_PlayerObject->setMuted(true);
    window.addLocalFiles(music);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    auto *session = window.findChild<FocusSession *>(QStringLiteral("focusSession"));
    QVERIFY(session);
    const auto plan = preview(window);
    QCOMPARE(plan.tracks.size(), 2);
    QVERIFY(window.startFocusPlan(plan));
    QTRY_VERIFY_WITH_TIMEOUT(session->isRunning(), 7000);
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_PlaySong_clicked", Qt::DirectConnection));
    QTRY_COMPARE(window.p_PlayerObject->state(), QMediaPlayer::PausedState);
    QVERIFY(session->isPaused());
    QVERIFY(!session->isRunning());
    const int remaining = session->remainingSeconds();
    if (backendError) {
        QVERIFY(QMetaObject::invokeMethod(window.p_PlayerObject, "error", Qt::DirectConnection,
                                         Q_ARG(QMediaPlayer::Error, QMediaPlayer::ResourceError)));
    } else {
        session->next(true);
    }
    QTRY_COMPARE_WITH_TIMEOUT(session->currentKey(), plan.tracks.at(1).key, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->currentMedia().request().url(), plan.tracks.at(1).url, 3000);
    QVERIFY(session->isActive());
    QVERIFY(session->isPaused());
    QVERIFY(!session->isRunning());
    QVERIFY(window.p_PlayerObject->state() != QMediaPlayer::PlayingState);
    QTest::qWait(1150);
    session->tick();
    QCOMPARE(session->remainingSeconds(), remaining);
    QVERIFY(window.p_PlayerObject->state() != QMediaPlayer::PlayingState);
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_PlaySong_clicked", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QTRY_VERIFY_WITH_TIMEOUT(session->isRunning(), 7000);
    QVERIFY(!session->isPaused());
    QCOMPARE(session->currentKey(), plan.tracks.at(1).key);
    session->cancel();
}

void FocusSessionTest::replacedPlanIgnoresQueuedFailure()
{
    const auto music = createMusic(2);
    QCOMPARE(music.size(), 2);
    MainWindow window;
    window.p_PlayerObject->setMuted(true);
    window.addLocalFiles(music);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    const auto plan = preview(window);
    auto *session = window.findChild<FocusSession *>(QStringLiteral("focusSession"));
    QVERIFY(session);
    QVERIFY(window.startFocusPlan(plan));
    QTRY_VERIFY_WITH_TIMEOUT(session->isRunning(), 7000);
    QSignalSpy tracks(session, &FocusSession::trackRequested);
    // The application's actual media-error handler queues a failed-track skip.
    // Replacing the plan with the same first key must invalidate that old skip.
    QVERIFY(QMetaObject::invokeMethod(window.p_PlayerObject, "error", Qt::DirectConnection,
                                     Q_ARG(QMediaPlayer::Error, QMediaPlayer::ResourceError)));
    const auto oldGeneration = session->generation();
    QVERIFY(window.startFocusPlan(plan));
    QVERIFY(session->generation() > oldGeneration);
    QTRY_VERIFY_WITH_TIMEOUT(session->isRunning(), 7000);
    QTest::qWait(50);
    QVERIFY(session->isActive());
    QCOMPARE(session->currentKey(), plan.tracks.first().key);
    QCOMPARE(tracks.count(), 1);
}

void FocusSessionTest::changedRulesInvalidateStalePreview()
{
    const auto music = createMusic(2);
    QCOMPARE(music.size(), 2);
    MainWindow window;
    window.p_PlayerObject->setMuted(true);
    window.addLocalFiles(music);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    const auto stale = preview(window);
    QVERIFY(FocusStore(QSqlDatabase::database()).addRule(window.accountProfile(), QStringLiteral("work"), QStringLiteral("exclude_track"), stale.tracks.first().key) > 0);
    QVERIFY(!window.startFocusPlan(stale));
    auto *session = window.findChild<FocusSession *>(QStringLiteral("focusSession"));
    QVERIFY(session);
    QVERIFY(!session->isActive());
    const auto fresh = preview(window);
    QCOMPARE(fresh.tracks.size(), 1);
    QVERIFY(window.startFocusPlan(fresh));
    QVERIFY(session->isActive());
    QCOMPARE(session->currentKey(), fresh.tracks.first().key);
    // A successful login callback also ends the active plan before switching data.
    window.updateUsername(QStringLiteral("fixture-alice"));
    QVERIFY(!session->isActive());
    QCOMPARE(window.p_PlayerObject->state(), QMediaPlayer::PausedState);
}

void FocusSessionTest::naturalCompletionPausesRealPlayback()
{
    const auto music = createMusic(1);
    QCOMPARE(music.size(), 1);
    MainWindow window;
    window.p_PlayerObject->setMuted(true);
    window.addLocalFiles(music);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    auto *session = window.findChild<FocusSession *>(QStringLiteral("focusSession"));
    QVERIFY(session);
    QSignalSpy finished(session, &FocusSession::finished);
    QSignalSpy tracks(session, &FocusSession::trackRequested);
    QVERIFY(window.startFocusPlan(preview(window)));
    QTRY_VERIFY_WITH_TIMEOUT(session->isRunning(), 7000);
    // A complete one-minute real session also verifies automatic end-of-media
    // cycling of a 12-second silent WAV, without substituting fake Qt signals.
    QTRY_VERIFY_WITH_TIMEOUT(!session->isActive(), 70000);
    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.first().at(1).toBool());
    QCOMPARE(session->remainingSeconds(), 0);
    QCOMPARE(window.p_PlayerObject->state(), QMediaPlayer::PausedState);
    QVERIFY(tracks.count() >= 4);
    QVERIFY(window.findChild<QPushButton *>(QStringLiteral("shuffleButton"))->isEnabled());
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("focusStatusLabel"))->text().contains(QStringLiteral("完成")));
}

QTEST_MAIN(FocusSessionTest)
#include "focussession_test.moc"
