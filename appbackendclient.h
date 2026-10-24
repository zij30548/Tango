#ifndef APPBACKENDCLIENT_H
#define APPBACKENDCLIENT_H

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Only the application backend is addressed here. Provider credentials remain
// on the server; the application's short-lived session is retained in memory.
class AppBackendClient : public QObject
{
    Q_OBJECT
  public:
    using Callback = std::function<void(const QString &error)>;
    using JsonCallback = std::function<void(const QJsonObject &, const QString &error)>;
    using StreamClosedCallback = std::function<void(int httpStatus, const QString &error)>;
    explicit AppBackendClient(QObject *parent = nullptr, int timeoutMs = 15000);
    static AppBackendClient *instance();
    static bool enabled();
    static QString configurationError();
    static bool developmentMode();
    void bootstrapDevelopmentSession(QObject *context, Callback callback);
    bool authenticated() const;
    QString userId() const;
    QString username() const;
    QString serviceIdentity() const;
    bool agentReady() const;
    QString modelName() const;
    bool taskRuntimeAvailable() const;
    bool conversationAvailable() const;
    bool routingAvailable() const;
    void registerUser(const QString &username, const QString &password, QObject *context,
                      Callback callback);
    void registerUser(const QString &username, const QString &password, const QString &inviteCode,
                      QObject *context, Callback callback);
    QNetworkReply *registrationConfig(QObject *context, JsonCallback callback);
    QNetworkReply *accountUsage(QObject *context, JsonCallback callback);
    void login(const QString &username, const QString &password, QObject *context,
               Callback callback);
    void logout(QObject *context, Callback callback = Callback());
    void me(QObject *context, Callback callback = Callback());
    void refreshAgentStatus(QObject *context, Callback callback = Callback());
    QNetworkReply *parseFocus(const QJsonObject &body, QObject *context, JsonCallback callback,
                              int timeoutMs = 15000);
    QNetworkReply *planRadio(const QJsonObject &body, QObject *context, JsonCallback callback);
    QNetworkReply *planAssistant(const QJsonObject &body, QObject *context, JsonCallback callback);
    QNetworkReply *selectRadio(const QJsonObject &body, QObject *context, JsonCallback callback);
    QNetworkReply *createMusicTask(const QJsonObject &body, QObject *context, JsonCallback callback);
    QNetworkReply *latestMusicTask(const QString &deviceId, QObject *context, JsonCallback callback);
    QNetworkReply *musicTask(const QString &taskId, QObject *context, JsonCallback callback);
    // 只查询原操作记录，不重发任务或创建新的模型尝试。
    QNetworkReply *musicTaskRequest(const QString &deviceId, const QString &requestId,
                                   QObject *context, JsonCallback callback);
    // A separate, bounded SSE observation channel. It never submits a task or
    // receives provider credentials; deleting context aborts it immediately.
    QNetworkReply *musicTaskEvents(const QString &taskId, qint64 lastStateVersion,
                                  QObject *context, JsonCallback progress,
                                  StreamClosedCallback closed, int idleTimeoutMs = 45000);
    QNetworkReply *musicTaskAction(const QString &taskId, const QJsonObject &body,
                                  QObject *context, JsonCallback callback);
    QNetworkReply *completeMusicTaskStep(const QString &taskId, const QString &stepId,
                                        const QJsonObject &body, QObject *context,
                                        JsonCallback callback);
    QNetworkReply *createConversation(const QJsonObject &body, QObject *context, JsonCallback callback);
    QNetworkReply *latestConversation(const QString &deviceId, QObject *context, JsonCallback callback);
    QNetworkReply *conversation(const QString &id, QObject *context, JsonCallback callback);
    QNetworkReply *appendConversationMessage(const QString &id, const QJsonObject &body,
                                            QObject *context, JsonCallback callback);
    QNetworkReply *conversationMessage(const QString &id, const QString &messageId,
                                      QObject *context, JsonCallback callback);
    QNetworkReply *routeConversationMessage(const QString &id, const QString &messageId,
                                           const QJsonObject &body, QObject *context, JsonCallback callback);
    QNetworkReply *conversationMessageRoute(const QString &id, const QString &messageId,
                                           QObject *context, JsonCallback callback);
    QNetworkReply *appendConversationEvent(const QString &id, const QJsonObject &body,
                                          QObject *context, JsonCallback callback);
    QNetworkReply *conversationBefore(const QString &id, qint64 sequence,
                                     QObject *context, JsonCallback callback);
    QNetworkReply *clearConversations(const QJsonObject &body, QObject *context, JsonCallback callback);


  signals:
    void authChanged(const QString &userId, const QString &username);
    void agentStatusChanged(bool ready, const QString &model);

  private:
    void synchronizeConfiguration();
    void clearSession();
    void setAgentStatus(bool ready, const QString &model = QString());
    void authenticate(const QString &route, const QString &username, const QString &password,
                      QObject *context, Callback callback, const QJsonObject &extra = QJsonObject());
    QNetworkReply *request(const QString &method, const QString &route, const QJsonObject &body,
                           bool authorize, QObject *context, JsonCallback callback,
                           int timeoutMs = 15000, const QByteArray &developmentHeader = QByteArray());
    QNetworkAccessManager *network;
    QTimer *expiryTimer;
    QUrl baseUrl;
    QString configError;
    QByteArray sessionToken;
    QString currentUserId, currentUsername;
    QDateTime expiresAt;
    bool ready = false;
    bool developmentSessionActive = false;
    bool configuredDevelopmentMode = false;
    QString configuredDevelopmentTokenFile;
    bool tasksAvailable = false;
    bool conversationsAvailable = false;
    bool routesAvailable = false;
    QString currentModel;
    quint64 authRevision = 0;
    quint64 authAttemptRevision = 0;
    int defaultTimeoutMs;
};

#endif
