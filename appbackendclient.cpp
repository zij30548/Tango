#include "appbackendclient.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QNetworkProxy>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <limits>
#include <memory>
#include <cmath>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace
{
constexpr qint64 ResponseLimit = 256 * 1024;
bool developmentModeRequested() { return qEnvironmentVariable("MUSICPLAYER_DEVELOPMENT_MODE") == QStringLiteral("1"); }
bool literalLoopback(const QUrl &url)
{
    return (url.host() == QStringLiteral("127.0.0.1") || url.host() == QStringLiteral("::1")) &&
        (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https")) &&
        url.path().isEmpty() && !url.hasQuery() && !url.hasFragment() && url.userInfo().isEmpty();
}

QByteArray privateDevelopmentToken()
{
    const QString path = qEnvironmentVariable("MUSICPLAYER_DEVELOPMENT_TOKEN_FILE");
    if (path.isEmpty() || !QDir::isAbsolutePath(path)) return {};
#ifdef Q_OS_UNIX
    const QByteArray encodedPath = QFile::encodeName(path);
    const int descriptor = ::open(encodedPath.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) return {};
    struct stat before{}, after{};
    const auto validFile = [](const struct stat &value) {
        return S_ISREG(value.st_mode) && value.st_uid == ::geteuid() &&
            (value.st_mode & 0077) == 0 && value.st_nlink == 1 && value.st_size >= 64 && value.st_size <= 65;
    };
    if (::fstat(descriptor, &before) != 0 || !validFile(before)) { ::close(descriptor); return {}; }
    QByteArray bytes(66, '\0');
    int received = 0;
    while (received < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + received, size_t(bytes.size() - received));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        received += int(count);
    }
    const bool stillPrivate = ::fstat(descriptor, &after) == 0 && validFile(after) &&
        before.st_dev == after.st_dev && before.st_ino == after.st_ino && after.st_size == received;
    ::close(descriptor);
    bytes.resize(received);
    if (bytes.endsWith('\n')) bytes.chop(1);
    bool valid = stillPrivate && bytes.size() == 64;
    for (const char byte : bytes) valid = valid && ((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'));
    if (!valid) { bytes.fill('\0'); return {}; }
    return bytes;
#else
    // No portable QFile permission check can establish an equivalent Windows
    // ACL boundary. Development bootstrap stays unavailable there by default.
    return {};
#endif
}
struct Configuration
{
    QUrl url;
    QString error;
};
QString configuredAddress()
{
    QString text = qEnvironmentVariable("MUSICPLAYER_BACKEND_URL").trimmed();
#ifdef QUIET_BACKEND_URL
    if (text.isEmpty())
        text = QString::fromUtf8(QUIET_BACKEND_URL).trimmed();
#endif
    return text;
}
Configuration configuration()
{
    const auto text = configuredAddress();
    Configuration result;
    if (text.isEmpty())
    {
        return result;
    }
    const QUrl url(text, QUrl::StrictMode);
    const auto host = url.host().toLower();
    const bool local = host == QStringLiteral("127.0.0.1") || host == QStringLiteral("localhost") ||
                       host == QStringLiteral("::1");
    const QString path = url.path();
    static const QRegularExpression allowedPath(QStringLiteral("^(?:/[A-Za-z0-9_-]+)*/?$"));
    if (text.size() > 2048 || !url.isValid() || host.isEmpty() || !url.userInfo().isEmpty() ||
        url.hasQuery() || url.hasFragment() || url.port() == 0 ||
        (url.scheme() != QStringLiteral("https") &&
         !(local && url.scheme() == QStringLiteral("http"))) ||
        url.path(QUrl::FullyEncoded).contains(QLatin1Char('%')) ||
        !allowedPath.match(path).hasMatch())
    {
        result.error =
            QStringLiteral("应用后台地址无效：须使用 HTTPS（本机调试允许 HTTP），且不能包含"
                           "账号、查询参数、片段或特殊路径。请联系应用维护者。");
        return result;
    }
    if (developmentModeRequested() && !((host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1")) &&
        (path.isEmpty() || path == QStringLiteral("/")))) {
        result.error = QStringLiteral("本机开发测试仅允许无路径前缀的本机回环地址，请使用开发启动配置。");
        return result;
    }
    result.url = url;
    QString cleanPath = path;
    while (cleanPath.endsWith(QLatin1Char('/')))
        cleanPath.chop(1);
    result.url.setPath(cleanPath);
    return result;
}

bool validUser(const QJsonObject &object)
{
    const auto id = object.value(QStringLiteral("id"));
    const auto name = object.value(QStringLiteral("username"));
    return id.isString() && !id.toString().trimmed().isEmpty() && id.toString().size() <= 128 &&
           name.isString() && !name.toString().trimmed().isEmpty() && name.toString().size() <= 100;
}

QDateTime expiry(const QJsonObject &object)
{
    const auto value = object.value(QStringLiteral("expiresAt"));
    return value.isString() ? QDateTime::fromString(value.toString(), Qt::ISODate) : QDateTime();
}

bool validToken(const QString &token)
{
    if (token.size() < 16 || token.size() > 4096)
        return false;
    for (const auto c : token)
        if (c.unicode() < 0x21 || c.unicode() > 0x7e)
            return false;
    return true;
}
} // namespace

AppBackendClient::AppBackendClient(QObject *parent, int timeoutMs)
    : QObject(parent), network(new QNetworkAccessManager(this)), expiryTimer(new QTimer(this)),
      defaultTimeoutMs(qBound(1, timeoutMs, 15000))
{
    expiryTimer->setSingleShot(true);
    connect(expiryTimer, &QTimer::timeout, this,
            [this]
            {
                const auto remaining = QDateTime::currentDateTimeUtc().msecsTo(expiresAt);
                if (remaining <= 0)
                    clearSession();
                else
                    expiryTimer->start(
                        int(qMin<qint64>(remaining, std::numeric_limits<int>::max())));
            });
    synchronizeConfiguration();
}

AppBackendClient *AppBackendClient::instance()
{
    static QPointer<AppBackendClient> client;
    if (!client)
        client = new AppBackendClient(QCoreApplication::instance());
    client->synchronizeConfiguration();
    return client;
}

bool AppBackendClient::enabled() { return !configuredAddress().isEmpty(); }
QString AppBackendClient::configurationError() { return configuration().error; }
bool AppBackendClient::developmentMode() { return developmentModeRequested(); }
bool AppBackendClient::authenticated() const
{
    return developmentSessionActive == developmentModeRequested() &&
        !sessionToken.isEmpty() && expiresAt > QDateTime::currentDateTimeUtc();
}
QString AppBackendClient::userId() const { return authenticated() ? currentUserId : QString(); }
QString AppBackendClient::username() const { return authenticated() ? currentUsername : QString(); }
QString AppBackendClient::serviceIdentity() const
{
    return QString::fromLatin1(
        QCryptographicHash::hash(baseUrl.toEncoded(), QCryptographicHash::Sha256).toHex());
}
bool AppBackendClient::agentReady() const { return authenticated() && ready; }
QString AppBackendClient::modelName() const { return agentReady() ? currentModel : QString(); }
bool AppBackendClient::taskRuntimeAvailable() const { return authenticated() && tasksAvailable; }
bool AppBackendClient::conversationAvailable() const { return authenticated() && conversationsAvailable; }
bool AppBackendClient::routingAvailable() const { return authenticated() && conversationsAvailable && routesAvailable; }

void AppBackendClient::setAgentStatus(bool value, const QString &model)
{
    const auto cleanModel = value ? model : QString();
    if (ready == value && currentModel == cleanModel)
        return;
    ready = value;
    currentModel = cleanModel;
    emit agentStatusChanged(ready, currentModel);
}

void AppBackendClient::clearSession()
{
    ++authRevision;
    ++authAttemptRevision;
    const bool changed = !sessionToken.isEmpty() || !currentUserId.isEmpty();
    sessionToken.fill('\0');
    sessionToken.clear();
    developmentSessionActive = false;
    currentUserId.clear();
    currentUsername.clear();
    expiresAt = QDateTime();
    expiryTimer->stop();
    tasksAvailable = false;
    conversationsAvailable = false; routesAvailable = false;
    setAgentStatus(false);
    if (changed)
        emit authChanged(QString(), QString());
}

void AppBackendClient::synchronizeConfiguration()
{
    const auto config = configuration();
    const bool development = developmentModeRequested();
    const QString tokenFile = development ? qEnvironmentVariable("MUSICPLAYER_DEVELOPMENT_TOKEN_FILE") : QString();
    if (baseUrl != config.url || configError != config.error || configuredDevelopmentMode != development ||
        configuredDevelopmentTokenFile != tokenFile)
    {
        clearSession(); // Neither real nor development sessions cross a mode/backend boundary.
        baseUrl = config.url;
        configError = config.error;
        configuredDevelopmentMode = development;
        configuredDevelopmentTokenFile = tokenFile;
        network->setProxy(QNetworkProxy(development ? QNetworkProxy::NoProxy : QNetworkProxy::DefaultProxy));
    }
}

QNetworkReply *AppBackendClient::request(const QString &method, const QString &route,
                                         const QJsonObject &body, bool authorize, QObject *context,
                                         JsonCallback callback, int timeoutMs,
                                         const QByteArray &developmentHeader)
{
    synchronizeConfiguration();
    if (!context || !callback)
        return nullptr;
    if (baseUrl.isEmpty() || !configError.isEmpty())
    {
        callback({}, baseUrl.isEmpty() && configError.isEmpty()
                         ? tr("AI 服务尚未配置，请联系应用维护者。")
                         : configError);
        return nullptr;
    }
    if (!developmentHeader.isEmpty() && (!developmentModeRequested() || !literalLoopback(baseUrl) ||
        route != QStringLiteral("/v1/dev/session") || method != QStringLiteral("POST") || authorize)) {
        callback({}, tr("本机开发会话配置无效，请使用开发启动配置。"));
        return nullptr;
    }
    if (authorize && !authenticated())
    {
        clearSession();
        callback({}, tr("请先登录探歌账户，再使用 AI 专注服务。"));
        return nullptr;
    }
    QUrl endpoint = baseUrl;
    endpoint.setPath(baseUrl.path() + route);
    QNetworkRequest request(endpoint);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (authorize)
        request.setRawHeader("Authorization", QByteArray("Bearer ") + sessionToken);
    if (!developmentHeader.isEmpty())
        request.setRawHeader("X-Tango-Development-Token", developmentHeader);
    const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    // 清除对话使用明确的DELETE，不把所有非GET请求误发成POST。
    auto *reply = method == QStringLiteral("GET") ? network->get(request)
        : method == QStringLiteral("DELETE") ? network->sendCustomRequest(request, QByteArrayLiteral("DELETE"), payload)
        : network->post(request, payload);
    reply->setReadBufferSize(ResponseLimit + 1);
    const auto revision = authRevision;
    const QPointer<QObject> guard(context);
    auto bytes = std::make_shared<QByteArray>();
    const auto collect = [reply, bytes]
    {
        if (!reply->isOpen())
            return;
        if (bytes->size() <= ResponseLimit)
            bytes->append(reply->read(ResponseLimit + 1 - bytes->size()));
        if (bytes->size() > ResponseLimit)
        {
            reply->setProperty("tooLarge", true);
            reply->abort();
        }
    };
    connect(reply, &QNetworkReply::readyRead, reply, collect);
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply](qint64 received, qint64 total)
            {
                if (received > ResponseLimit || total > ResponseLimit)
                {
                    reply->setProperty("tooLarge", true);
                    reply->abort();
                }
            });
    connect(context, &QObject::destroyed, reply, [reply] {
        // QWidget emits destroyed while its QPointer can still be non-null.
        // abort() may emit finished synchronously, after the view's children
        // are already gone, so invalidate delivery before aborting the reply.
        reply->setProperty("contextDestroyed", true);
        reply->abort();
    });
    QTimer::singleShot(qBound(1, timeoutMs, 15000), reply,
                       [reply]
                       {
                           if (reply->isRunning())
                           {
                               reply->setProperty("timedOut", true);
                               reply->abort();
                           }
                       });
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, bytes, collect, guard, callback, authorize, revision, route]
        {
            collect();
            reply->deleteLater();
            if (!guard || reply->property("contextDestroyed").toBool())
                return;
            QString error;
            QJsonObject object;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (authorize && revision != authRevision && route != QStringLiteral("/v1/auth/logout"))
                error = tr("探歌账户已切换，请重新操作。");
            else if (reply->property("tooLarge").toBool())
                error = tr("应用后台回复超过 256 KiB，已停止处理。");
            else if (reply->property("timedOut").toBool())
                error = tr("应用后台请求超时，请稍后重试。");
            else if (status >= 300 && status < 400)
                error = tr("应用后台要求重定向，已拒绝跳转。请联系应用维护者。");
            else
            {
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(*bytes, &parseError);
                if (parseError.error == QJsonParseError::NoError && document.isObject())
                    object = document.object();
                if (status < 200 || status >= 300)
                {
                    const auto message = object.value(QStringLiteral("error"))
                                             .toObject()
                                             .value(QStringLiteral("message"));
                    if (message.isString() && !message.toString().trimmed().isEmpty() &&
                        message.toString().size() <= 500)
                        error = message.toString();
                    else
                        error = tr("无法完成应用后台请求，请稍后重试。");
                    if (status == 401 && authorize && revision == authRevision)
                        clearSession();
                }
                else if (reply->error() != QNetworkReply::NoError)
                    error = tr("无法连接应用后台，请检查网络后重试。");
                else if (parseError.error != QJsonParseError::NoError || !document.isObject())
                    error = tr("应用后台返回的内容不是有效 JSON。");
            }
            // 错误只暴露受限协议事实；任务入口据完整机器码区分明确拒绝和传输未知，不能复用成功正文。
            if (!error.isEmpty() && (route.startsWith(QStringLiteral("/v1/conversations")) ||
                                      route == QStringLiteral("/v1/tasks") || route.startsWith(QStringLiteral("/v1/tasks/")))) {
                QJsonObject metadata{{QStringLiteral("httpStatus"), status}};
                {
                    const auto code = object.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString();
                    const auto networkError = reply->error();
                    const bool httpError = networkError == QNetworkReply::NoError ||
                        (status == 400 && networkError == QNetworkReply::ProtocolInvalidOperationError) ||
                        (networkError >= QNetworkReply::ContentAccessDenied && networkError <= QNetworkReply::UnknownContentError) ||
                        (networkError >= QNetworkReply::InternalServerError && networkError <= QNetworkReply::UnknownServerError);
                    const bool complete = !reply->property("tooLarge").toBool() && !reply->property("timedOut").toBool() &&
                        httpError &&
                        status >= 400 && status < 600 && object.value(QStringLiteral("error")).isObject() &&
                        QRegularExpression(QStringLiteral("^[A-Z][A-Z0-9_]{0,79}$")).match(code).hasMatch();
                    metadata.insert(QStringLiteral("completeHttp"), complete);
                    if (complete) metadata.insert(QStringLiteral("errorCode"), code);
                }
                callback(metadata, error);
            } else callback(error.isEmpty() ? object : QJsonObject(), error);
        });
    return reply;
}

