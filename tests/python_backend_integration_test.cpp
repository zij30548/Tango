#include "appbackendclient.h"
#include "conversationclient.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QUuid>
#include <QtTest>
#include <functional>

// 使用现有客户端类直接连接真实 Python 进程，模型和曲库是隔离的测试数据。
class PythonBackendIntegration : public QObject {
    Q_OBJECT
    AppBackendClient backend;
    QString taskId, device = QUuid::createUuid().toString(QUuid::WithoutBraces);
    static QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    static bool wait(const std::function<bool()> &ready, int timeout = 6000) {
        QElapsedTimer timer; timer.start();
        while (!ready() && timer.elapsed() < timeout) QTest::qWait(10);
        return ready();
    }
    QJsonObject fetchTask() {
        bool done = false; QJsonObject task; QString error;
        backend.musicTask(taskId, this, [&](const QJsonObject &body, const QString &failure) {
            task = body.value("task").toObject(); error = failure; done = true;
        });
        if (!wait([&] { return done; }) || !error.isEmpty()) return {};
        return task;
    }
private slots:
    void pythonHealth() {
        QNetworkAccessManager network;
        auto *reply = network.get(QNetworkRequest(QUrl(QString::fromUtf8(qgetenv("MUSICPLAYER_BACKEND_URL")) + "/health")));
        QVERIFY(wait([&] { return reply->isFinished(); }));
        const auto health = QJsonDocument::fromJson(reply->readAll()).object(); reply->deleteLater();
        QCOMPARE(health.value("runtimeLanguage").toString(), QString("python"));
        QCOMPARE(health.value("routingFeaturesVersion").toInt(), 1);
    }
    void nativeAccountAndCapabilities() {
        bool done = false; QString error;
        backend.registerUser("qt-python-" + QString::number(QCoreApplication::applicationPid()), "fixture-private-password", this,
            [&](const QString &failure) { error = failure; done = true; });
        QVERIFY(wait([&] { return done; })); QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(backend.authenticated()); done = false;
        backend.refreshAgentStatus(this, [&](const QString &failure) { error = failure; done = true; });
        QVERIFY(wait([&] { return done; })); QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(backend.agentReady()); QVERIFY(backend.taskRuntimeAvailable());
        QVERIFY(backend.conversationAvailable()); QVERIFY(backend.routingAvailable());
    }
    void nativeConversationRouteAndRestore() {
        ConversationClient conversation(nullptr, &backend);
        QSignalSpy accepted(&conversation, &ConversationClient::messageAccepted);
        QSignalSpy routed(&conversation, &ConversationClient::routeReady);
        QSignalSpy errors(&conversation, &ConversationClient::error);
        conversation.submit("你能做什么");
        QVERIFY(wait([&] { return !accepted.isEmpty() || !errors.isEmpty(); }));
        QVERIFY2(!accepted.isEmpty(), "Python会话未接受原生客户端消息");
        const auto turn = accepted.first().at(0).toString();
        QCOMPARE(accepted.first().at(2).toString(), QString("router"));
        conversation.route(turn, "");
        QVERIFY(wait([&] { return !routed.isEmpty() || !errors.isEmpty(); }));
        QVERIFY2(!routed.isEmpty(), "原生客户端未接受Python路由计划");
        const auto plan = routed.first().at(1).toJsonObject();
        QCOMPARE(plan.value("strategy").toString(), QString("direct"));
        QCOMPARE(plan.value("actions").toArray().first().toObject().value("capability").toString(), QString("assistant.reply"));
        const auto cid = conversation.snapshot().value("id").toString();
        QVERIFY(!cid.isEmpty());
        ConversationClient restored(nullptr, &backend);
        restored.restoreLatest();
        QVERIFY(wait([&] { return restored.snapshot().value("id").toString() == cid; }));
        QCOMPARE(restored.snapshot().value("messages").toArray().size(), 2);
    }
    void nativeTaskToolsPreviewAndAdopt() {
        bool done = false; QString error; QJsonObject task;
        backend.createMusicTask({{"deviceId", device}, {"requestId", id()}, {"instruction", "读书20分钟，不要现场版"}, {"preference", ""}}, this,
            [&](const QJsonObject &body, const QString &failure) { error = failure; task = body.value("task").toObject(); done = true; });
        QVERIFY(wait([&] { return done; })); QVERIFY2(error.isEmpty(), qPrintable(error));
        taskId = task.value("id").toString(); QVERIFY(!taskId.isEmpty());
        QVERIFY(wait([&] { task = fetchTask(); return task.value("status").toString() == "waiting_tools" || task.value("status").toString() == "failed"; }));
        QCOMPARE(task.value("status").toString(), QString("waiting_tools"));
        const auto stamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QJsonArray tracks;
        for (int i = 1; i <= 10; ++i) tracks.append(QJsonObject{{"id", i}, {"title", QString("真实曲目%1").arg(i)}, {"artist", "测试歌手"}, {"album", "测试专辑"}, {"durationMs", 240000}, {"sourceId", 77}, {"sourceKind", "playlist"}, {"sourceName", "读书"}, {"sourceTags", QJsonArray()}});
        const QJsonObject result{{"tracks", tracks}, {"sources", QJsonArray{QJsonObject{{"id", 77}, {"name", "读书"}, {"checkedAt", stamp}, {"tags", QJsonArray()}}}}, {"checkedAt", stamp}};
        for (const auto &entry : task.value("pendingSteps").toArray()) {
            const auto step = entry.toObject(); done = false;
            backend.completeMusicTaskStep(taskId, step.value("id").toString(), {{"deviceId", device}, {"leaseId", step.value("leaseId")}, {"revision", step.value("revision")}, {"result", result}}, this,
                [&](const QJsonObject &, const QString &failure) { error = failure; done = true; });
            QVERIFY(wait([&] { return done; })); QVERIFY2(error.isEmpty(), qPrintable(error));
        }
        QVERIFY(wait([&] { task = fetchTask(); return task.value("status").toString() == "preview" || task.value("status").toString() == "failed"; }));
        QCOMPARE(task.value("status").toString(), QString("preview"));
        QCOMPARE(task.value("preview").toObject().value("totalDurationMs").toInt(), 1200000);
        QVERIFY(!task.contains("userId")); done = false;
        backend.musicTaskAction(taskId, {{"deviceId", device}, {"requestId", id()}, {"expectedRevision", task.value("revision")}, {"action", "adopt"}}, this,
            [&](const QJsonObject &body, const QString &failure) { error = failure; task = body.value("task").toObject(); done = true; });
        QVERIFY(wait([&] { return done; })); QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(task.value("status").toString(), QString("awaiting_apply"));
    }
    void nativeLogout() {
        bool done = false; backend.logout(this, [&](const QString &) { done = true; });
        QVERIFY(wait([&] { return done; })); QVERIFY(!backend.authenticated());
    }
};
QTEST_GUILESS_MAIN(PythonBackendIntegration)
#include "python_backend_integration_test.moc"
