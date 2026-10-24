#ifndef MUSICTASKCLIENT_H
#define MUSICTASKCLIENT_H

#include <QJsonObject>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVector>

class AppBackendClient;
class NeteaseClient;
class QTimer;

// Account-owned, durable task session. Its lifetime must be the main window,
// rather than the assistant page: hiding a page never cancels a task.
class MusicTaskClient : public QObject
{
    Q_OBJECT
public:
    explicit MusicTaskClient(QObject *parent = nullptr, AppBackendClient *backend = nullptr);
    ~MusicTaskClient() override;
    QJsonObject snapshot() const;
    QString deviceId() const;
    bool busy() const;
    void restoreLatest();
    void restore(const QString &taskId);
    // 原路由事实必须与回执逐项一致；selectTask=false 不改变当前任务或运行中的工具。
    void observeRequest(const QString &requestId, const QJsonObject &expectedFacts, bool selectTask = true);
    // 采用恢复须核对原动作和版本；只刷新当前同一任务，不抢占其他任务或执行本机工具。
    void observeAdoptionRequest(const QString &requestId, const QString &taskId, int expectedRevision);
    // Creates when detached; otherwise amends the current durable task.
    void submit(const QString &instruction);
    void submit(const QString &instruction, const QString &preference,
                const QVector<qint64> &excludedIds = {}, const QString &messageId = QString());
    void action(const QString &name, const QString &instruction = QString(),
                const QJsonObject &details = QJsonObject(), const QString &messageId = QString());
    // 原采用标识由本机先持久保存；这条入口只供明确的当前预览采用操作使用。
    void adopt(const QString &requestId);
    // Cancels local requests and clears visible state. Does not alter persisted
    // server state, nor automatically replay/adopt a playlist after login.
    void detach();
signals:
    void snapshotChanged(const QJsonObject &snapshot);
    void error(const QString &message);
    void busyChanged(bool busy);
    void instructionAccepted(QString instruction);
    void requestSettled(QString requestId, bool accepted, QString error);
    // 仅完整HTTP回执且可证实的提交前拒绝触发；未知和幂等冲突不能借此终结原操作。
    void requestRejected(QString requestId, QString error);
private:
    void poll();
    bool acceptSnapshot(const QJsonObject &response, quint64 generation);
    void runPending();
    void sendStepResult(const QString &key, const QJsonObject &step, const QJsonObject &body,
                        QObject *context, quint64 generation);
    void setBusy(bool value);
    void failed(const QString &message, quint64 generation);
    void rejected(const QString &requestId, const QString &message, quint64 generation);
    void cancelRequests();
    void dispatchAction(const QString &name, const QString &instruction, const QJsonObject &details,
                        const QString &messageId, bool explicitAdoption);
    void ensureEvents();
    void stopEvents();
    void receiveProgress(const QJsonObject &progress, quint64 generation);
    void scheduleProgressCatchup();
    AppBackendClient *backend;
    NeteaseClient *music;
    QTimer *timer;
    QTimer *eventReconnectTimer;
    QTimer *progressRefreshTimer;
    QPointer<QObject> eventContext;
    QPointer<QObject> operation;
    QHash<QString, QPointer<QObject>> tools;
    QSet<QString> completedSteps;
    QHash<QString, QJsonObject> resultOutbox;
    QHash<QString, int> resultAttempts;
    QJsonObject state;
    // Device-local observation health; never replaces the durable task status.
    QJsonObject connectionState;
    QJsonObject observedProgress;
    QString installationId;
    QString selectedTaskId;
    quint64 generation = 0;
    bool requestBusy = false;
    bool discoverLatest = false;
    int consecutiveErrors = 0;
    int eventFailures = 0;
    int progressCatchupAttempts = 0;
    bool eventsUnsupported = false;
    bool eventsHealthy = false;
    // Conversation restoration only observes persisted state. A user action
    // explicitly enables leased local tools; subsequent polling cannot do so.
    bool observationOnly = false;
};

#endif
