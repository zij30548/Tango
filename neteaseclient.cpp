#include "neteaseclient.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

namespace
{
constexpr qint64 ResponseLimit = 4 * 1024 * 1024;
QString activeMusicProfile = QStringLiteral("guest");
quint64 musicProfileGeneration = 0;
QString catalogContext = QUuid::createUuid().toString(QUuid::WithoutBraces);
void boundReply(QNetworkReply *reply, int timeout)
{
    reply->setReadBufferSize(ResponseLimit + 1);
    QObject::connect(reply, &QNetworkReply::readyRead, reply,
                     [reply]()
                     {
                         if (reply->bytesAvailable() > ResponseLimit)
                         {
                             reply->setProperty("responseTooLarge", true);
                             reply->abort();
                         }
                     });
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                     [reply](qint64 received, qint64 total)
                     {
                         if (received > ResponseLimit || total > ResponseLimit)
                         {
                             reply->setProperty("responseTooLarge", true);
                             reply->abort();
                         }
                     });
    QTimer::singleShot(timeout, reply,
                       [reply]()
                       {
                           if (reply->isRunning())
                           {
                               reply->setProperty("timedOut", true);
                               reply->abort();
                           }
                       });
}
} // namespace

NeteaseClient::NeteaseClient(QObject *parent)
    : QObject(parent), network(new QNetworkAccessManager(this))
{
    bool valid = false;
    const int configured = qEnvironmentVariableIntValue("MUSICPLAYER_NETEASE_PORT", &valid);
    customPort = qEnvironmentVariableIsSet("MUSICPLAYER_NETEASE_PORT");
    if (valid && configured > 0 && configured <= 65535)
        port = quint16(configured);
    else if (customPort)
        port = 0;
    network->setProxy(QNetworkProxy::NoProxy);
}

QString NeteaseClient::dataDirectory()
{
    QString directory = qEnvironmentVariable("MUSICPLAYER_DATA_DIR");
    if (directory.isEmpty())
        directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return directory;
}

void NeteaseClient::setAccountProfile(const QString &profileKey)
{
    const QString profile = profileKey.isEmpty() || profileKey == QStringLiteral("guest")
        ? QStringLiteral("guest")
        : QString::fromLatin1(QCryptographicHash::hash(profileKey.toUtf8(),
                              QCryptographicHash::Sha256).toHex());
    if (profile == activeMusicProfile)
        return;
    activeMusicProfile = profile;
    ++musicProfileGeneration;
    catalogContext = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString NeteaseClient::accountProfile()
{
    return activeMusicProfile;
}

QString NeteaseClient::catalogCacheContext()
{
    return catalogContext;
}

bool NeteaseClient::isAllowedStreamUrl(const QUrl &url)
{
    if (!url.isValid() || !url.userInfo().isEmpty())
        return false;
    if (url.scheme() == "https" && !url.host().isEmpty())
        return true;
    // Upstream still returns HTTP music.126.net links for some authorized media.
    const QString host = url.host().toLower();
    if (url.scheme() == "http" && (host == "music.126.net" || host.endsWith(".music.126.net")))
        return true;
    // Isolated native playback tests serve a generated WAV on their own loopback port.
    return qEnvironmentVariableIsSet("MUSICPLAYER_NETEASE_PORT") && url.scheme() == "http" &&
           host == "127.0.0.1";
}

QString NeteaseClient::projectDirectory() const
{
    QStringList roots{qEnvironmentVariable("MUSICPLAYER_PROJECT_DIR"), QDir::currentPath()};
    QDir executable(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 7; ++depth)
    {
        roots.append(executable.absolutePath());
        if (!executable.cdUp())
            break;
    }
    for (const QString &root : roots)
    {
        if (!root.isEmpty() &&
            QFileInfo(QDir(root).filePath("scripts/ensure-music-service.sh")).isFile() &&
            QFileInfo(QDir(root).filePath("service/package.json")).isFile())
            return root;
    }
    return {};
}

void NeteaseClient::ensureReady(ReadyCallback callback)
{
    if (!port)
    {
        callback(tr("网易云服务端口配置无效。"));
        return;
    }
    if (waiting.size() >= 32)
    {
        callback(tr("网易云请求过多，请稍后重试。"));
        return;
    }
    waiting.append(std::move(callback));
    if (probing)
        return;
    probing = true;
    probe(!customPort);
}

void NeteaseClient::probe(bool mayStartService)
{
    QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1/health").arg(port)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    auto *reply = network->get(request);
    boundReply(reply, 2000);
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, mayStartService]()
        {
            const auto object = QJsonDocument::fromJson(reply->readAll()).object();
            const bool oldCatalog = reply->error() == QNetworkReply::NoError &&
                object.value("app").toString() == "quiet-netease" && object.value("version").toInt() == 1 &&
                object.value("catalogVersion").toInt() != 2;
            const bool ok = reply->error() == QNetworkReply::NoError &&
                            object.value("app").toString() == "quiet-netease" &&
                            object.value("version").toInt() == 1 && object.value("catalogVersion").toInt() == 2;
            const bool occupied =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid();
            reply->deleteLater();
            if (oldCatalog) {
                // 新版按艺人ID读取作品，旧服务即使在线也不能被误认为具备此契约。
                finishReady(tr("本机网易云适配仍是旧版，请正常重启该服务后再查询；尚未执行曲库请求。"));
                return;
            }
            if (ok)
            {
                profileIsolationSupported = object.value("profileIsolation").toInt() == 1;
                finishReady({});
                return;
            }
            if (!mayStartService || occupied)
            {
                finishReady(tr("无法连接本机网易云服务，或端口被其他程序占用。"));
                return;
            }
            const QString root = projectDirectory();
            if (root.isEmpty())
            {
                finishReady(tr("找不到网易云服务启动脚本，请从项目目录运行播放器。"));
                return;
            }
            auto *process = new QProcess(this);
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.remove("MUSICPLAYER_AGENT_API_KEY");
            environment.remove("DASHSCOPE_API_KEY");
            process->setProcessEnvironment(environment);
            starter = process;
            process->setWorkingDirectory(root);
            process->setProcessChannelMode(QProcess::SeparateChannels);
            connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                    [this, process](int code, QProcess::ExitStatus status)
                    {
                        process->deleteLater();
                        starter.clear();
                        if (status == QProcess::NormalExit && code == 0)
                            probe(false);
                        else
                            finishReady(tr("网易云服务尚未就绪。请先在 VS Code 终端运行 bash "
                                           "scripts/setup-music-service.sh。"));
                    });
            connect(process, &QProcess::errorOccurred, this,
                    [this, process](QProcess::ProcessError error)
                    {
                        if (error == QProcess::FailedToStart)
                        {
                            process->deleteLater();
                            starter.clear();
                            finishReady(tr("无法启动本机网易云服务。"));
                        }
                    });
            QTimer::singleShot(12000, process,
                               [process]()
                               {
                                   if (process->state() != QProcess::NotRunning)
                                       process->kill();
                               });
            process->start("/bin/bash", {QDir(root).filePath("scripts/ensure-music-service.sh")});
        });
}

