#include "signin.h"
#include "appbackendclient.h"
#include "dialogstyle.h"
#include "passwordvisibility.h"
#include "ui_signin.h"
#include <QLabel>
#include <QShowEvent>
#include <QHideEvent>

SignIn::SignIn(QWidget *parent) : QDialog(parent), ui(new Ui::SignIn)
{
    ui->setupUi(this);
    applyDialogStyle(this);
    passwordVisibility = installPasswordVisibility(ui->lineEdit_Password,
        QStringLiteral("action_PasswordVisibility"), QStringLiteral("密码"));
    setMinimumSize(420, 300);
    setWindowTitle(QStringLiteral("登录探歌"));
    ui->label_3->setText(QStringLiteral("登录探歌"));
    ui->label_AccountHint->setText(
        AppBackendClient::enabled()
            ? QStringLiteral("登录你的探歌账户。网易云音乐请通过「连接网易云」单独授权。")
            : QStringLiteral("使用本机探歌账户。网易云音乐请通过「连接网易云」单独授权。"));
    ui->label_AccountHint->setWordWrap(true);
    authStatus = new QLabel(this);
    authStatus->setObjectName(QStringLiteral("label_AuthStatus"));
    authStatus->setWordWrap(true);
    authStatus->setStyleSheet(QStringLiteral("color: #e9b5a9;"));
    authStatus->hide();
    ui->rootLayout->insertWidget(ui->rootLayout->count() - 1, authStatus);
    ui->lineEdit_Id->setFocus();
    connect(ui->lineEdit_Password, &QLineEdit::returnPressed, this,
            &SignIn::on_pushButton_SignIn_clicked);
}

SignIn::~SignIn()
{
    ++requestGeneration;
    delete requestContext;
    delete ui;
}

void SignIn::done(int result)
{
    ++requestGeneration;
    delete requestContext;
    requestContext = nullptr;
    setBusy(false);
    concealPassword(ui->lineEdit_Password, passwordVisibility);
    ui->lineEdit_Password->clear();
    QDialog::done(result);
}

void SignIn::showEvent(QShowEvent *event)
{
    concealPassword(ui->lineEdit_Password, passwordVisibility);
    QDialog::showEvent(event);
}

void SignIn::hideEvent(QHideEvent *event)
{
    concealPassword(ui->lineEdit_Password, passwordVisibility);
    QDialog::hideEvent(event);
}

void SignIn::setBusy(bool busy)
{
    requestPending = busy;
    ui->lineEdit_Id->setEnabled(!busy);
    ui->lineEdit_Password->setEnabled(!busy);
    passwordVisibility->setEnabled(!busy);
    ui->pushButton_SignUp->setEnabled(!busy);
    ui->pushButton_SignIn->setEnabled(!busy);
    ui->pushButton_SignIn->setText(busy ? QStringLiteral("登录中…") : QStringLiteral("登录"));
}

void SignIn::showBackendError(const QString &error)
{
    authStatus->setText(error);
    authStatus->setVisible(!error.isEmpty());
}

void SignIn::on_pushButton_Back_clicked() { close(); }

void SignIn::on_pushButton_SignUp_clicked()
{
    if (requestPending)
        return;
    SignUp dialog(this);
    if (dialog.exec() == QDialog::Accepted && AppBackendClient::enabled() &&
        AppBackendClient::instance()->authenticated())
    {
        isSignIn = true;
        emit userLoggedIn(AppBackendClient::instance()->username());
        accept();
    }
}

void SignIn::on_pushButton_SignIn_clicked()
{
    if (requestPending)
        return;
    isSignIn = false;
    const QString username = ui->lineEdit_Id->text().trimmed();
    const QString password = ui->lineEdit_Password->text();

    const QString configurationError = AppBackendClient::configurationError();
    if (AppBackendClient::enabled())
    {
        showBackendError({});
        if (!configurationError.isEmpty())
        {
            ui->lineEdit_Password->clear();
            showBackendError(configurationError);
            return;
        }
        if (username.isEmpty() || password.isEmpty())
        {
            showBackendError(QStringLiteral("请输入用户名和密码。"));
            return;
        }
        setBusy(true);
        concealPassword(ui->lineEdit_Password, passwordVisibility);
        ui->lineEdit_Password->clear();
        const quint64 generation = ++requestGeneration;
        delete requestContext;
        requestContext = new QObject(this);
        AppBackendClient::instance()->login(
            username, password, requestContext,
            [this, generation](const QString &error)
            {
                if (generation != requestGeneration)
                    return;
                setBusy(false);
                if (!error.isEmpty())
                {
                    showBackendError(error);
                    ui->lineEdit_Password->setFocus();
                    return;
                }
                if (!AppBackendClient::instance()->authenticated())
                {
                    showBackendError(QStringLiteral("登录未完成，请重试。"));
                    return;
                }
                isSignIn = true;
                emit userLoggedIn(AppBackendClient::instance()->username());
                accept();
            });
        return;
    }

    if (username.isEmpty() || password.isEmpty())
    {
        QMessageBox::warning(this, "登录失败", "用户名或密码不能为空");
        return;
    }

    QString error;
    RedisContextPtr context(connectLocalRedis(&error), &redisFree);
    if (!context)
    {
        QMessageBox::warning(this, "登录失败", error);
        return;
    }

    const QByteArray usernameData = username.toUtf8();
    RedisReplyPtr reply(static_cast<redisReply *>(redisCommand(
                            context.get(), "HGET user:%b password", usernameData.constData(),
                            static_cast<size_t>(usernameData.size()))),
                        &freeReplyObject);
    if (!reply || reply->type == REDIS_REPLY_ERROR)
    {
        QMessageBox::warning(this, "登录失败", "读取账户失败，请检查账户服务后重试。");
        return;
    }
    if (reply->type == REDIS_REPLY_NIL)
    {
        QMessageBox::warning(this, "登录失败", "用户名不存在，请注册用户");
        return;
    }
    if (reply->type != REDIS_REPLY_STRING || !reply->str)
    {
        QMessageBox::warning(this, "登录失败", "账户数据格式异常，请检查账户服务。");
        return;
    }

    const QByteArray hashedPassword =
        QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QByteArray storedPassword(reply->str, static_cast<int>(reply->len));
    if (storedPassword != hashedPassword)
    {
        QMessageBox::warning(this, "登录失败", "密码错误，请重新输入或者注册用户");
        return;
    }

    QMessageBox::information(this, "登录成功", "欢迎回来 " + username);
    isSignIn = true;
    emit userLoggedIn(username);
    accept();
}
