#include "signup.h"
#include "appbackendclient.h"
#include "dialogstyle.h"
#include "passwordvisibility.h"
#include "ui_signup.h"
#include <QCryptographicHash>
#include <QLabel>
#include <QShowEvent>
#include <QHideEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QMessageBox>

namespace
{
bool incrementCounter(redisContext *context, const char *key)
{
    RedisReplyPtr reply(static_cast<redisReply *>(redisCommand(context, "INCR %s", key)),
                        &freeReplyObject);
    return reply && reply->type == REDIS_REPLY_INTEGER;
}

qint64 readCounter(redisContext *context, const char *key, bool *valid)
{
    RedisReplyPtr reply(static_cast<redisReply *>(redisCommand(context, "GET %s", key)),
                        &freeReplyObject);
    if (reply && reply->type == REDIS_REPLY_NIL)
    {
        *valid = true;
        return 0;
    }
    if (!reply || reply->type != REDIS_REPLY_STRING || !reply->str)
    {
        *valid = false;
        return 0;
    }
    return QByteArray(reply->str, static_cast<int>(reply->len)).toLongLong(valid);
}
} // namespace

SignUp::SignUp(QWidget *parent) : QDialog(parent), ui(new Ui::SignUp)
{
    ui->setupUi(this);
    applyDialogStyle(this);
    passwordVisibility = installPasswordVisibility(ui->lineEdit_Password,
        QStringLiteral("action_PasswordVisibility"), QStringLiteral("密码"));
    confirmPasswordVisibility = installPasswordVisibility(ui->lineEdit_ConfirmPassword,
        QStringLiteral("action_ConfirmPasswordVisibility"), QStringLiteral("确认密码"));
    ui->lineEdit_Password->setMaxLength(128);
    ui->lineEdit_ConfirmPassword->setMaxLength(128);
    ui->lineEdit_ConfirmPassword->setAccessibleName(QStringLiteral("再次输入注册密码"));
    ui->lineEdit_InviteCode->setMaxLength(128);
    ui->lineEdit_InviteCode->setAccessibleName(QStringLiteral("内测邀请代码"));
    ui->label_InviteCode->hide();
    ui->lineEdit_InviteCode->hide();
    setMinimumSize(440, 400);
    setWindowTitle(QStringLiteral("注册探歌"));
    ui->label_3->setText(QStringLiteral("创建探歌账户"));
    const bool hosted = AppBackendClient::enabled();
    ui->label_AccountHint->setText(
        hosted ? QStringLiteral("注册后即可使用探歌。网易云音乐请通过「连接网易云」单独授权。")
               : QStringLiteral("账户保存在本机，网易云音乐授权单独连接。"));
    ui->label_AccountHint->setWordWrap(true);
    if (hosted)
    {
        ui->label_4->hide();
        ui->radioButton_man->hide();
        ui->radioButton_woman->hide();
        ui->lineEdit_Password->setPlaceholderText(QStringLiteral("设置密码，正在读取长度要求…"));
    }
    authStatus = new QLabel(this);
    authStatus->setObjectName(QStringLiteral("label_AuthStatus"));
    authStatus->setWordWrap(true);
    authStatus->setStyleSheet(QStringLiteral("color: #e9b5a9;"));
    authStatus->hide();
    ui->rootLayout->insertWidget(ui->rootLayout->count() - 1, authStatus);
    retryPolicy = new QPushButton(QStringLiteral("重新读取注册方式"), this);
    retryPolicy->setObjectName(QStringLiteral("pushButton_RetryRegistrationPolicy"));
    retryPolicy->setAutoDefault(false);
    retryPolicy->hide();
    ui->rootLayout->insertWidget(ui->rootLayout->count() - 1, retryPolicy);
    connect(retryPolicy, &QPushButton::clicked, this, &SignUp::loadRegistrationPolicy);
    ui->lineEdit_Id->setFocus();
    QButtonGroup *genderGroup = new QButtonGroup(this);
    genderGroup->addButton(ui->radioButton_man, 0);
    genderGroup->addButton(ui->radioButton_woman, 1);
    connect(ui->lineEdit_ConfirmPassword, &QLineEdit::returnPressed, this,
            &SignUp::on_pushButton_SignUp_clicked);
    connect(ui->lineEdit_InviteCode, &QLineEdit::returnPressed, this,
            &SignUp::on_pushButton_SignUp_clicked);
    if (hosted) loadRegistrationPolicy();
}

SignUp::~SignUp()
{
    ++requestGeneration;
    cancelRegistrationPolicy();
    delete requestContext;
    delete ui;
}

void SignUp::done(int result)
{
    cancelRegistrationPolicy();
    ++requestGeneration;
    delete requestContext;
    requestContext = nullptr;
    setBusy(false);
    concealPassword(ui->lineEdit_Password, passwordVisibility);
    ui->lineEdit_Password->clear();
    concealPassword(ui->lineEdit_ConfirmPassword, confirmPasswordVisibility);
    ui->lineEdit_ConfirmPassword->clear();
    ui->lineEdit_InviteCode->clear();
    QDialog::done(result);
}