void AppBackendClient::authenticate(const QString &route, const QString &name,
                                    const QString &password, QObject *context, Callback callback,
                                    const QJsonObject &extra)
{
    if (!context || !callback)
        return;
    synchronizeConfiguration();
    if (developmentModeRequested()) {
        ++authAttemptRevision;
        callback(tr("当前为本机开发测试，请使用开发会话连接，无需登录真实账户。"));
        return;
    }
    // Trying another account must not sign the current account out. Keep its
    // session until the newest authentication attempt has fully succeeded.
    const auto attemptRevision = ++authAttemptRevision;
    QJsonObject body{{QStringLiteral("username"), name.trimmed()},
                     {QStringLiteral("password"), password}};
    if (route == QStringLiteral("/v1/auth/register") && extra.contains(QStringLiteral("inviteCode")))
        body.insert(QStringLiteral("inviteCode"), extra.value(QStringLiteral("inviteCode")));
    if (name.size() > 100 || password.size() > 256)
    {
        callback(tr("账户名或密码过长。"));
        return;
    }
    request(
        QStringLiteral("POST"), route, body, false, context,
        [this, callback, attemptRevision](const QJsonObject &object, const QString &error)
        {
            if (attemptRevision != authAttemptRevision)
            {
                callback(tr("登录请求已失效，请重新操作。"));
                return;
            }
            if (!error.isEmpty())
            {
                callback(error);
                return;
            }
            const auto user = object.value(QStringLiteral("user")).toObject();
            const auto token = object.value(QStringLiteral("token"));
            const auto until = expiry(object);
            if (!validUser(user) || !token.isString() || !validToken(token.toString()) ||
                !until.isValid() || until <= QDateTime::currentDateTimeUtc())
            {
                callback(tr("应用后台返回的登录信息无效，请联系应用维护者。"));
                return;
            }
            ++authRevision;
            ++authAttemptRevision;
            sessionToken.fill('\0');
            sessionToken = token.toString().toLatin1();
            developmentSessionActive = false;
            currentUserId = user.value(QStringLiteral("id")).toString();
            currentUsername = user.value(QStringLiteral("username")).toString();
            expiresAt = until;
            expiryTimer->start(int(qMin<qint64>(QDateTime::currentDateTimeUtc().msecsTo(until),
                                                std::numeric_limits<int>::max())));
            tasksAvailable = false;
            conversationsAvailable = false; routesAvailable = false;
            setAgentStatus(false);
            emit authChanged(currentUserId, currentUsername);
            callback(QString());
        },
        defaultTimeoutMs);
}

