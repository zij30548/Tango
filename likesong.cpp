#include "likesong.h"
#include "ui_likesong.h"
#include "dialogstyle.h"
#include "songlistdelegate.h"
#include <QHeaderView>
#include <QUrl>

LikeSong::LikeSong(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::LikeSong)
{
    ui->setupUi(this);
    applyDialogStyle(this);
    setMinimumSize(680, 440);
    resize(820, 590);

    ui->tableWidget_LikeSong->setColumnCount(2);
    QStringList headers = {"来源", "歌曲"};
    ui->tableWidget_LikeSong->setHorizontalHeaderLabels(headers);

    ui->tableWidget_LikeSong->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    ui->tableWidget_LikeSong->setColumnWidth(0, 90);
    ui->tableWidget_LikeSong->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->tableWidget_LikeSong->horizontalHeader()->moveSection(1, 0);
    SongListDelegate::install(ui->tableWidget_LikeSong, 1);
    ui->tableWidget_LikeSong->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableWidget_LikeSong->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->tableWidget_LikeSong->setToolTip(tr("双击播放；右键可移除收藏"));
    LikeSong::loadFavoriteSongs();

    ui->tableWidget_LikeSong->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->tableWidget_LikeSong, &QTableWidget::customContextMenuRequested, this, &LikeSong::showContextMenu);
    connect(ui->tableWidget_LikeSong, &QTableWidget::cellDoubleClicked, this, &LikeSong::onSongDoubleClicked);

}

LikeSong::~LikeSong()
{
    delete ui;
}

void LikeSong::on_pushButton_Back_clicked()
{
    this->close();
}

void LikeSong::loadFavoriteSongs() {
    QSqlQuery count;
    if (!count.exec("SELECT COUNT(*) FROM favorite_songs") || !count.next()) {
        ui->tableWidget_LikeSong->setRowCount(0);
        ui->label_FavoritesHint->setText(tr("暂时无法读取收藏，请稍后重试。"));
        ui->pushButton_PreviousPage->setEnabled(false);
        ui->pushButton_NextPage->setEnabled(false);
        ui->label_Page->setText(tr("无法读取页码"));
        qWarning() << "Could not count favorites:" << count.lastError();
        return;
    }
    const qint64 total = count.value(0).toLongLong();
    totalPages = total > 0 ? (total - 1) / pageSize + 1 : 1;
    currentPage = qBound<qint64>(0, currentPage, totalPages - 1);
    QSqlQuery query;
    query.setForwardOnly(true);
    query.prepare("SELECT song_id, song_name, artist_name, media_url FROM favorite_songs ORDER BY id DESC LIMIT ? OFFSET ?");
    query.addBindValue(pageSize);
    query.addBindValue(currentPage * pageSize);
    query.exec();

    ui->tableWidget_LikeSong->setUpdatesEnabled(false);
    ui->tableWidget_LikeSong->setRowCount(0);

    while (query.next()) {
        int currentRow = ui->tableWidget_LikeSong->rowCount();
        ui->tableWidget_LikeSong->insertRow(currentRow);

        const QUrl mediaUrl(query.value(3).toString());
        QTableWidgetItem *item0 = new QTableWidgetItem(mediaUrl.isLocalFile() ? tr("本地") : tr("在线"));
        item0->setData(Qt::UserRole, query.value(3).toString());
        item0->setToolTip(query.value(3).toString());
        item0->setFlags(item0->flags() & ~Qt::ItemIsEditable);
        ui->tableWidget_LikeSong->setItem(currentRow, 0, item0);

        QTableWidgetItem *item1 = new QTableWidgetItem(query.value(1).toString());
        item1->setData(SongListDelegate::ArtistRole, query.value(2).toString());
        item1->setData(SongListDelegate::MediaUrlRole, mediaUrl);
        item1->setData(SongListDelegate::SongIdRole, query.value(0).toLongLong());
        item1->setToolTip(query.value(1).toString() + QStringLiteral(" · ") + query.value(2).toString());
        item1->setFlags(item1->flags() & ~Qt::ItemIsEditable);
        ui->tableWidget_LikeSong->setItem(currentRow, 1, item1);

    }

    if (query.lastError().isValid()) {
        qDebug() << "Database Error: " << query.lastError();
        ui->label_FavoritesHint->setText(tr("暂时无法读取收藏，请稍后重试。"));
        ui->pushButton_PreviousPage->setEnabled(false);
        ui->pushButton_NextPage->setEnabled(false);
        ui->label_Page->setText(tr("无法读取页码"));
    } else {
        ui->label_FavoritesHint->setText(total > 0 ? tr("%1 首珍藏 · 双击播放，右键移除").arg(total)
                                                   : tr("喜欢的歌曲会出现在这里。播放时点击爱心，收藏你的音乐。"));
        ui->label_Page->setText(tr("第 %1 / %2 页 · 共 %3 首").arg(currentPage + 1).arg(totalPages).arg(total));
        ui->pushButton_PreviousPage->setEnabled(currentPage > 0);
        ui->pushButton_NextPage->setEnabled(currentPage + 1 < totalPages);
    }
    ui->tableWidget_LikeSong->setUpdatesEnabled(true);
}

void LikeSong::on_pushButton_PreviousPage_clicked()
{
    if (currentPage <= 0) return;
    --currentPage;
    loadFavoriteSongs();
}

void LikeSong::on_pushButton_NextPage_clicked()
{
    if (currentPage + 1 >= totalPages) return;
    ++currentPage;
    loadFavoriteSongs();
}

void LikeSong::onRemoveSongTriggered() {
    int currentRow = ui->tableWidget_LikeSong->currentRow();
    if (currentRow >= 0) {
        const auto *item = ui->tableWidget_LikeSong->item(currentRow, 0);
        if (!item) return;
        const QString mediaUrl = item->data(Qt::UserRole).toString();

        QSqlQuery query;
        query.prepare("DELETE FROM favorite_songs WHERE media_url = :mediaUrl");
        query.bindValue(":mediaUrl", mediaUrl);
        if (!query.exec()) {
            qDebug() << "Database Error: " << query.lastError();
        } else {
            loadFavoriteSongs();
        }
    }
}

void LikeSong::showContextMenu(const QPoint &pos) {
    const QModelIndex index = ui->tableWidget_LikeSong->indexAt(pos);
    if (!index.isValid()) return;
    ui->tableWidget_LikeSong->selectRow(index.row());
    QMenu contextMenu(tr("收藏歌曲"), this);

    QAction actionRemove(tr("删除歌曲"), this);
    connect(&actionRemove, &QAction::triggered, this, &LikeSong::onRemoveSongTriggered);
    contextMenu.addAction(&actionRemove);

    contextMenu.exec(ui->tableWidget_LikeSong->viewport()->mapToGlobal(pos));
}

void LikeSong::onSongDoubleClicked(int row, int column) {
    Q_UNUSED(column);
    const auto *item = ui->tableWidget_LikeSong->item(row, 0);
    if (!item) return;
    const QString mediaUrl = item->data(Qt::UserRole).toString();
    if (mediaUrl.isEmpty()) return;
    emit songSelected(mediaUrl);
    if(isWindow())this->close();
}
