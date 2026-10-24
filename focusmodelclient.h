#ifndef FOCUSMODELCLIENT_H
#define FOCUSMODELCLIENT_H

#include "focuscore.h"
#include <QObject>
#include <QPointer>
#include <functional>

class AppBackendClient;
class QNetworkReply;

// Instruction parsing goes through the application's authenticated backend.
// The desktop client never handles a model-provider credential.
class FocusModelClient : public QObject
{
    Q_OBJECT
  public:
    using Callback = std::function<void(const FocusRequest &, const QString &)>;
    explicit FocusModelClient(QObject *parent = nullptr, int timeoutMs = 15000,
                              AppBackendClient *backend = nullptr);
    ~FocusModelClient() override;
    static bool configured();
    static QString configurationError();
    void parse(const QString &text, const FocusRequest &base, QObject *context, Callback callback);
    void cancel();

  private:
    AppBackendClient *backend;
    QPointer<QNetworkReply> pending;
    quint64 generation = 0;
    int timeoutMs;
};

#endif