void AppBackendClient::bootstrapDevelopmentSession(QObject *context, Callback callback)
{
    if (!context || !callback) return;
    synchronizeConfiguration();
    if (!developmentModeRequested()) { callback(tr("未启用本机开发测试。")); return; }
    if (authenticated() && developmentSessionActive) { callback({}); return; }
    // Revoke only local access. Never send a logout for a real account here.
    clearSession();
    const auto attemptRevision = ++authAttemptRevision;
    if (baseUrl.isEmpty() || !configError.isEmpty() || !literalLoopback(baseUrl)) {
        callback(tr("本机开发测试仅允许本机回环后台，请使用开发启动配置。"));
        return;
    }
    QByteArray capability = privateDevelopmentToken();
    if (capability.isEmpty()) {
        callback(tr("无法读取私有开发令牌，请使用开发启动配置重新启动。"));
        return;
    }
    const QPointer<QObject> guard(context);
    // The actual capability request below is intentionally the only place that
    // supplies the dedicated header; ordinary APIs never inherit it.
    request(QStringLiteral("POST"), QStringLiteral("/v1/dev/session"), {}, false, context,
        [this, attemptRevision, guard, callback](const QJsonObject &object, const QString &error) {
            synchronizeConfiguration();
            if (!guard) return;
            if (attemptRevision != authAttemptRevision || !developmentModeRequested()) {
                callback(tr("本机开发连接已失效，请重试。")); return;
            }
            if (!error.isEmpty()) {
                callback(tr("本机开发会话连接失败，请确认开发后台已启动后重试。")); return;
            }
            const auto user = object.value(QStringLiteral("user")).toObject();
            const auto token = object.value(QStringLiteral("token"));
            const auto until = expiry(object);
            if (!object.value(QStringLiteral("development")).isBool() ||
                !object.value(QStringLiteral("development")).toBool() || !validUser(user) ||
                !token.isString() || !validToken(token.toString()) || !until.isValid() || until <= QDateTime::currentDateTimeUtc()) {
                callback(tr("后台未返回有效的本机开发身份，请检查开发启动配置。")); return;
            }
            ++authRevision; ++authAttemptRevision;
            sessionToken.fill('\0'); sessionToken = token.toString().toLatin1();
            developmentSessionActive = true;
            currentUserId = user.value(QStringLiteral("id")).toString();
            currentUsername = user.value(QStringLiteral("username")).toString(); expiresAt = until;
            expiryTimer->start(int(qMin<qint64>(QDateTime::currentDateTimeUtc().msecsTo(until), std::numeric_limits<int>::max())));
            tasksAvailable = false; conversationsAvailable = false; routesAvailable = false; setAgentStatus(false);
            emit authChanged(currentUserId, currentUsername);
            callback({});
        }, defaultTimeoutMs, capability);
    capability.fill('\0');
}

