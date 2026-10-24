#include "focusmodelclient.h"
#include "appbackendclient.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSet>
#include <cmath>

namespace
{
QJsonObject requestObject(const FocusRequest &request)
{
    return {
        {QStringLiteral("minutes"), request.minutes},
        {QStringLiteral("scene"), request.scene},
        {QStringLiteral("source"), request.source},
        {QStringLiteral("excludeLive"), request.excludeLive},
        {QStringLiteral("instrumentalOnly"), request.instrumentalOnly},
        {QStringLiteral("excludedArtists"), QJsonArray::fromStringList(request.excludedArtists)}};
}

QString validate(const QJsonObject &object, FocusRequest *request)
{
    const QSet<QString> expected{
        QStringLiteral("minutes"),          QStringLiteral("scene"),
        QStringLiteral("source"),           QStringLiteral("excludeLive"),
        QStringLiteral("instrumentalOnly"), QStringLiteral("excludedArtists")};
    const auto keys = object.keys();
    if (QSet<QString>(keys.cbegin(), keys.cend()) != expected)
        return QStringLiteral("AI 返回了缺失或未知字段，请调整指令或使用表单设置。");
    const auto minutes = object.value(QStringLiteral("minutes"));
    if (!minutes.isDouble() || !std::isfinite(minutes.toDouble()) ||
        minutes.toDouble() != std::floor(minutes.toDouble()) || minutes.toDouble() < 1 ||
        minutes.toDouble() > 180)
        return QStringLiteral("AI 返回的时长无效；专注时长应为 1 至 180 分钟。");
    const auto scene = object.value(QStringLiteral("scene"));
    const auto source = object.value(QStringLiteral("source"));
    if (!scene.isString() ||
        !QStringList{QStringLiteral("work"), QStringLiteral("study"), QStringLiteral("rest")}
             .contains(scene.toString()) ||
        !source.isString() ||
        !QStringList{QStringLiteral("queue"), QStringLiteral("favorites")}.contains(
            source.toString()))
        return QStringLiteral("AI 返回的场景或曲目来源不受支持。");
    if (!object.value(QStringLiteral("excludeLive")).isBool() ||
        !object.value(QStringLiteral("instrumentalOnly")).isBool() ||
        !object.value(QStringLiteral("excludedArtists")).isArray())
        return QStringLiteral("AI 返回的筛选条件格式无效。");
    const auto artists = object.value(QStringLiteral("excludedArtists")).toArray();
    if (artists.size() > 20)
        return QStringLiteral("AI 返回的筛选条件过多，请缩短指令。");
    QSet<QString> seen;
    QStringList names;
    for (const auto &value : artists)
    {
        if (!value.isString() || value.toString().trimmed().isEmpty() ||
            value.toString().size() > 200)
            return QStringLiteral("AI 返回的筛选条件文本无效。");
        const QString text = value.toString().trimmed();
        if (seen.contains(text.toCaseFolded()))
            return QStringLiteral("AI 返回了重复条件，请重试。");
        seen.insert(text.toCaseFolded());
        names.append(text);
    }
    request->minutes = minutes.toInt();
    request->scene = scene.toString();
    request->source = source.toString();
    request->excludeLive = object.value(QStringLiteral("excludeLive")).toBool();
    request->instrumentalOnly = object.value(QStringLiteral("instrumentalOnly")).toBool();
    request->excludedArtists = names;
    return {};
}

QString readinessError(AppBackendClient *backend)
{
    if (!AppBackendClient::enabled())
        return QStringLiteral("AI 服务尚未配置，请联系应用维护者。");
    const auto configurationError = AppBackendClient::configurationError();
    if (!configurationError.isEmpty())
        return configurationError;
    if (!backend->authenticated())
        return QStringLiteral("请先登录探歌账户，再使用 AI 专注服务。");
    if (!backend->agentReady())
        return QStringLiteral(
            "AI 服务尚未就绪，请稍后重试或联系应用维护者。当前仍可使用本地表单。");
    return {};
}
} // namespace

FocusModelClient::FocusModelClient(QObject *parent, int timeout, AppBackendClient *client)
    : QObject(parent), backend(client ? client : AppBackendClient::instance()),
      timeoutMs(qBound(1, timeout, 15000))
{
    connect(backend, &AppBackendClient::authChanged, this,
            [this](const QString &, const QString &) { cancel(); });
}
FocusModelClient::~FocusModelClient() { cancel(); }
bool FocusModelClient::configured() { return configurationError().isEmpty(); }
QString FocusModelClient::configurationError()
{
    return readinessError(AppBackendClient::instance());
}
void FocusModelClient::cancel()
{
    ++generation;
    if (pending)
        pending->abort();
    pending.clear();
}

void FocusModelClient::parse(const QString &text, const FocusRequest &base, QObject *context,
                             Callback callback)
{
    cancel();
    if (!context || !callback)
        return;
    const auto configError = readinessError(backend);
    if (!configError.isEmpty())
    {
        callback(base, configError);
        return;
    }
    if (text.trimmed().isEmpty() || text.size() > 2000)
    {
        callback(base, tr("请输入 1 至 2000 字的听歌要求。"));
        return;
    }
    FocusRequest checked;
    const auto baseError = validate(requestObject(base), &checked);
    if (!baseError.isEmpty())
    {
        callback(base, tr("当前表单设置无效：%1").arg(baseError));
        return;
    }
    const QJsonObject body{{QStringLiteral("instruction"), text},
                           {QStringLiteral("currentRequest"), requestObject(base)}};
    const auto currentGeneration = generation;
    const QPointer<QObject> guard(context);
    const QPointer<FocusModelClient> self(this);
    pending = backend->parseFocus(
        body, context,
        [self, guard, callback, base, text, currentGeneration](const QJsonObject &object,
                                                               const QString &failure)
        {
            if (!self || !guard || currentGeneration != self->generation)
                return;
            self->pending.clear();
            QString error = failure;
            FocusRequest parsed = base;
            if (error.isEmpty())
            {
                if (object.keys() != QStringList{QStringLiteral("request")} ||
                    !object.value(QStringLiteral("request")).isObject())
                    error = QStringLiteral("AI 服务未返回有效的专注计划。");
                else
                    error = validate(object.value(QStringLiteral("request")).toObject(), &parsed);
            }
            if (error.isEmpty())
                parsed.originalText = text;
            callback(error.isEmpty() ? parsed : base, error);
        },
        timeoutMs);
}
