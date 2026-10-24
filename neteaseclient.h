#ifndef NETEASECLIENT_H
#define NETEASECLIENT_H

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QUrlQuery>
#include <functional>

class QNetworkAccessManager;
class QProcess;

// Only the loopback adapter receives the local service token. Music/CDN requests
// never receive this token or the NetEase account cookie.
class NeteaseClient : public QObject
{
    Q_OBJECT
  public:
    using Callback = std::function<void(const QJsonObject &, const QString &)>;
    explicit NeteaseClient(QObject *parent = nullptr);
    void get(const QString &path, const QUrlQuery &query, QObject *context, Callback callback);
    void post(const QString &path, const QJsonObject &body, QObject *context, Callback callback);
    static QString dataDirectory();
    // Pass the account-scoped local database profile key, never a session token.
    // Empty / guest retains the legacy guest music connection.
    static void setAccountProfile(const QString &profileKey);
    static QString accountProfile();
    // Opaque, process-local authorization epoch for metadata cache isolation.
    // Contains no account identity, cookie, or local service token.
    static QString catalogCacheContext();
    static bool isAllowedStreamUrl(const QUrl &url);

  private:
    using ReadyCallback = std::function<void(const QString &)>;
    void ensureReady(ReadyCallback callback);
    void probe(bool mayStartService);
    void finishReady(const QString &error);
    void send(const QString &method, const QString &path, const QUrlQuery &query,
              const QJsonObject &body, QObject *context, Callback callback);
    QString projectDirectory() const;
    QNetworkAccessManager *network;
    QPointer<QProcess> starter;
    QVector<ReadyCallback> waiting;
    bool probing = false;
    bool customPort = false;
    bool profileIsolationSupported = false;
    quint16 port = 16381;
};
#endif