void AppBackendClient::registerUser(const QString &name, const QString &password, QObject *context,
                                    Callback callback)
{
    authenticate(QStringLiteral("/v1/auth/register"), name, password, context, callback);
}
void AppBackendClient::registerUser(const QString &name, const QString &password,
                                    const QString &inviteCode, QObject *context, Callback callback)
{
    if (inviteCode.size() > 128) {
        if (context && callback) {
            ++authAttemptRevision;
            callback(tr("内测码格式无效。"));
        }
        return;
    }
    QJsonObject extra;
    if (!inviteCode.trimmed().isEmpty()) extra.insert(QStringLiteral("inviteCode"), inviteCode.trimmed());
    authenticate(QStringLiteral("/v1/auth/register"), name, password, context, callback, extra);
}

QNetworkReply *AppBackendClient::registrationConfig(QObject *context, JsonCallback callback)
{
    if (!context || !callback) return nullptr;
    synchronizeConfiguration();
    const QString identity = serviceIdentity();
    const QPointer<QObject> guard(context);
    return request(QStringLiteral("GET"), QStringLiteral("/v1/auth/config"), {}, false, context,
        [this, identity, guard, callback](const QJsonObject &object, const QString &error) {
            synchronizeConfiguration();
            if (!guard) return;
            if (identity != serviceIdentity()) {
                callback({}, tr("应用后台已变更，请重新读取注册方式。"));
                return;
            }
            if (!error.isEmpty()) { callback({}, error); return; }
            const auto mode = object.value(QStringLiteral("registrationMode")).toString();
            const auto integer = [&object](const char *key, int minimum, int maximum) {
                const auto value = object.value(QString::fromLatin1(key));
                return value.isDouble() && value.toDouble() >= minimum && value.toDouble() <= maximum &&
                       value.toDouble() == value.toInt();
            };
            const bool valid = object.value(QStringLiteral("version")).toInt(-1) == 1 &&
                object.value(QStringLiteral("version")).toDouble(-1) == 1 &&
                QStringList{QStringLiteral("invite_only"), QStringLiteral("open"), QStringLiteral("closed")}.contains(mode) &&
                object.value(QStringLiteral("inviteRequired")).isBool() &&
                object.value(QStringLiteral("inviteRequired")).toBool() == (mode == QStringLiteral("invite_only")) &&
                integer("passwordMin", 8, 128) && integer("passwordMax", 8, 128) &&
                object.value(QStringLiteral("passwordMin")).toInt() <= object.value(QStringLiteral("passwordMax")).toInt() &&
                integer("usernameMin", 1, 100) && integer("usernameMax", 1, 100) &&
                object.value(QStringLiteral("usernameMin")).toInt() <= object.value(QStringLiteral("usernameMax")).toInt();
            if (!valid) { callback({}, tr("无法核验注册方式，请重试或联系应用维护者。")); return; }
            callback(object, {});
        }, defaultTimeoutMs);
}

