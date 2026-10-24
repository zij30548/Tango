#include <QtTest>
#include <QSqlDatabase>
#include <QTemporaryDir>
#include "focuscore.h"

namespace {
FocusTrack track(const QString &key, qint64 duration = 180000)
{
    FocusTrack result;
    result.key = key;
    result.title = QStringLiteral("曲目 ") + key;
    result.artist = QStringLiteral("歌手甲");
    result.url = QUrl::fromLocalFile(QStringLiteral("/music/") + key + QStringLiteral(".mp3"));
    result.durationMs = duration;
    return result;
}
}

class FocusCoreTest : public QObject
{
    Q_OBJECT
private slots:
    void parsesOnlySupportedInstructions();
    void rejectsUnsupportedAndAmbiguousInstructions();
    void filtersRealTracksAndDoesNotInventAudioAttributes();
    void honorsSceneRulesAndExactArtistCredits();
    void reportsUnknownAndInsufficientDuration();
    void persistenceIsScopedAndExplicit();
    void labelsAreScopedAndDurableWithoutAudioCopies();
    void rejectsInvalidStoreInputs();
};

void FocusCoreTest::parsesOnlySupportedInstructions()
{
    FocusRequest request;
    QString error;
    QVERIFY(FocusPlanner::parse(QStringLiteral("我要写代码 50 分钟，从我的收藏里选音乐，不要现场版，不要人声，结束提醒我"),
                                FocusRequest(), &request, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(request.minutes, 50);
    QCOMPARE(request.scene, QStringLiteral("work"));
    QCOMPARE(request.source, QStringLiteral("favorites"));
    QVERIFY(request.excludeLive);
    QVERIFY(request.instrumentalOnly);
    const auto withArtist = FocusPlanner::parse(QStringLiteral("学习25分钟，排除歌手：歌手甲、歌手乙"), request, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(withArtist.scene, QStringLiteral("study"));
    QCOMPARE(withArtist.minutes, 25);
    QCOMPARE(withArtist.excludedArtists, QStringList({QStringLiteral("歌手甲"), QStringLiteral("歌手乙")}));
    QCOMPARE(withArtist.originalText, QStringLiteral("学习25分钟，排除歌手：歌手甲、歌手乙"));
    const auto structured = FocusPlanner::parse(QString(), request, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(structured.source, request.source);
}

void FocusCoreTest::rejectsUnsupportedAndAmbiguousInstructions()
{
    const QStringList unsupported{
        QStringLiteral("工作25分钟，舒缓爵士，节奏80bpm"),
        QStringLiteral("根据我的心情选歌"), QStringLiteral("不要歌手甲"),
        QStringLiteral("工作学习25分钟"), QStringLiteral("队列和收藏25分钟"),
        QStringLiteral("25分钟，50分钟"), QStringLiteral("0分钟"),
        QStringLiteral("181分钟"), QStringLiteral("1.5分钟"),
        QStringLiteral("排除歌手：歌手甲 不要现场版"),
        QStringLiteral("尽量没人声")
    };
    for (const QString &text : unsupported) {
        QString error;
        FocusRequest output;
        output.minutes = 33;
        QVERIFY2(!FocusPlanner::parse(text, FocusRequest(), &output, &error), qPrintable(text));
        QVERIFY2(!error.isEmpty(), qPrintable(text));
        QCOMPARE(output.minutes, 33); // Failure must not partially change a pending plan.
    }
    FocusRequest invalid;
    invalid.scene = QStringLiteral("unknown");
    QVERIFY(!FocusPlanner::build(invalid, {track(QStringLiteral("1"))}).error.isEmpty());
}

void FocusCoreTest::filtersRealTracksAndDoesNotInventAudioAttributes()
{
    auto unknown = track(QStringLiteral("unknown"));
    unknown.title = QStringLiteral("纯音乐 Relaxing Piano");
    auto known = track(QStringLiteral("known"));
    known.instrumental = 1;
    auto live = track(QStringLiteral("live"));
    live.instrumental = 1;
    live.live = 1;
    auto namedLive = track(QStringLiteral("namedlive"));
    namedLive.instrumental = 1;
    namedLive.title = QStringLiteral("Piano (LIVE 2025)");
    auto notInstrumental = track(QStringLiteral("voice"));
    notInstrumental.instrumental = 0;
    auto explicitStudio = namedLive;
    explicitStudio.key = QStringLiteral("studio");
    explicitStudio.live = 0;
    auto invalid = known;
    invalid.key.clear();
    FocusRequest request;
    request.instrumentalOnly = true;
    request.excludeLive = true;
    const auto plan = FocusPlanner::build(request, {unknown, known, known, live, namedLive,
                                                   notInstrumental, invalid, explicitStudio});
    QVERIFY(plan.error.isEmpty());
    QCOMPARE(plan.tracks.size(), 2);
    QCOMPARE(plan.tracks.at(0).key, known.key);
    QCOMPARE(plan.tracks.at(1).key, explicitStudio.key);
    QVERIFY(plan.notes.join(QLatin1Char('\n')).contains(QStringLiteral("未标注")));
    QVERIFY(plan.notes.join(QLatin1Char('\n')).contains(QStringLiteral("无法保证")));
    const auto empty = FocusPlanner::build(request, {unknown});
    QVERIFY(!empty.error.isEmpty());
    QVERIFY(empty.tracks.isEmpty());
}

void FocusCoreTest::honorsSceneRulesAndExactArtistCredits()
{
    auto first = track(QStringLiteral("1"));
    first.favorite = true;
    first.artist = QStringLiteral("歌手甲 / 歌手乙");
    auto second = track(QStringLiteral("2"));
    second.favorite = true;
    second.artist = QStringLiteral("歌手甲乙");
    auto third = track(QStringLiteral("3"));
    auto fourth = track(QStringLiteral("4"));
    fourth.favorite = true;
    const QVector<FocusRule> rules{
        {1, QStringLiteral("work"), QStringLiteral("exclude_artist"), QStringLiteral("歌手甲")},
        {2, QStringLiteral("study"), QStringLiteral("exclude_track"), second.key},
        {3, QStringLiteral("work"), QStringLiteral("exclude_track"), fourth.key}
    };
    FocusRequest request;
    request.source = QStringLiteral("favorites");
    auto plan = FocusPlanner::build(request, {first, second, third, fourth}, rules);
    QCOMPARE(plan.tracks.size(), 1);
    QCOMPARE(plan.tracks.first().key, second.key);
    request.scene = QStringLiteral("study");
    plan = FocusPlanner::build(request, {first, second, third, fourth}, rules);
    QCOMPARE(plan.tracks.size(), 2);
    QCOMPARE(plan.tracks.first().key, first.key);
}

void FocusCoreTest::reportsUnknownAndInsufficientDuration()
{
    FocusRequest request;
    request.minutes = 5;
    const auto unknown = track(QStringLiteral("unknown"), 0);
    const auto first = track(QStringLiteral("1"), 180000);
    const auto second = track(QStringLiteral("2"), 180000);
    const auto third = track(QStringLiteral("3"), 180000);
    auto plan = FocusPlanner::build(request, {unknown, first, second, third});
    QCOMPARE(plan.tracks.size(), 3);
    QVERIFY(plan.notes.join(QLatin1Char('\n')).contains(QStringLiteral("时长未知")));
    QVERIFY(!plan.notes.join(QLatin1Char('\n')).contains(QStringLiteral("未达到")));
    plan = FocusPlanner::build(request, {unknown, first});
    QCOMPARE(plan.tracks.size(), 2);
    QVERIFY(plan.notes.join(QLatin1Char('\n')).contains(QStringLiteral("未达到")));
    QVERIFY(plan.notes.join(QLatin1Char('\n')).contains(QStringLiteral("循环至播放计时结束")));
}

void FocusCoreTest::persistenceIsScopedAndExplicit()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString connection = QStringLiteral("focus_rules_test");
    qint64 id = 0;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(directory.filePath(QStringLiteral("memory.sqlite")));
        QVERIFY(database.open());
        FocusStore store(database);
        QString error;
        QVERIFY2(store.initialize(&error), qPrintable(error));
        const QString profile = QStringLiteral("local:alice'); DROP TABLE focus_rules;--");
        id = store.addRule(profile, QStringLiteral("work"), QStringLiteral("exclude_artist"),
                           QStringLiteral("歌手'); DELETE FROM focus_rules;--"), &error);
        QVERIFY2(id > 0, qPrintable(error));
        QCOMPARE(store.addRule(profile, QStringLiteral("work"), QStringLiteral("exclude_artist"),
                                QStringLiteral("歌手'); DELETE FROM focus_rules;--"), &error), id);
        QVERIFY(store.rules(QStringLiteral("guest"), QString(), &error).isEmpty());
        QVERIFY(error.isEmpty());
        QVERIFY(store.rules(profile, QStringLiteral("study"), &error).isEmpty());
        QVERIFY(!store.removeRule(QStringLiteral("guest"), id, &error));
        QCOMPARE(store.rules(profile).size(), 1);
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(directory.filePath(QStringLiteral("memory.sqlite")));
        QVERIFY(database.open());
        FocusStore store(database);
        QVERIFY(store.initialize());
        const QString profile = QStringLiteral("local:alice'); DROP TABLE focus_rules;--");
        const auto persisted = store.rules(profile);
        QCOMPARE(persisted.size(), 1);
        QCOMPARE(persisted.first().id, id);
        QVERIFY(store.removeRule(profile, id));
        QVERIFY(store.rules(profile).isEmpty());
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void FocusCoreTest::labelsAreScopedAndDurableWithoutAudioCopies()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString connection = QStringLiteral("focus_labels_test");
    auto original = track(QStringLiteral("file:///a'; DROP TABLE focus_rules;--"), 0);
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(directory.filePath(QStringLiteral("memory.sqlite")));
        QVERIFY(database.open());
        FocusStore store(database);
        QVERIFY(store.initialize());
        QVERIFY(store.setTrackLabels(QStringLiteral("local:alice"), original.key, 1, 0, 234567));
        auto labeled = store.applyLabels(QStringLiteral("local:alice"), {original, original});
        QCOMPARE(labeled.size(), 2);
        QCOMPARE(labeled.first().instrumental, 1);
        QCOMPARE(labeled.last().instrumental, 1);
        QCOMPARE(labeled.first().live, 0);
        QCOMPARE(labeled.first().durationMs, qint64(234567));
        QCOMPARE(labeled.first().url, original.url);
        const auto guest = store.applyLabels(QStringLiteral("guest"), {original});
        QCOMPARE(guest.first().instrumental, -1);
        QCOMPARE(guest.first().durationMs, qint64(0));
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(directory.filePath(QStringLiteral("memory.sqlite")));
        QVERIFY(database.open());
        FocusStore store(database);
        QCOMPARE(store.applyLabels(QStringLiteral("local:alice"), {original}).first().instrumental, 1);
        QVERIFY(store.setTrackLabels(QStringLiteral("local:alice"), original.key, -1, -1));
        QCOMPARE(store.applyLabels(QStringLiteral("local:alice"), {original}).first().instrumental, -1);
        QCOMPARE(store.applyLabels(QStringLiteral("local:alice"), {original}).first().durationMs, qint64(234567));
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void FocusCoreTest::rejectsInvalidStoreInputs()
{
    const QString connection = QStringLiteral("focus_validation_test");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(database.open());
        FocusStore store(database);
        QVERIFY(store.initialize());
        QString error;
        QCOMPARE(store.addRule(QString(), QStringLiteral("work"), QStringLiteral("exclude_live"), {}, &error), qint64(0));
        QVERIFY(!error.isEmpty());
        QCOMPARE(store.addRule(QStringLiteral("guest"), QStringLiteral("invalid"), QStringLiteral("exclude_live")), qint64(0));
        QCOMPARE(store.addRule(QStringLiteral("guest"), QStringLiteral("work"), QStringLiteral("invented_rule")), qint64(0));
        QCOMPARE(store.addRule(QStringLiteral("guest"), QStringLiteral("work"), QStringLiteral("exclude_artist")), qint64(0));
        QVERIFY(!store.setTrackLabels(QStringLiteral("guest"), QStringLiteral("key"), 2, -1));
        QVERIFY(!store.setTrackLabels(QStringLiteral("guest"), QStringLiteral("key"), -1, -2));
        QVERIFY(!store.setTrackLabels(QStringLiteral("guest"), QStringLiteral("key"), -1, -1, -1));
        QVERIFY(store.rules(QStringLiteral("guest")).isEmpty());
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

QTEST_GUILESS_MAIN(FocusCoreTest)
#include "focuscore_test.moc"
