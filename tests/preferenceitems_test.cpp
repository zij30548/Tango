#include "radiostore.h"
#include <QJsonObject>
#include <QSqlQuery>
#include <QTest>
#include <QUuid>
#include <QTemporaryDir>

namespace {
struct JournalFixture {
    QString connection = QUuid::createUuid().toString();
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
    explicit JournalFixture(const QString &path = ":memory:") { db.setDatabaseName(path); db.open(); }
    ~JournalFixture() { db.close(); db = {}; QSqlDatabase::removeDatabase(connection); }
};
QJsonObject journalPlan(const QString &turn = "turn", bool dependent = true)
{
    const QJsonObject memory{{"id", "memory"}, {"capability", "memory.propose"}, {"source", "记住爵士然后找歌"},
        {"parameters", QJsonObject{{"operation", "set"}, {"preference", "喜欢爵士"}}}, {"dependsOn", QJsonArray()}};
    const QJsonObject music{{"id", "music"}, {"capability", "music.task"}, {"source", "记住爵士然后找歌"},
        {"parameters", QJsonObject{{"mode", "new"}}}, {"dependsOn", dependent ? QJsonArray{"memory"} : QJsonArray()}};
    return {{"version", 1}, {"messageId", turn}, {"planner", "model"}, {"strategy", "single_agent"},
        {"intents", QJsonArray{"preference_proposal", "scene_research"}}, {"actions", QJsonArray{memory, music}},
        {"missingSlots", QJsonArray()}, {"slots", QJsonObject{{"artist", ""}, {"targetMinutes", 0}}}};
}
bool awaitPreference(RadioStore &store, const QString &turn = "turn")
{
    const auto base = store.preferenceSnapshot("alice");
    return store.journalRoute("alice", "device", "conversation", turn, journalPlan(turn), base.value("preference").toString(),
        qint64(base.value("revision").toDouble(-1))) &&
        store.setRouteAction("alice", "device", "conversation", turn, "memory", "pending", "awaiting_confirmation") &&
        store.setRouteConfirmationTurn("alice", "device", "conversation", turn, "confirmation");
}
}