QNetworkReply *AppBackendClient::accountUsage(QObject *context, JsonCallback callback)
{
    return request(QStringLiteral("GET"), QStringLiteral("/v1/account/usage"), {}, true,
                   context, callback, defaultTimeoutMs);
}

void AppBackendClient::login(const QString &name, const QString &password, QObject *context,
                             Callback callback)
{
    authenticate(QStringLiteral("/v1/auth/login"), name, password, context, callback);
}
void AppBackendClient::logout(QObject *context, Callback callback)
{
    if (!authenticated())
    {
        clearSession();
        if (context && callback)
            callback(QString());
        return;
    }
    request(
        QStringLiteral("POST"), QStringLiteral("/v1/auth/logout"), {}, true, context,
        [callback](const QJsonObject &, const QString &error)
        {
            if (callback)
                callback(error);
        },
        defaultTimeoutMs);
    clearSession(); // Local access ends immediately, even if the network is
                    // unavailable.
}
void AppBackendClient::me(QObject *context, Callback callback)
{
    synchronizeConfiguration();
    const auto revision = authRevision;
    request(
        QStringLiteral("GET"), QStringLiteral("/v1/auth/me"), {}, true, context,
        [this, callback, revision](const QJsonObject &object, const QString &error)
        {
            QString result = error;
            if (revision == authRevision && result.isEmpty())
            {
                const auto user = object.value(QStringLiteral("user")).toObject();
                if (!validUser(user) ||
                    user.value(QStringLiteral("id")).toString() != currentUserId ||
                    user.value(QStringLiteral("username")).toString() != currentUsername ||
                    !expiry(object).isValid() || expiry(object) <= QDateTime::currentDateTimeUtc())
                {
                    clearSession();
                    result = tr("账户会话校验失败，请重新登录探歌。");
                }
            }
            if (callback)
                callback(result);
        },
        defaultTimeoutMs);
}
void AppBackendClient::refreshAgentStatus(QObject *context, Callback callback)
{
    synchronizeConfiguration();
    const auto revision = authRevision;
    request(
        QStringLiteral("GET"), QStringLiteral("/v1/agent/status"), {}, true, context,
        [this, callback, revision](const QJsonObject &object, const QString &error)
        {
            QString result = error;
            if (revision != authRevision)
            {
                // A previous account's delayed status must not clear the AI
                // availability already established for the new account.
                if (callback)
                    callback(result);
                return;
            }
            if (result.isEmpty())
            {
                if (!object.value(QStringLiteral("configured")).isBool() ||
                    !object.value(QStringLiteral("model")).isString() ||
                    object.value(QStringLiteral("model")).toString().size() > 200)
                    result = tr("AI 服务状态格式无效，请联系应用维护者。");
            }
            const bool oldTasks = tasksAvailable;
            const bool oldConversations = conversationsAvailable;
            const bool oldRoutes = routesAvailable;
            tasksAvailable = result.isEmpty() && object.value(QStringLiteral("taskRuntime")).toBool() &&
                             object.value(QStringLiteral("taskVersion")).toInt() == 1;
            conversationsAvailable = result.isEmpty() && object.value(QStringLiteral("conversationVersion")).toInt() == 1;
            routesAvailable = result.isEmpty() && object.value(QStringLiteral("routingVersion")).toInt() == 1;
            const bool statusUnchanged = ready == (result.isEmpty() && object.value(QStringLiteral("configured")).toBool()) &&
                currentModel == (result.isEmpty() && object.value(QStringLiteral("configured")).toBool() ? object.value(QStringLiteral("model")).toString() : QString());
            setAgentStatus(result.isEmpty() && object.value(QStringLiteral("configured")).toBool(),
                           object.value(QStringLiteral("model")).toString());
            if ((oldTasks != tasksAvailable || oldConversations != conversationsAvailable || oldRoutes != routesAvailable) && statusUnchanged)
                emit agentStatusChanged(ready, currentModel);
            if (callback)
                callback(result);
        },
        defaultTimeoutMs);
}
QNetworkReply *AppBackendClient::parseFocus(const QJsonObject &body, QObject *context,
                                            JsonCallback callback, int timeoutMs)
{
    return request(QStringLiteral("POST"), QStringLiteral("/v1/focus/parse"), body, true, context,
                   callback, timeoutMs);
}

