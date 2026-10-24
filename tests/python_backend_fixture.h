#ifndef TANGO_PYTHON_BACKEND_FIXTURE_H
#define TANGO_PYTHON_BACKEND_FIXTURE_H

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QProcess>
#include <QTimer>

// 与 Python 本机启动器的 80 次、每次 100ms 等待一致；仅限制测试后台就绪，
// 不调整模型、工具或业务响应的超时，也不降低 PID/运行时/就绪检查。
constexpr int PythonBackendFixtureStartupTimeoutMs = 8000;

// 只探测测试自己启动的本机进程；不能靠启动日志判断就绪，更不能复用真实后台。
inline QJsonObject pythonBackendFixtureGet(quint16 port, const QString &path)
{
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port).arg(path)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto *reply = network.get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout.start(500);
    loop.exec();
    if (reply->error() != QNetworkReply::NoError ||
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
        reply->bytesAvailable() > 4096)
        return {};
    return QJsonDocument::fromJson(reply->readAll()).object();
}

inline bool ownedPythonBackendReady(QProcess &process, quint16 port)
{
    if (process.state() != QProcess::Running)
        return false;
    const auto health = pythonBackendFixtureGet(port, QStringLiteral("/health"));
    if (health.value("app") != QStringLiteral("quiet-app-backend") ||
        health.value("pid").toVariant().toLongLong() != process.processId() ||
        health.value("runtimeLanguage") != QStringLiteral("python") ||
        health.value("storage") != QStringLiteral("sqlite"))
        return false;
    const auto ready = pythonBackendFixtureGet(port, QStringLiteral("/ready"));
    return ready.value("ready").toBool() && ready.value("storage") == QStringLiteral("sqlite");
}

#endif
