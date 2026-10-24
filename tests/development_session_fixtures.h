#ifndef DEVELOPMENT_SESSION_FIXTURES_H
#define DEVELOPMENT_SESSION_FIXTURES_H
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

namespace DevelopmentFixtures {
class Environment {
public:
    ~Environment() { for (auto entry = values.crbegin(); entry != values.crend(); ++entry)
        if (entry->existed) qputenv(entry->name.constData(), entry->value); else qunsetenv(entry->name.constData()); }
    void set(const QByteArray &name, const QByteArray &value) {
        values.append({name, qgetenv(name.constData()), qEnvironmentVariableIsSet(name.constData())});
        qputenv(name.constData(), value);
    }
private:
    struct Value { QByteArray name, value; bool existed; }; QVector<Value> values;
};
class Backend : public QObject {
public:
    struct Request { QString route; QByteArray headers; QJsonObject body; };
    QTcpServer server;
    QVector<Request> requests;
    bool rejectDevelopment = false, omitDevelopment = false, expired = false;
    QJsonValue developmentValue = true;
    QByteArray redirectedTo;
    int delay = 0;
    const QByteArray capability = QByteArray(64, 'a');
    Backend() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                auto bytes = std::make_shared<QByteArray>(); auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes, handled] {
                    bytes->append(socket->readAll()); if (*handled) return;
                    const int end = bytes->indexOf("\r\n\r\n"); if (end < 0) return;
                    const auto headers = bytes->left(end); int length = 0;
                    for (const auto &line : headers.split('\n')) if (line.toLower().startsWith("content-length:")) length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    if (bytes->size() < end + 4 + length) return; *handled = true;
                    const QString route = QString::fromLatin1(headers.split(' ').value(1));
                    requests.append({route, headers, QJsonDocument::fromJson(bytes->mid(end + 4, length)).object()});
                    int status = 200; QJsonObject body;
                    if (route == "/v1/dev/session") {
                        if (!redirectedTo.isEmpty()) status = 302;
                        else if (rejectDevelopment) { status = 403; body = {{"error", QJsonObject{{"message", QString::fromLatin1(capability)}}}}; }
                        else {
                            body = session("development-fixture-user", "本机开发测试");
                            if (!omitDevelopment) body.insert("development", developmentValue);
                        }
                    } else if (route == "/v1/auth/login") body = session("owner-fixture-user", "owner-fixture");
                    else if (route == "/v1/agent/status") body = {{"configured", true}, {"model", "offline-fixture"}};
                    else if (route == "/v1/account/usage") body = {{"daily", QJsonObject{{"limit",30},{"used",0},{"remaining",30},{"resetAt","2030-01-01T00:00:00Z"}}}, {"total",QJsonObject{{"limit",300},{"used",0},{"remaining",300}}},{"disabled",false}};
                    else if (route == "/v1/auth/me") body = {{"user", QJsonObject{{"id","development-fixture-user"},{"username","本机开发测试"}}},{"expiresAt",QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODate)}};
                    else if (route == "/v1/auth/logout") body = {{"ok",true}};
                    else { status = 404; body = {{"error",QJsonObject{{"message","unexpected fixture route"}}}}; }
                    const QPointer<QTcpSocket> guard(socket); const auto location = redirectedTo;
                    QTimer::singleShot(delay, this, [guard, body, status, location] {
                        if (!guard || guard->state() != QAbstractSocket::ConnectedState) return;
                        const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
                        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " fixture\r\nContent-Type: application/json\r\nConnection: close\r\n";
                        if (status == 302) response += "Location: " + location + "\r\n";
                        response += "Content-Length: " + QByteArray::number(payload.size()) + "\r\n\r\n" + payload;
                        guard->write(response); guard->disconnectFromHost();
                    });
                });
            }
        });
    }
    QJsonObject session(const QString &id, const QString &name) const {
        return {{"token",QString(43,QLatin1Char('s'))},{"user",QJsonObject{{"id",id},{"username",name}}},
            {"expiresAt",QDateTime::currentDateTimeUtc().addSecs(expired ? -1 : 3600).toString(Qt::ISODate)}};
    }
    QByteArray url() const { return "http://127.0.0.1:" + QByteArray::number(server.serverPort()); }
    int count(const QString &route) const { int count = 0; for (const auto &request : requests) count += request.route == route; return count; }
};
class Fixture {
public:
    Environment environment;
    QTemporaryDir directory;
    Backend backend;
    QString tokenPath;
    Fixture() {
        tokenPath = directory.filePath("development-token");
        writeToken(backend.capability + '\n');
        environment.set("MUSICPLAYER_BACKEND_URL", backend.url());
        environment.set("MUSICPLAYER_DEVELOPMENT_MODE", "1");
        environment.set("MUSICPLAYER_DEVELOPMENT_TOKEN_FILE", tokenPath.toUtf8());
        environment.set("MUSICPLAYER_DATA_DIR", directory.path().toUtf8());
        environment.set("MUSICPLAYER_NETEASE_PORT", "1");
        environment.set("MUSICPLAYER_REDIS_PORT", "invalid-test-port");
    }
    bool writeToken(const QByteArray &bytes) {
        QFile file(tokenPath); if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) return false;
        return file.write(bytes) == bytes.size();
    }
};
}
#endif
