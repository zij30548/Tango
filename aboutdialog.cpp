#include "aboutdialog.h"
#include "ui_aboutdialog.h"
#include "dialogstyle.h"
#include "redisconnection.h"
#include <QMessageBox>
#include <QSet>

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::AboutDialog)
{
    ui->setupUi(this);
    applyDialogStyle(this);
    setWindowTitle(tr("本机账户"));
    setMinimumSize(500, 340);
    ui->groupBox_usersList->setTitle(tr("本机账户"));
    ui->label_UserHint->setText(tr("本机账户 · 只读列表"));
    ui->tableWidget_usersList->setColumnCount(2);
    ui->tableWidget_usersList->setHorizontalHeaderLabels({"用户名", "性别"});
    ui->tableWidget_usersList->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->tableWidget_usersList->setContextMenuPolicy(Qt::NoContextMenu);
    ui->tableWidget_usersList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    loadUserData();
}

AboutDialog::~AboutDialog()
{
    delete ui;
}

void AboutDialog::on_pushButton_CloseW_clicked()
{
    close();
}

void AboutDialog::loadUserData()
{
    ui->tableWidget_usersList->setRowCount(0);
    QString error;
    RedisContextPtr context(connectLocalRedis(&error), &redisFree);
    if (!context) {
        QMessageBox::warning(this, "加载失败", error);
        return;
    }

    // Redis SCAN avoids copying the entire keyspace into a single reply.
    // The table remains bounded even when the local account database is large.
    QSet<QByteArray> loadedKeys;
    QByteArray cursor("0");
    do {
        RedisReplyPtr reply(static_cast<redisReply *>(redisCommand(
            context.get(), "SCAN %s MATCH user:* COUNT 100", cursor.constData())), &freeReplyObject);
        if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2
            || !reply->element[0] || reply->element[0]->type != REDIS_REPLY_STRING
            || !reply->element[0]->str || !reply->element[1]
            || reply->element[1]->type != REDIS_REPLY_ARRAY) {
            QMessageBox::warning(this, "加载失败", "无法读取账户列表，请检查账户服务后重试。");
            return;
        }
        cursor = QByteArray(reply->element[0]->str, static_cast<int>(reply->element[0]->len));
        const redisReply *keys = reply->element[1];
        for (size_t i = 0; i < keys->elements && loadedKeys.size() < 500; ++i) {
            const redisReply *key = keys->element[i];
            if (!key || key->type != REDIS_REPLY_STRING || !key->str) continue;
            const QByteArray keyData(key->str, static_cast<int>(key->len));
            if (loadedKeys.contains(keyData)) continue;
            RedisReplyPtr userReply(static_cast<redisReply *>(redisCommand(
                context.get(), "HMGET %b username gender", keyData.constData(),
                static_cast<size_t>(keyData.size()))), &freeReplyObject);
            if (!userReply || userReply->type != REDIS_REPLY_ARRAY || userReply->elements != 2
                || !userReply->element[0] || !userReply->element[1]) continue;
            const redisReply *usernameReply = userReply->element[0];
            const redisReply *genderReply = userReply->element[1];
            if (usernameReply->type != REDIS_REPLY_STRING || !usernameReply->str
                || genderReply->type != REDIS_REPLY_STRING || !genderReply->str) continue;

            loadedKeys.insert(keyData);
            const QString username = QString::fromUtf8(usernameReply->str, static_cast<int>(usernameReply->len));
            const QString gender = QString::fromUtf8(genderReply->str, static_cast<int>(genderReply->len));
            const int row = ui->tableWidget_usersList->rowCount();
            ui->tableWidget_usersList->insertRow(row);
            ui->tableWidget_usersList->setItem(row, 0, new QTableWidgetItem(username));
            ui->tableWidget_usersList->setItem(row, 1, new QTableWidgetItem(gender));
        }
    } while (cursor != "0" && loadedKeys.size() < 500);
    ui->label_UserHint->setText(tr("本机账户 · 只读列表 · 当前显示 %1 个，最多加载 500 个")
        .arg(ui->tableWidget_usersList->rowCount()));

}