QNetworkReply *AppBackendClient::planRadio(const QJsonObject &body, QObject *context,
                                         JsonCallback callback)
{
    return request(QStringLiteral("POST"), QStringLiteral("/v1/radio/plan"), body, true,
                   context, callback, defaultTimeoutMs);
}

QNetworkReply *AppBackendClient::selectRadio(const QJsonObject &body, QObject *context,
                                           JsonCallback callback)
{
    return request(QStringLiteral("POST"), QStringLiteral("/v1/radio/select"), body, true,
                   context, callback, defaultTimeoutMs);
}

QNetworkReply *AppBackendClient::planAssistant(const QJsonObject &body, QObject *context,
                                             JsonCallback callback)
{
    return request(QStringLiteral("POST"), QStringLiteral("/v1/assistant/plan"), body, true,
                   context, callback, defaultTimeoutMs);
}

namespace {
QString taskSegment(const QString &value) {
    static const QRegularExpression allowed(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return allowed.match(value).hasMatch() ? value : QString();
}
}
QNetworkReply *AppBackendClient::createMusicTask(const QJsonObject &body, QObject *context,
                                                JsonCallback callback) {
    return request(QStringLiteral("POST"), QStringLiteral("/v1/tasks"), body, true, context,
                   callback, 15000);
}
QNetworkReply *AppBackendClient::latestMusicTask(const QString &deviceId, QObject *context,
                                                JsonCallback callback) {
    const auto id = taskSegment(deviceId);
    if (id.isEmpty()) { if (context && callback) callback({}, tr("设备标识无效。")); return nullptr; }
    return request(QStringLiteral("GET"), QStringLiteral("/v1/tasks/latest/") + id, {}, true,
                   context, callback, 15000);
}
QNetworkReply *AppBackendClient::musicTask(const QString &taskId, QObject *context,
                                          JsonCallback callback) {
    const auto id = taskSegment(taskId);
    if (id.isEmpty()) { if (context && callback) callback({}, tr("任务标识无效。")); return nullptr; }
    return request(QStringLiteral("GET"), QStringLiteral("/v1/tasks/") + id, {}, true,
                   context, callback, 15000);
}
QNetworkReply *AppBackendClient::musicTaskRequest(const QString &deviceId, const QString &requestId,
                                                 QObject *context, JsonCallback callback) {
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_-]{8,100}$"));
    if (!valid.match(deviceId).hasMatch() || !valid.match(requestId).hasMatch()) {
        if (context && callback) callback({}, tr("原操作或设备标识无效。"));
        return nullptr;
    }
    return request(QStringLiteral("GET"), QStringLiteral("/v1/tasks/requests/") + deviceId +
                   QLatin1Char('/') + requestId, {}, true, context, callback, 15000);
}
QNetworkReply *AppBackendClient::musicTaskEvents(const QString &taskId, qint64 lastStateVersion,
                                                 QObject *context, JsonCallback progress,
                                                 StreamClosedCallback closed, int idleTimeoutMs) {
    synchronizeConfiguration();
    if (!context || !progress || !closed) return nullptr;
    const auto id = taskSegment(taskId);
    if (id.isEmpty() || lastStateVersion < 0 || lastStateVersion > 9007199254740991LL) {
        closed(0, tr("任务进度标识无效。")); return nullptr;
    }
    if (baseUrl.isEmpty() || !configError.isEmpty() || !authenticated()) {
        closed(0, tr("任务进度连接不可用，请检查后台配置和账户登录状态。")); return nullptr;
    }
    QUrl endpoint = baseUrl;
    endpoint.setPath(baseUrl.path() + QStringLiteral("/v1/tasks/") + id + QStringLiteral("/events"));
    QNetworkRequest request(endpoint);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", "text/event-stream");
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("Authorization", QByteArray("Bearer ") + sessionToken);
    request.setRawHeader("Last-Event-ID", QByteArray::number(lastStateVersion));
    auto *reply = network->get(request);
    constexpr int FrameLimit = 16 * 1024;
    reply->setReadBufferSize(FrameLimit * 2 + 1);
    const auto revision = authRevision;
    const QPointer<QObject> guard(context);
    struct Parser {
        QByteArray pending, data, event, eventId;
        bool rejected = false;
    };
    const auto parser = std::make_shared<Parser>();
    auto *idle = new QTimer(reply);
    idle->setSingleShot(true);
    const int idleMs = qBound(100, idleTimeoutMs, 120000);
    idle->start(idleMs);
    connect(idle, &QTimer::timeout, reply, [reply] {
        reply->setProperty("streamIdle", true); reply->abort();
    });
    const auto reject = [reply, parser] {
        parser->rejected = true; reply->abort();
    };
    const auto collect = [this, reply, parser, guard, progress, reject, revision, idle, idleMs, id] {
        if (!guard || revision != authRevision || reply->property("contextDestroyed").toBool() ||
            !reply->isOpen() || parser->rejected) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) { reply->readAll(); return; }
        const auto contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString()
            .section(QLatin1Char(';'), 0, 0).trimmed();
        if (contentType.compare(QStringLiteral("text/event-stream"), Qt::CaseInsensitive) != 0) {
            reject(); return;
        }
        const auto bytes = reply->read(FrameLimit * 2 + 1);
        if (bytes.isEmpty()) return;
        idle->start(idleMs); // Heartbeat comments also prove the transport is alive.
        parser->pending.append(bytes);
        if (parser->pending.size() > FrameLimit * 2) { reject(); return; }
        for (;;) {
            int end = -1, separator = 1;
            for (int i = 0; i < parser->pending.size(); ++i) {
                const char c = parser->pending.at(i);
                if (c == '\n') { end = i; break; }
                if (c == '\r') {
                    if (i + 1 == parser->pending.size()) return; // Possibly a split CRLF.
                    end = i; separator = parser->pending.at(i + 1) == '\n' ? 2 : 1; break;
                }
            }
            if (end < 0) {
                if (parser->pending.size() > FrameLimit) reject();
                return;
            }
            if (end > FrameLimit) { reject(); return; }
            const auto line = parser->pending.left(end);
            parser->pending.remove(0, end + separator);
            if (line.isEmpty()) {
                const auto event = parser->event;
                auto data = parser->data;
                const auto eventId = parser->eventId;
                parser->event.clear(); parser->data.clear(); parser->eventId.clear();
                if (data.endsWith('\n')) data.chop(1);
                if (event == "auth_expired") {
                    clearSession(); return;
                }
                if (event != "progress" || data.isEmpty()) continue;
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(data, &parseError);
                const auto value = document.object();
                const auto version = value.value("stateVersion");
                const auto turn = value.value("revision");
                const double v = version.toDouble(-1), r = turn.toDouble(-1);
                const auto at = value.value("at");
                const double milliseconds = at.toDouble(-1);
                const bool validAt = (at.isDouble() && std::isfinite(milliseconds) &&
                    milliseconds > 0 && milliseconds <= 253402300799999. && std::floor(milliseconds) == milliseconds) ||
                    (at.isString() && at.toString().size() <= 64 &&
                     QDateTime::fromString(at.toString(), Qt::ISODate).isValid());
                bool idOkay = false;
                const qint64 eventVersion = eventId.toLongLong(&idOkay);
                if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
                    value.value("taskId").toString() != id || !version.isDouble() ||
                    !std::isfinite(v) || v < 0 || v > 9007199254740991. || std::floor(v) != v ||
                    !turn.isDouble() || !std::isfinite(r) || r < 1 || r > 2147483647. || std::floor(r) != r ||
                    (!eventId.isEmpty() && (!idOkay || eventVersion != qint64(v))) ||
                    !value.value("status").isString() || value.value("status").toString().size() > 64 ||
                    !value.value("stage").isString() || value.value("stage").toString().size() > 64 ||
                    !value.value("message").isString() || value.value("message").toString().size() > 1000 ||
                    !validAt) {
                    reject(); return;
                }
                progress(value, QString());
                if (!guard || revision != authRevision || reply->property("contextDestroyed").toBool()) return;
            } else if (!line.startsWith(':')) {
                const int colon = line.indexOf(':');
                const auto field = colon < 0 ? line : line.left(colon);
                auto value = colon < 0 ? QByteArray() : line.mid(colon + 1);
                if (value.startsWith(' ')) value.remove(0, 1);
                if (field == "data") parser->data.append(value + '\n');
                else if (field == "event") parser->event = value;
                else if (field == "id" && !value.contains('\0')) parser->eventId = value;
                if (parser->data.size() > FrameLimit || parser->event.size() > 64 ||
                    parser->eventId.size() > 64) { reject(); return; }
            }
        }
    };
    connect(reply, &QNetworkReply::readyRead, reply, collect);
    connect(context, &QObject::destroyed, reply, [reply] {
        reply->setProperty("contextDestroyed", true); reply->abort();
    });
    connect(this, &AppBackendClient::authChanged, reply, [this, reply, revision] {
        if (authRevision != revision) {
            reply->setProperty("contextDestroyed", true); reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, guard, closed, collect, parser, revision] {
            collect(); reply->deleteLater();
            if (!guard || revision != authRevision || reply->property("contextDestroyed").toBool()) return;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 401) { clearSession(); return; }
            QString error;
            if (parser->rejected) error = tr("任务进度格式无效，已改为查询已保存进度。");
            else if (reply->property("streamIdle").toBool()) error = tr("任务进度连接超时，正在重新连接。");
            else if (status != 200 || reply->error() != QNetworkReply::NoError)
                error = tr("任务进度连接中断，正在查询已保存进度。");
            closed(status, error);
        });
    return reply;
}
QNetworkReply *AppBackendClient::musicTaskAction(const QString &taskId, const QJsonObject &body,
                                                QObject *context, JsonCallback callback) {
    const auto id = taskSegment(taskId);
    if (id.isEmpty()) { if (context && callback) callback({}, tr("任务标识无效。")); return nullptr; }
    return request(QStringLiteral("POST"), QStringLiteral("/v1/tasks/") + id + QStringLiteral("/actions"),
                   body, true, context, callback, 15000);
}
QNetworkReply *AppBackendClient::completeMusicTaskStep(const QString &taskId, const QString &stepId,
                                                      const QJsonObject &body, QObject *context,
                                                      JsonCallback callback) {
    const auto id = taskSegment(taskId), step = taskSegment(stepId);
    if (id.isEmpty() || step.isEmpty()) {
        if (context && callback) callback({}, tr("任务步骤标识无效。")); return nullptr;
    }
    return request(QStringLiteral("POST"), QStringLiteral("/v1/tasks/") + id + QStringLiteral("/steps/") + step + QStringLiteral("/result"),
                   body, true, context, callback, 15000);
}