class PreferenceItemsTest : public QObject
{
    Q_OBJECT
private slots:
    void structuredEntriesAndPartialDeletion()
    {
        const QString connection = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(":memory:"); QVERIFY(db.open());
            RadioStore store(db); QString error;
            QVERIFY(store.save("alice", QStringLiteral("喜欢华语 R&B；少推荐摇滚；喜欢华语 R&B"), &error));
            const auto original = store.items("alice"); QCOMPARE(original.size(), 2);
            const auto retained = original.first().toObject();
            QCOMPARE(retained["scope"].toString(), QString("global"));
            QCOMPARE(retained["source"].toString(), QString("user_confirmed_editor"));
            QVERIFY(!retained["confirmedAt"].toString().isEmpty());
            QVERIFY(store.save("bob", QStringLiteral("喜欢爵士"), &error));
            QVERIFY(store.save("alice", QStringLiteral("喜欢华语 R&B"), &error));
            const auto current = store.items("alice"); QCOMPARE(current.size(), 1);
            QCOMPARE(current.first().toObject()["id"], retained["id"]);
            QCOMPARE(current.first().toObject()["confirmedAt"], retained["confirmedAt"]);
            QCOMPARE(current.first().toObject()["revision"].toInt(), 2);
            QCOMPARE(store.preference("bob"), QStringLiteral("喜欢爵士"));
            QCOMPARE(store.items("bob").size(), 1);
            QVERIFY(store.save("alice", {}, &error));
            QVERIFY(store.preference("alice").isEmpty()); QVERIFY(store.items("alice").isEmpty());
            QCOMPARE(store.items("bob").size(), 1);
        }
        QSqlDatabase::removeDatabase(connection);
    }
    void failedItemWriteRollsBackSummaryAndRevision()
    {
        const QString connection = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(":memory:"); QVERIFY(db.open());
            RadioStore store(db); QString error;
            QVERIFY(store.save("alice", QStringLiteral("喜欢爵士"), &error));
            const auto before = store.items("alice");
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER fail_item BEFORE INSERT ON radio_preference_items BEGIN SELECT RAISE(ABORT,'fixture'); END"));
            QVERIFY(!store.save("alice", QStringLiteral("改为摇滚"), &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(store.preference("alice"), QStringLiteral("喜欢爵士"));
            QCOMPARE(store.items("alice"), before);
            QVERIFY(query.exec("DROP TRIGGER fail_item"));
            QVERIFY(store.save("alice", QStringLiteral("喜欢爵士；少人声"), &error));
            QCOMPARE(store.items("alice").first().toObject()["revision"].toInt(), 2);
        }
        QSqlDatabase::removeDatabase(connection);
    }
    void legacySummaryRequiresConfirmationToCreateItems()
    {
        const QString connection = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(":memory:"); QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TABLE radio_preferences(profile TEXT PRIMARY KEY,preference TEXT NOT NULL)"));
            QVERIFY(query.exec("INSERT INTO radio_preferences VALUES('alice','legacy jazz')"));
            RadioStore store(db);
            QCOMPARE(store.preference("alice"), QString("legacy jazz"));
            QVERIFY(store.items("alice").isEmpty());
            QVERIFY(store.save("alice", store.preference("alice")));
            QCOMPARE(store.items("alice").size(), 1);
            QVERIFY(!store.save("", "bad"));
            QVERIFY(!store.save("alice", QString(501, QLatin1Char('x'))));
            QCOMPARE(store.preference("alice"), QString("legacy jazz"));
        }
        QSqlDatabase::removeDatabase(connection);
    }
    void pendingJournalSurvivesConnectionRestartWithoutConfirmation()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("owned.sqlite");
        {
            JournalFixture fixture(path); QVERIFY(fixture.db.isOpen()); RadioStore store(fixture.db);
            QVERIFY(awaitPreference(store));
        }
        {
            JournalFixture fixture(path); RadioStore store(fixture.db); QString error;
            const auto restored = store.routes("alice", "device", "conversation", &error);
            QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(restored.size(), 1);
            const auto row = restored.first().toObject();
            QCOMPARE(row.value("actions").toObject().value("memory").toString(), QString("awaiting_confirmation"));
            QCOMPARE(row.value("actions").toObject().value("music").toString(), QString("pending"));
            QCOMPARE(row.value("confirmationTurn").toString(), QString("confirmation"));
            QVERIFY(row.value("receipts").toObject().isEmpty());
            QCOMPARE(store.preferenceSnapshot("alice").value("revision").toInt(), 0);
            QVERIFY(store.preference("alice").isEmpty());
        }
    }
    void confirmationReceiptAndPreferenceAreCommittedOnce()
    {
        JournalFixture fixture; RadioStore store(fixture.db); QVERIFY(awaitPreference(store));
        QJsonObject receipt; QString error;
        QVERIFY2(store.decidePreference("alice", "device", "conversation", "turn", "memory", true, "喜欢器乐爵士", &receipt, &error), qPrintable(error));
        QCOMPARE(store.preference("alice"), QString("喜欢器乐爵士"));
        QCOMPARE(receipt.value("preferenceRevision").toInt(), 1);
        const auto items = store.items("alice");
        QJsonObject repeated;
        QVERIFY(store.decidePreference("alice", "device", "conversation", "turn", "memory", true, "喜欢器乐爵士", &repeated));
        QCOMPARE(repeated, receipt); QCOMPARE(store.items("alice"), items);
        QCOMPARE(store.preferenceSnapshot("alice").value("revision").toInt(), 1);
        QVERIFY(!store.decidePreference("alice", "device", "conversation", "turn", "memory", true, "偷偷改成摇滚"));
        QVERIFY(!store.decidePreference("alice", "device", "conversation", "turn", "memory", false, ""));
        QCOMPARE(store.preference("alice"), QString("喜欢器乐爵士"));
    }
    void rejectedPreferenceKeepsVersionAndRecordsDecision()
    {
        JournalFixture fixture; RadioStore store(fixture.db); QVERIFY(store.save("alice", "喜欢粤语"));
        QVERIFY(awaitPreference(store)); QJsonObject receipt;
        QVERIFY(store.decidePreference("alice", "device", "conversation", "turn", "memory", false, "", &receipt));
        QVERIFY(!receipt.value("confirmed").toBool());
        QCOMPARE(store.preference("alice"), QString("喜欢粤语"));
        QCOMPARE(store.preferenceSnapshot("alice").value("revision").toInt(), 1);
        QJsonObject repeated;
        QVERIFY(store.decidePreference("alice", "device", "conversation", "turn", "memory", false, "", &repeated));
        QCOMPARE(receipt, repeated);
        QCOMPARE(store.routes("alice", "device", "conversation").first().toObject().value("actions").toObject().value("memory").toString(), QString("cancelled"));
    }
    void confirmationFailureRollsBackPreferenceAndReceiptTogether()
    {
        JournalFixture fixture; RadioStore store(fixture.db); QVERIFY(awaitPreference(store)); QSqlQuery query(fixture.db);
        QVERIFY(query.exec("CREATE TRIGGER fail_journal BEFORE UPDATE ON radio_route_journal BEGIN SELECT RAISE(ABORT,'owned fixture'); END"));
        QString error; QJsonObject receipt;
        QVERIFY(!store.decidePreference("alice", "device", "conversation", "turn", "memory", true, "喜欢爵士", &receipt, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(receipt.isEmpty()); QVERIFY(store.preference("alice").isEmpty());
        QVERIFY(store.items("alice").isEmpty()); QCOMPARE(store.preferenceSnapshot("alice").value("revision").toInt(), 0);
        QVERIFY(query.exec("DROP TRIGGER fail_journal"));
        const auto row = store.routes("alice", "device", "conversation").first().toObject();
        QCOMPARE(row.value("actions").toObject().value("memory").toString(), QString("awaiting_confirmation"));
        QVERIFY(row.value("receipts").toObject().isEmpty());
        QVERIFY(store.decidePreference("alice", "device", "conversation", "turn", "memory", true, "喜欢爵士", &receipt));
        QCOMPARE(receipt.value("preferenceRevision").toInt(), 1);
    }
    void changedAndRestoredSummaryStillInvalidatesOldProposal()
    {
        JournalFixture fixture; RadioStore store(fixture.db); QVERIFY(store.save("alice", "喜欢爵士"));
        QVERIFY(awaitPreference(store)); QVERIFY(store.save("alice", "喜欢摇滚")); QVERIFY(store.save("alice", "喜欢爵士"));
        QString error;
        QVERIFY(!store.decidePreference("alice", "device", "conversation", "turn", "memory", true, "喜欢人声爵士", nullptr, &error));
        QVERIFY(error.contains("未用旧摘要覆盖")); QCOMPARE(store.preference("alice"), QString("喜欢爵士"));
        QCOMPARE(store.preferenceSnapshot("alice").value("revision").toInt(), 3);
        QVERIFY(store.routes("alice", "device", "conversation").first().toObject().value("receipts").toObject().isEmpty());
    }
    void accountDeviceAndConversationCannotBorrowProposal()
    {
        JournalFixture fixture; RadioStore store(fixture.db); QVERIFY(awaitPreference(store));
        QVERIFY(store.routes("bob", "device", "conversation").isEmpty());
        QVERIFY(store.routes("alice", "other-device", "conversation").isEmpty());
        QVERIFY(store.routes("alice", "device", "other-conversation").isEmpty());
        QVERIFY(!store.decidePreference("bob", "device", "conversation", "turn", "memory", true, "喜欢爵士"));
        QVERIFY(!store.decidePreference("alice", "other-device", "conversation", "turn", "memory", true, "喜欢爵士"));
        QVERIFY(!store.decidePreference("alice", "device", "other-conversation", "turn", "memory", true, "喜欢爵士"));
        QVERIFY(store.preference("alice").isEmpty()); QVERIFY(store.preference("bob").isEmpty());
    }
    void replayCannotReplacePlanBaseOrCompletedState()
    {
        JournalFixture fixture; RadioStore store(fixture.db); QVERIFY(awaitPreference(store));
        QVERIFY(store.journalRoute("alice", "device", "conversation", "turn", journalPlan(), "", 0));
        QVERIFY(!store.journalRoute("alice", "device", "conversation", "turn", journalPlan("turn", false), "", 0));
        QVERIFY(!store.journalRoute("alice", "device", "conversation", "turn", journalPlan(), "changed", 1));
        QVERIFY(!store.setRouteAction("alice", "device", "conversation", "turn", "memory", "pending", "awaiting_confirmation"));
        QVERIFY(!store.setRouteAction("alice", "device", "conversation", "turn", "memory", "awaiting_confirmation", "completed"));
        QVERIFY(store.decidePreference("alice", "device", "conversation", "turn", "memory", true, "喜欢爵士"));
        QVERIFY(store.journalRoute("alice", "device", "conversation", "turn", journalPlan(), "", 0));
        QCOMPARE(store.routes("alice", "device", "conversation").first().toObject().value("actions").toObject().value("memory").toString(), QString("completed"));
    }
    void dispatchedRecoveryBecomesUnknownAndCannotReplay()
    {
        JournalFixture fixture; RadioStore store(fixture.db); QVERIFY(awaitPreference(store));
        QVERIFY(!store.setRouteAction("alice", "device", "conversation", "turn", "music", "pending", "dispatched"));
        QVERIFY(store.decidePreference("alice", "device", "conversation", "turn", "memory", true, "喜欢爵士"));
        QVERIFY(store.setRouteAction("alice", "device", "conversation", "turn", "music", "pending", "dispatched"));
        const auto restored = store.routes("alice", "device", "conversation").first().toObject();
        QCOMPARE(restored.value("actions").toObject().value("music").toString(), QString("unknown"));
        QVERIFY(!store.setRouteAction("alice", "device", "conversation", "turn", "music", "pending", "dispatched"));
        QVERIFY(!store.setRouteAction("alice", "device", "conversation", "turn", "music", "unknown", "dispatched"));
        QVERIFY(store.setRouteAction("alice", "device", "conversation", "turn", "music", "unknown", "completed"));
    }
    void invalidPayloadCorruptionAndExpiryFailClosed()
    {
        JournalFixture fixture; RadioStore store(fixture.db); auto plan = journalPlan();
        auto actions = plan.value("actions").toArray(); auto action = actions.first().toObject();
        action.insert("parameters", QJsonObject{{"operation", "set"}, {"preference", "https://private.invalid/key"}});
        actions[0] = action; plan.insert("actions", actions);
        QVERIFY(!store.journalRoute("alice", "device", "conversation", "turn", plan, "", 0));
        QVERIFY(awaitPreference(store)); QSqlQuery query(fixture.db);
        QVERIFY(query.exec("UPDATE radio_route_journal SET plan_digest='tampered'")); QString error;
        QVERIFY(store.routes("alice", "device", "conversation", &error).isEmpty()); QVERIFY(!error.isEmpty());
        QVERIFY(!store.decidePreference("alice", "device", "conversation", "turn", "memory", true, "喜欢爵士"));
        QVERIFY(query.exec("DELETE FROM radio_route_journal")); QVERIFY(awaitPreference(store));
        QVERIFY(query.exec("UPDATE radio_route_journal SET created_at=1,expires_at=2"));
        QVERIFY(store.routes("alice", "device", "conversation").isEmpty());
        QVERIFY(!store.decidePreference("alice", "device", "conversation", "turn", "memory", true, "喜欢爵士"));
        QVERIFY(store.preference("alice").isEmpty());
    }
    void boundedPendingRoutesRejectOverflowWithoutEvictingConfirmation()
    {
        JournalFixture fixture; RadioStore store(fixture.db);
        for (int i = 0; i < 32; ++i) QVERIFY(awaitPreference(store, QString("turn-%1").arg(i)));
        QVERIFY(!awaitPreference(store, "overflow"));
        const auto restored = store.routes("alice", "device", "conversation"); QCOMPARE(restored.size(), 32);
        QVERIFY(store.decidePreference("alice", "device", "conversation", "turn-0", "memory", false, ""));
        QVERIFY(store.setRouteAction("alice", "device", "conversation", "turn-0", "music", "pending", "skipped"));
        QVERIFY(awaitPreference(store, "after-completed"));
        QCOMPARE(store.routes("alice", "device", "conversation").size(), 32);
    }
};
QTEST_GUILESS_MAIN(PreferenceItemsTest)
#include "preferenceitems_test.moc"