void NeteaseClient::finishReady(const QString &error)
{
    probing = false;
    const auto callbacks = std::move(waiting);
    waiting.clear();
    for (const auto &callback : callbacks)
        callback(error);
}

void NeteaseClient::get(const QString &path, const QUrlQuery &query, QObject *context,
                        Callback callback)
{
    send("GET", path, query, {}, context, std::move(callback));
}

void NeteaseClient::post(const QString &path, const QJsonObject &body, QObject *context,
                         Callback callback)
{
    send("POST", path, {}, body, context, std::move(callback));
}

void NeteaseClient::send(const QString &method, const QString &path, const QUrlQuery &query,
                         const QJsonObject &body, QObject *context, Callback callback)
{
    QPointer<QObject> guard(context);
    const QString requestProfile = activeMusicProfile;
    const quint64 profileGeneration = musicProfileGeneration;
    ensureReady(
        [this, method, path, query, body, guard, callback, requestProfile, profileGeneration](const QString &error)
        {
            if (!guard || profileGeneration != musicProfileGeneration)
                return;
            if (!error.isEmpty())
            {
                callback({}, error);
                return;
            }
            if (requestProfile != QStringLiteral("guest") && !profileIsolationSupported)
            {
                callback({}, tr("本机音乐服务版本较旧，无法隔离账号。请退出播放器并重新启动音乐服务。"));
                return;
            }
            QFile tokenFile(QDir(dataDirectory()).filePath("netease/token"));
            if (!tokenFile.open(QIODevice::ReadOnly) || tokenFile.size() > 256)
            {
                callback({}, tr("本机网易云服务认证文件不可读，请重新启动服务。"));
                return;
            }
            const QByteArray token = tokenFile.readAll().trimmed();
            if (token.size() < 16 || token.contains('\n') || token.contains('\r'))
            {
                callback({}, tr("本机网易云服务认证文件无效。"));
                return;
            }
            QUrl url(QString("http://127.0.0.1:%1%2").arg(port).arg(path));
            url.setQuery(query);
            QNetworkRequest request(url);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::ManualRedirectPolicy);
            request.setRawHeader("X-MusicPlayer-Token", token);
            if (requestProfile != QStringLiteral("guest"))
                request.setRawHeader("X-MusicPlayer-Profile", requestProfile.toLatin1());
            request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            auto *reply =
                method == "POST"
                    ? network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact))
                    : network->get(request);
            boundReply(reply, 18000);
            connect(guard.data(), &QObject::destroyed, reply, [reply] {
                reply->setProperty("contextDestroyed", true);
                reply->abort();
            });
            connect(reply, &QNetworkReply::finished, this,
                    [reply, guard, callback, profileGeneration, method, path]()
                    {
                        reply->deleteLater();
                        if (!guard || profileGeneration != musicProfileGeneration ||
                            reply->property("contextDestroyed").toBool())
                            return;
                        const QByteArray bytes = reply->readAll();
                        const auto networkError = reply->error();
                        const bool timedOut = reply->property("timedOut").toBool();
                        const bool tooLarge = reply->property("responseTooLarge").toBool();
                        QJsonParseError parse;
                        const auto document = QJsonDocument::fromJson(bytes, &parse);
                        const auto object = document.object();
                        QString failure;
                        if (tooLarge)
                            failure = QObject::tr("网易云接口返回内容过大。");
                        else if (timedOut)
                            failure = QObject::tr("网易云请求超时，请稍后重试。");
                        else if (!object.value("error").isUndefined())
                            failure = object.value("error").toObject().value("message").toString(
                                QObject::tr("网易云请求失败。"));
                        else if (networkError != QNetworkReply::NoError)
                            failure = QObject::tr("网易云服务暂不可用，请重试。");
                        else if (parse.error != QJsonParseError::NoError || !document.isObject())
                            failure = QObject::tr("网易云服务返回格式异常。");
                        if (failure.isEmpty() && method == QStringLiteral("POST") &&
                            (path == QStringLiteral("/logout") ||
                             (path == QStringLiteral("/login/qr/check") && object.value("code").toInt() == 803)))
                            catalogContext = QUuid::createUuid().toString(QUuid::WithoutBraces);
                        callback(object, failure);
                    });
        });
}