QNetworkReply *AppBackendClient::createConversation(const QJsonObject &body, QObject *context,
                                                  JsonCallback callback) {
    return request(QStringLiteral("POST"), QStringLiteral("/v1/conversations"), body, true,
                   context, callback, defaultTimeoutMs);
}
QNetworkReply *AppBackendClient::clearConversations(const QJsonObject &body, QObject *context,
                                                  JsonCallback callback) {
    return request(QStringLiteral("DELETE"), QStringLiteral("/v1/conversations"), body, true,
                   context, callback, defaultTimeoutMs);
}
QNetworkReply *AppBackendClient::latestConversation(const QString &deviceId, QObject *context,
                                                  JsonCallback callback) {
    const auto id = taskSegment(deviceId);
    if (id.isEmpty()) { if (context && callback) callback({}, tr("设备标识无效。")); return nullptr; }
    return request(QStringLiteral("GET"), QStringLiteral("/v1/conversations/latest/") + id,
                   {}, true, context, callback, defaultTimeoutMs);
}
QNetworkReply *AppBackendClient::conversation(const QString &conversationId, QObject *context,
                                             JsonCallback callback) {
    const auto id = taskSegment(conversationId);
    if (id.isEmpty()) { if (context && callback) callback({}, tr("会话标识无效。")); return nullptr; }
    return request(QStringLiteral("GET"), QStringLiteral("/v1/conversations/") + id,
                   {}, true, context, callback, defaultTimeoutMs);
}
QNetworkReply *AppBackendClient::appendConversationMessage(const QString &conversationId,
    const QJsonObject &body, QObject *context, JsonCallback callback) {
    const auto id = taskSegment(conversationId);
    if (id.isEmpty()) { if (context && callback) callback({}, tr("会话标识无效。")); return nullptr; }
    return request(QStringLiteral("POST"), QStringLiteral("/v1/conversations/") + id + QStringLiteral("/messages"),
                   body, true, context, callback, defaultTimeoutMs);
}
QNetworkReply *AppBackendClient::conversationMessage(const QString &conversationId, const QString &messageId,
    QObject *context, JsonCallback callback) {
    const auto id = taskSegment(conversationId), message = taskSegment(messageId);
    if (id.isEmpty() || message.isEmpty()) { if (context && callback) callback({}, tr("消息标识无效。")); return nullptr; }
    return request(QStringLiteral("GET"), QStringLiteral("/v1/conversations/") + id + QStringLiteral("/messages/") + message,
                   {}, true, context, callback, defaultTimeoutMs);
}
QNetworkReply *AppBackendClient::appendConversationEvent(const QString &conversationId,
    const QJsonObject &body, QObject *context, JsonCallback callback) {
    const auto id = taskSegment(conversationId);
    if (id.isEmpty()) { if (context && callback) callback({}, tr("会话标识无效。")); return nullptr; }
    return request(QStringLiteral("POST"), QStringLiteral("/v1/conversations/") + id + QStringLiteral("/events"),
                   body, true, context, callback, defaultTimeoutMs);
}
QNetworkReply *AppBackendClient::conversationBefore(const QString &conversationId, qint64 sequence,
    QObject *context, JsonCallback callback) {
    const auto id = taskSegment(conversationId);
    if (id.isEmpty() || sequence < 1 || sequence > 9007199254740991LL) {
        if (context && callback) callback({}, tr("会话分页标识无效。")); return nullptr;
    }
    return request(QStringLiteral("GET"), QStringLiteral("/v1/conversations/") + id + QStringLiteral("/before/") + QString::number(sequence),
                   {}, true, context, callback, defaultTimeoutMs);
}

