#ifndef REDISCONNECTION_H
#define REDISCONNECTION_H

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <hiredis/hiredis.h>
#include <memory>
#ifndef Q_OS_WIN
#include <sys/time.h>
#endif

using RedisContextPtr = std::unique_ptr<redisContext, decltype(&redisFree)>;
using RedisReplyPtr = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

// The desktop launcher supplies a dedicated local Redis instance by default.
inline redisContext *connectLocalRedis(QString *error = nullptr)
{
    if (error) error->clear();

    QByteArray host = qgetenv("MUSICPLAYER_REDIS_HOST").trimmed();
    if (host.isEmpty()) host = "127.0.0.1";

    const QByteArray configuredPort = qgetenv("MUSICPLAYER_REDIS_PORT").trimmed();
    bool validPort = true;
    const int port = configuredPort.isEmpty() ? 16379 : configuredPort.toInt(&validPort);
    if (!validPort || port < 1 || port > 65535) {
        if (error) *error = QStringLiteral("Redis 端口配置无效，请检查启动配置。");
        return nullptr;
    }

    const timeval timeout = {2, 0};
    RedisContextPtr context(redisConnectWithTimeout(host.constData(), port, timeout), &redisFree);
    if (!context || context->err) {
        if (error) *error = QStringLiteral("无法连接账户服务，请使用项目启动脚本启动本机 Redis。");
        return nullptr;
    }
    if (redisSetTimeout(context.get(), timeout) != REDIS_OK) {
        if (error) *error = QStringLiteral("无法设置账户服务超时，请重新启动程序。");
        return nullptr;
    }

    const QByteArray password = qgetenv("MUSICPLAYER_REDIS_PASSWORD");
    if (!password.isEmpty()) {
        RedisReplyPtr reply(static_cast<redisReply *>(redisCommand(
                                context.get(), "AUTH %b", password.constData(),
                                static_cast<size_t>(password.size()))), &freeReplyObject);
        if (!reply || reply->type != REDIS_REPLY_STATUS || !reply->str
            || QByteArray(reply->str, static_cast<int>(reply->len)) != "OK") {
            if (error) *error = QStringLiteral("账户服务身份验证失败，请检查启动配置。");
            return nullptr;
        }
    }

    return context.release();
}

#endif // REDISCONNECTION_H
