#include "historysong.h"
#include "ui_historysong.h"
#include "dialogstyle.h"
#include "songlistdelegate.h"
#include <QDebug>
#include <QHeaderView>
#include <QMessageBox>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QUrl>

HistorySong::HistorySong(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::HistorySong)
{
    ui->setupUi(this);
    applyDialogStyle(this);
    setMinimumSize(680, 440);
    auto *table = ui->table_HistorySong;
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    table->setColumnWidth(1, 154);
    table->setColumnWidth(2, 84);
    table->verticalHeader()->setDefaultSectionSize(72);
    table->setAlternatingRowColors(false);
    SongListDelegate::install(table, 0);
    connect(table, &QTableWidget::cellDoubleClicked, this, &HistorySong::onSongDoubleClicked);
    HistorySong::DisplayHistory();
}

HistorySong::~HistorySong()
{
    delete ui;
}

void HistorySong::DisplayHistory()
{
    auto *table = ui->table_HistorySong;
    QSqlQuery count;
    if (!count.exec("SELECT COUNT(*) FROM play_history") || !count.next()) {
        table->setRowCount(0);
        ui->label_EmptyHistory->setText(tr("暂时无法读取播放历史"));
        ui->label_EmptyHistory->show();
        ui->pushButton_DeleteHistory->setEnabled(false);
        ui->pushButton_PreviousPage->setEnabled(false);
        ui->pushButton_NextPage->setEnabled(false);
        ui->label_Page->setText(tr("无法读取页码"));
        qWarning() << "Could not count play history:" << count.lastError();
        return;
    }
    const qint64 total = count.value(0).toLongLong();
    totalPages = total > 0 ? (total - 1) / pageSize + 1 : 1;
    currentPage = qBound<qint64>(0, currentPage, totalPages - 1);
    QSqlQuery query;
    query.setForwardOnly(true);
    query.prepare("SELECT song_id, song_name, artist_name, media_url, play_time "
                  "FROM play_history ORDER BY id DESC LIMIT ? OFFSET ?");
    query.addBindValue(pageSize);
    query.addBindValue(currentPage * pageSize);
    if (!query.exec()) {
        table->setRowCount(0);
        ui->label_EmptyHistory->setText(tr("暂时无法读取播放历史"));
        ui->label_EmptyHistory->show();
        ui->pushButton_DeleteHistory->setEnabled(false);
        ui->pushButton_PreviousPage->setEnabled(false);
        ui->pushButton_NextPage->setEnabled(false);
        ui->label_Page->setText(tr("无法读取页码"));
        qWarning() << "Could not load play history:" << query.lastError();
        return;
    }
    table->setUpdatesEnabled(false);
    table->setRowCount(0);
    while (query.next()) {
        const int row = table->rowCount();
        table->insertRow(row);
        const QUrl media(query.value(3).toString());
        auto *title = new QTableWidgetItem(query.value(1).toString());
        title->setData(SongListDelegate::ArtistRole, query.value(2).toString());
        title->setData(SongListDelegate::MediaUrlRole, media);
        title->setData(SongListDelegate::SongIdRole, query.value(0).toLongLong());
        title->setData(Qt::UserRole, query.value(3).toString());
        title->setToolTip(query.value(1).toString() + "\n" + query.value(2).toString());
        table->setItem(row, 0, title);
        auto *time = new QTableWidgetItem(query.value(4).toString());
        time->setForeground(QColor("#8b9890"));
        table->setItem(row, 1, time);
        auto *source = new QTableWidgetItem(media.isLocalFile() ? tr("本地") : tr("网易云"));
        source->setForeground(QColor("#8b9890"));
        table->setItem(row, 2, source);
    }
    table->setUpdatesEnabled(true);
    ui->label_EmptyHistory->setText(tr("还没有播放记录。添加歌曲，开始聆听。"));
    ui->label_EmptyHistory->setVisible(total == 0);
    ui->pushButton_DeleteHistory->setEnabled(total > 0);
    ui->label_Page->setText(tr("第 %1 / %2 页 · 共 %3 条").arg(currentPage + 1).arg(totalPages).arg(total));
    ui->pushButton_PreviousPage->setEnabled(currentPage > 0);
    ui->pushButton_NextPage->setEnabled(currentPage + 1 < totalPages);
}

void HistorySong::onSongDoubleClicked(int row, int column)
{
    Q_UNUSED(column);
    const auto *item = ui->table_HistorySong->item(row, 0);
    if (!item)
        return;
    const QString source = item->data(Qt::UserRole).toString();
    if (source.isEmpty())
        return;
    emit songSelected(source);
    if(isWindow())accept();
}

void HistorySong::on_pushButton_PreviousPage_clicked()
{
    if (currentPage <= 0) return;
    --currentPage;
    DisplayHistory();
}

void HistorySong::on_pushButton_NextPage_clicked()
{
    if (currentPage + 1 >= totalPages) return;
    ++currentPage;
    DisplayHistory();
}

void HistorySong::on_pushButton_Back_clicked()
{
    this->close();
}

void HistorySong::clearHistory()
{
    QSqlQuery query;
    if (query.exec("DELETE FROM play_history")) {
        currentPage = 0;
        DisplayHistory();
        emit historyCleared();
    }
    else QMessageBox::warning(this, tr("清空失败"), query.lastError().text());
}

void HistorySong::on_pushButton_DeleteHistory_clicked()
{
    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(this, "删除历史记录", "确定要删除所有历史记录吗？", QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        clearHistory();
    }
}