QNetworkReply *AppBackendClient::routeConversationMessage(const QString &conversationId, const QString &messageId,
    const QJsonObject &body, QObject *context, JsonCallback callback) {
    const auto id = taskSegment(conversationId), message = taskSegment(messageId);
    if (id.isEmpty() || message.isEmpty() || !routingAvailable()) {
        if (context && callback) callback({}, tr("当前后台不支持统一意图路由。")); return nullptr;
    }
    return request(QStringLiteral("POST"), QStringLiteral("/v1/conversations/") + id +
        QStringLiteral("/messages/") + message + QStringLiteral("/route"), body, true, context, callback, 25000);
}
QNetworkReply *AppBackendClient::conversationMessageRoute(const QString &conversationId, const QString &messageId,
    QObject *context, JsonCallback callback) {
    const auto id = taskSegment(conversationId), message = taskSegment(messageId);
    if (id.isEmpty() || message.isEmpty() || !routingAvailable()) {
        if (context && callback) callback({}, tr("当前后台不支持统一意图路由。")); return nullptr;
    }
    return request(QStringLiteral("GET"), QStringLiteral("/v1/conversations/") + id +
        QStringLiteral("/messages/") + message + QStringLiteral("/route"), {}, true, context, callback, defaultTimeoutMs);
}
