#ifndef CONVERSATIONCLIENT_H
#define CONVERSATIONCLIENT_H

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

class AppBackendClient;

// An account-owned journal, independent of the current music task or page.
// A durable message receipt permits execution; restoring history never does.
class ConversationClient : public QObject
{
    Q_OBJECT
public:
    explicit ConversationClient(QObject *parent = nullptr, AppBackendClient *backend = nullptr);
    ~ConversationClient() override;
    QJsonObject snapshot() const;
    QString deviceId() const;
    bool busy() const;
    bool available() const;
    void restoreLatest();
    void submit(const QString &instruction);
    // Only a freshly accepted router receipt authorizes this POST. History
    // restoration and subsequent uncertainty recovery are observation-only.
    void route(const QString &messageId, const QString &preference, const QJsonObject &lookupContext = {});
    void observeRoute(const QString &messageId);
    // 仅未取得权威终态的原请求可继续GET观察，已知拒绝不能显示成未知。
    bool routeNeedsObservation(const QString &messageId) const;
    // 只读核验本机续接日志对应的原路由，不授予新的 POST 或模型调用权限。
    void restoreRoute(const QString &messageId);
    void appendLocalReply(const QString &turnId, const QString &text);
    void syncTask(const QString &turnId, const QString &taskId, int revision, qint64 stateVersion = 0);
    void loadEarlier();
    void startNew();
    void clearAll();
    bool clearing() const;
    void detach();
signals:
    void snapshotChanged(const QJsonObject &snapshot);
    void messageAccepted(const QString &messageId, const QString &instruction, const QString &dispatchHint);
    void submissionFailed(const QString &instruction, const QString &error);
    void error(const QString &message);
    void busyChanged(bool busy);
    void routeReady(const QString &messageId, const QJsonObject &plan);
    void routeRestored(const QString &messageId, const QJsonObject &plan);
    void routeFailed(const QString &messageId, const QString &error);
    void clearStarted();
    void conversationsCleared(int conversationCount);
    void clearFailed(const QString &message);
private:
    void setBusy(bool value);
    bool acceptSnapshot(const QJsonObject &response, bool earlier = false);
    void postPending(quint64 epoch);
    void queryPending(quint64 epoch, const QString &failure);
    bool acceptReceipt(const QJsonObject &response);
    void submissionError(const QString &failure);
    void enqueueEvent(const QJsonObject &event);
    void flushEvents();
    void resetOperations();
    bool current(quint64 epoch) const;
    QString clearRequestPath() const;
    bool loadClearRequest();
    bool saveClearRequest();
    void receiveRoute(const QString &messageId, quint64 epoch, const QJsonObject &response,
                      const QString &failure, bool originalSubmission = false);
    void queryRoute(const QString &messageId, quint64 epoch);
    AppBackendClient *backend;
    QJsonObject state;
    QString installationId, pendingId, pendingInstruction, createId;
    QString accountIdentity;
    QPointer<QObject> operation, eventOperation;
    QPointer<QObject> routeOperation;
    QSet<QString> routerAccepted, routesStarted, routesDelivered, routesFinished;
    QString routingMessage;
    QJsonObject routingLookupContext;
    int routeObservations = 0;
    bool restoringRoute = false;
    QHash<QString, QJsonObject> outbox;
    QStringList eventOrder;
    QSet<QString> accepted;
    QSet<QString> completedEvents;
    quint64 generation = 0, operationEpoch = 0;
    bool requestBusy = false;
    bool clearingConversations = false;
    QString clearRequestId, clearPersistenceError;
    int eventFailures = 0;
};
#endif
