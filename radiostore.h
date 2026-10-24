#ifndef RADIOSTORE_H
#define RADIOSTORE_H
#include <QSqlDatabase>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
class RadioStore
{
public:
    explicit RadioStore(QSqlDatabase database) : database(database) {}
    QString preference(const QString &profile);
    bool save(const QString &profile, const QString &preference, QString *error = nullptr);
    QJsonArray items(const QString &profile);
    // 本机日志只承接已验证的后台路由；恢复不会授予新的执行或记忆写入权限。
    QJsonObject preferenceSnapshot(const QString &profile, QString *error = nullptr);
    bool journalRoute(const QString &profile, const QString &deviceId, const QString &conversationId,
                      const QString &turnId, const QJsonObject &plan, const QString &preferenceBase,
                      qint64 baseRevision, QString *error = nullptr);
    QJsonArray routes(const QString &profile, const QString &deviceId, const QString &conversationId,
                      QString *error = nullptr);
    bool clearConversationRoutes(const QString &profile, QString *error = nullptr);
    bool setRouteAction(const QString &profile, const QString &deviceId, const QString &conversationId,
                        const QString &turnId, const QString &actionId, const QString &expectedState,
                        const QString &nextState, QString *error = nullptr);
    bool setRouteConfirmationTurn(const QString &profile, const QString &deviceId, const QString &conversationId,
                                  const QString &turnId, const QString &confirmationTurn, QString *error = nullptr);
    bool decidePreference(const QString &profile, const QString &deviceId, const QString &conversationId,
                          const QString &turnId, const QString &actionId, bool confirmed, const QString &edited,
                          QJsonObject *receipt = nullptr, QString *error = nullptr);
private:
    bool initialize();
    QSqlDatabase database;
};
#endif