void SignUp::showEvent(QShowEvent *event)
{
    concealPassword(ui->lineEdit_Password, passwordVisibility);
    concealPassword(ui->lineEdit_ConfirmPassword, confirmPasswordVisibility);
    QDialog::showEvent(event);
    if (AppBackendClient::enabled() && !policyLoading && !requestPending) loadRegistrationPolicy();
}

void SignUp::hideEvent(QHideEvent *event)
{
    concealPassword(ui->lineEdit_Password, passwordVisibility);
    concealPassword(ui->lineEdit_ConfirmPassword, confirmPasswordVisibility);
    cancelRegistrationPolicy();
    QDialog::hideEvent(event);
}

void SignUp::setBusy(bool busy)
{
    requestPending = busy;
    ui->lineEdit_Id->setEnabled(!busy);
    ui->lineEdit_Password->setEnabled(!busy);
    passwordVisibility->setEnabled(!busy);
    ui->lineEdit_ConfirmPassword->setEnabled(!busy);
    confirmPasswordVisibility->setEnabled(!busy);
    ui->lineEdit_InviteCode->setEnabled(!busy);
    updateRegistrationControls();
}

void SignUp::cancelRegistrationPolicy()
{
    ++policyGeneration;
    delete policyContext;
    policyContext = nullptr;
    policyLoading = false;
    policyReady = false;
}

void SignUp::updateRegistrationControls()
{
    const bool hosted = AppBackendClient::enabled();
    ui->pushButton_SignUp->setEnabled(!requestPending &&
        (!hosted || (policyReady && registrationMode != QStringLiteral("closed"))));
    ui->pushButton_SignUp->setText(requestPending ? QStringLiteral("注册中…") : policyLoading
        ? QStringLiteral("读取注册方式…") : hosted && policyReady && registrationMode == QStringLiteral("closed")
        ? QStringLiteral("暂未开放注册") : QStringLiteral("注册"));
    const bool requiresInvite = hosted && policyReady && registrationMode == QStringLiteral("invite_only");
    ui->label_InviteCode->setVisible(requiresInvite);
    ui->lineEdit_InviteCode->setVisible(requiresInvite);
    retryPolicy->setVisible(hosted && !policyReady && !policyLoading && !requestPending);
}

void SignUp::loadRegistrationPolicy()
{
    if (!AppBackendClient::enabled() || requestPending) return;
    cancelRegistrationPolicy();
    ui->lineEdit_InviteCode->clear();
    policyLoading = true;
    showBackendError({});
    ui->label_AccountHint->setText(QStringLiteral("正在读取当前注册方式…"));
    updateRegistrationControls();
    const quint64 generation = policyGeneration;
    policyContext = new QObject(this);
    auto *backend = AppBackendClient::instance();
    const QString identity = backend->serviceIdentity();
    backend->registrationConfig(policyContext,
        [this, generation, identity](const QJsonObject &config, const QString &error) {
            if (generation != policyGeneration) return;
            policyLoading = false;
            if (!error.isEmpty()) {
                policyReady = false;
                ui->label_AccountHint->setText(QStringLiteral("暂时无法读取注册方式，已有账户可返回登录。"));
                showBackendError(error);
                updateRegistrationControls();
                return;
            }
            policyReady = true;
            policyServiceIdentity = identity;
            registrationMode = config.value(QStringLiteral("registrationMode")).toString();
            passwordMinimum = config.value(QStringLiteral("passwordMin")).toInt();
            passwordMaximum = config.value(QStringLiteral("passwordMax")).toInt();
            usernameMinimum = config.value(QStringLiteral("usernameMin")).toInt();
            usernameMaximum = config.value(QStringLiteral("usernameMax")).toInt();
            ui->lineEdit_Password->setMaxLength(passwordMaximum);
            ui->lineEdit_ConfirmPassword->setMaxLength(passwordMaximum);
            ui->lineEdit_Id->setMaxLength(usernameMaximum);
            ui->lineEdit_Password->setPlaceholderText(QStringLiteral("设置 %1–%2 位密码").arg(passwordMinimum).arg(passwordMaximum));
            ui->label_AccountHint->setText(registrationMode == QStringLiteral("closed")
                ? QStringLiteral("当前暂未开放新账户注册，已有账户可返回登录。")
                : registrationMode == QStringLiteral("invite_only")
                ? QStringLiteral("内测期间仅向受邀用户开放注册，请填写内测码。网易云音乐授权单独连接。")
                : QStringLiteral("创建你的探歌账户。网易云音乐请通过「连接网易云」单独授权。"));
            updateRegistrationControls();
        });
}

void SignUp::showBackendError(const QString &error)
{
    authStatus->setText(error);
    authStatus->setVisible(!error.isEmpty());
}

void SignUp::on_pushButton_Back_clicked() { close(); }

void SignUp::on_pushButton_SignUp_clicked()
{
    if (requestPending)
        return;
    const QString username = ui->lineEdit_Id->text().trimmed();
    const QString password = ui->lineEdit_Password->text();
    if (password != ui->lineEdit_ConfirmPassword->text()) {
        showBackendError(QStringLiteral("两次输入的密码不一致，请重新确认。"));
        ui->lineEdit_ConfirmPassword->setFocus();
        return;
    }
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
        if (!policyReady || policyServiceIdentity != AppBackendClient::instance()->serviceIdentity()) {
            loadRegistrationPolicy();
            showBackendError(QStringLiteral("请先读取当前注册方式，再提交注册。"));
            return;
        }
        if (registrationMode == QStringLiteral("closed")) {
            showBackendError(QStringLiteral("当前暂未开放注册。"));
            return;
        }
        if (username.size() < usernameMinimum || username.size() > usernameMaximum ||
            password.size() < passwordMinimum || password.size() > passwordMaximum) {
            showBackendError(QStringLiteral("用户名需 %1–%2 位，密码需 %3–%4 位。")
                .arg(usernameMinimum).arg(usernameMaximum).arg(passwordMinimum).arg(passwordMaximum));
            return;
        }
        const QString inviteCode = registrationMode == QStringLiteral("invite_only")
            ? ui->lineEdit_InviteCode->text().trimmed() : QString();
        static const QRegularExpression invitePattern(QStringLiteral("^TANGO-[A-Za-z0-9_-]{32}$"));
        if (registrationMode == QStringLiteral("invite_only") && !invitePattern.match(inviteCode).hasMatch()) {
            showBackendError(QStringLiteral("请输入有效的内测码。"));
            ui->lineEdit_InviteCode->setFocus();
            return;
        }
        setBusy(true);
        concealPassword(ui->lineEdit_Password, passwordVisibility);
        concealPassword(ui->lineEdit_ConfirmPassword, confirmPasswordVisibility);
        ui->lineEdit_Password->clear();
        ui->lineEdit_ConfirmPassword->clear();
        ui->lineEdit_InviteCode->clear();
        const quint64 generation = ++requestGeneration;
        delete requestContext;
        requestContext = new QObject(this);
        AppBackendClient::instance()->registerUser(
            username, password, inviteCode, requestContext,
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
                    showBackendError(QStringLiteral("注册未完成，请重试。"));
                    return;
                }
                accept();
            });
        return;
    }
    if (username.isEmpty() || password.isEmpty())
    {
        QMessageBox::warning(this, "注册失败", "用户名或密码不能为空");
        return;
    }

    QString gender;
    if (ui->radioButton_man->isChecked())
    {
        gender = "男";
    }
    else if (ui->radioButton_woman->isChecked())
    {
        gender = "女";
    }
    else
    {
        QMessageBox::warning(this, "注册失败", "请选择性别");
        return;
    }

    QString error;
    RedisContextPtr context(connectLocalRedis(&error), &redisFree);
    if (!context)
    {
        QMessageBox::warning(this, "注册失败", error);
        return;
    }

    const QByteArray usernameData = username.toUtf8();
    RedisReplyPtr existsReply(static_cast<redisReply *>(redisCommand(
                                  context.get(), "EXISTS user:%b", usernameData.constData(),
                                  static_cast<size_t>(usernameData.size()))),
                              &freeReplyObject);
    if (!existsReply || existsReply->type != REDIS_REPLY_INTEGER)
    {
        QMessageBox::warning(this, "注册失败", "检查账户失败，请检查账户服务后重试。");
        return;
    }
    if (existsReply->integer > 0)
    {
        QMessageBox::warning(this, "注册失败", "用户名已存在");
        return;
    }

    const QByteArray hashedPassword =
        QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QByteArray genderData = gender.toUtf8();
    RedisReplyPtr saveReply(
        static_cast<redisReply *>(
            redisCommand(context.get(), "HMSET user:%b username %b password %b gender %b",
                         usernameData.constData(), static_cast<size_t>(usernameData.size()),
                         usernameData.constData(), static_cast<size_t>(usernameData.size()),
                         hashedPassword.constData(), static_cast<size_t>(hashedPassword.size()),
                         genderData.constData(), static_cast<size_t>(genderData.size()))),
        &freeReplyObject);
    if (!saveReply || saveReply->type != REDIS_REPLY_STATUS || !saveReply->str ||
        QByteArray(saveReply->str, static_cast<int>(saveReply->len)) != "OK")
    {
        QMessageBox::warning(this, "注册失败", "存储账户失败，请检查账户服务后重试。");
        return;
    }

    const bool totalUpdated = incrementCounter(context.get(), "total_num");
    const bool genderUpdated =
        incrementCounter(context.get(), gender == "男" ? "man_num" : "woman_num");
    bool manCountValid = false;
    bool womanCountValid = false;
    const qint64 manCount = readCounter(context.get(), "man_num", &manCountValid);
    const qint64 womanCount = readCounter(context.get(), "woman_num", &womanCountValid);

    QString message = "欢迎 " + username;
    if (totalUpdated && genderUpdated && manCountValid && womanCountValid)
    {
        message += QString("\n男性用户数: %1\n女性用户数: %2").arg(manCount).arg(womanCount);
    }
    else
    {
        message += "\n账户已创建，用户统计暂不可用。";
    }
    QMessageBox::information(this, "注册成功", message);
    accept();
}
