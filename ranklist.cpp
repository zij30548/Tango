#include "ranklist.h"
#include "ui_ranklist.h"
#include "dialogstyle.h"
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QFileDialog>
#include <QStandardPaths>

RankList::RankList(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::RankList)
{
    ui->setupUi(this);
    applyDialogStyle(this);
    setMinimumSize(560, 380);

    // 设置表头
    ui->tableWidget_RankList->setColumnCount(4);
    QStringList headers;
    headers << "排名" << "歌曲" << "作者" << "时长";
    ui->tableWidget_RankList->setHorizontalHeaderLabels(headers);
    ui->tableWidget_RankList->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->tableWidget_RankList->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->tableWidget_RankList->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    // 插入数据
    struct Song {
        int rank;
        QString name;
        QString artist;
        QString duration;
    };

    QList<Song> songs = {
                         {1, "如果爱忘了（live）", "汪苏泷", "04:05"},
                         {2, "还是会想你", "林达浪", "03:10"},
                         {3, "5：20AM", "刀酱", "02:41"},
                         {4, "如果可以", "韦礼安", "04:34"},
                         {5, "在加纳共和国离婚", "杨坤", "04:33"},
                         {6, "紫荆花盛开", "李荣浩", "03:30"},
                         {7, "无名的人", "毛不易", "04:42"},
                         {8, "命运", "汪苏泷", "04:18"},
                         {9, "离别开出花", "就是南方凯", "03:50"},
                         {10, "悬溺", "葛东琪", "03:17"},
                         };

    ui->tableWidget_RankList->setRowCount(songs.size());

    for (int i = 0; i < songs.size(); ++i) {
        QTableWidgetItem *item0 = new QTableWidgetItem(QString::number(songs[i].rank));
        item0->setFlags(item0->flags() & ~Qt::ItemIsEditable);
        ui->tableWidget_RankList->setItem(i, 0, item0);

        QTableWidgetItem *item1 = new QTableWidgetItem(songs[i].name);
        item1->setFlags(item1->flags() & ~Qt::ItemIsEditable);
        ui->tableWidget_RankList->setItem(i, 1, item1);

        QTableWidgetItem *item2 = new QTableWidgetItem(songs[i].artist);
        item2->setFlags(item2->flags() & ~Qt::ItemIsEditable);
        ui->tableWidget_RankList->setItem(i, 2, item2);

        QTableWidgetItem *item3 = new QTableWidgetItem(songs[i].duration);
        item3->setFlags(item3->flags() & ~Qt::ItemIsEditable);
        ui->tableWidget_RankList->setItem(i, 3, item3);
    }

    // 设置右键菜单
    ui->tableWidget_RankList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->tableWidget_RankList, &QTableWidget::customContextMenuRequested, this, &RankList::onCustomContextMenuRequested);

    contextMenu = new QMenu(this);
    playMVAction = new QAction("播放MV", this);
    contextMenu->addAction(playMVAction);
    connect(playMVAction, &QAction::triggered, this, &RankList::playMV);

    connect(ui->tableWidget_RankList, &QTableWidget::cellDoubleClicked, this, &RankList::onSongDoubleClicked);
}

RankList::~RankList()
{
    delete ui;
}

void RankList::updateRankList(int type)
{
    // Allocate the optional network service only when it is requested.
    if (!manager) {
        manager = new QNetworkAccessManager(this);
        connect(manager, &QNetworkAccessManager::finished, this, &RankList::onNetworkReply);
    }
    const QUrl url(QString("http://127.0.0.1:3000/top/song?type=%1").arg(type));
    QNetworkRequest request(url);
    request.setTransferTimeout(10000);
    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > 2 * 1024 * 1024) reply->abort();
    });
}

void RankList::on_pushButton_Back_clicked()
{
    close();
}

void RankList::onNetworkReply(QNetworkReply *reply) {
    if (reply->error() == QNetworkReply::NoError) {
        QByteArray responseData = reply->readAll();
        QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);
        QJsonArray tracks = jsonDoc.object().value("data").toArray();

        const int count = qMin(tracks.size(), 100);
        if (!count) {
            reply->deleteLater();
            return;
        }
        ui->tableWidget_RankList->setUpdatesEnabled(false);
        ui->tableWidget_RankList->setRowCount(count);
        for (int i = 0; i < count; ++i) {
            const QJsonObject track = tracks.at(i).toObject();
            const QJsonArray artists = track.value("artists").toArray();
            const QString artist = artists.isEmpty() ? QString() : artists.first().toObject().value("name").toString();
            const int seconds = track.value("duration").toInt() / 1000;
            const QStringList values = {QString::number(i + 1), track.value("name").toString(), artist,
                QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'))};
            for (int column = 0; column < values.size(); ++column)
                ui->tableWidget_RankList->setItem(i, column, new QTableWidgetItem(values.at(column)));
        }
        ui->tableWidget_RankList->setUpdatesEnabled(true);
        ui->groupBox_RankList->setTitle(tr("本机服务榜单 · 最多 100 首"));

    } else {
        qDebug() << "Network error:" << reply->errorString();
    }
    reply->deleteLater();
}

void RankList::onSongDoubleClicked(int row, int column) {
    Q_UNUSED(column);
    const auto *item = ui->tableWidget_RankList->item(row, 1);
    if (!item) return;
    const QString songName = item->text();
    emit songSelected1(songName);
    this->close();
}

void RankList::onCustomContextMenuRequested(const QPoint &pos) {
    const QModelIndex index = ui->tableWidget_RankList->indexAt(pos);
    if (!index.isValid()) return;
    ui->tableWidget_RankList->selectRow(index.row());
    contextMenu->exec(ui->tableWidget_RankList->viewport()->mapToGlobal(pos));
}

void RankList::playMV()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("选择本地 MV"),
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
        tr("视频文件 (*.mp4 *.mov *.m4v *.mkv);;所有文件 (*)"));
    if (!path.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        QMessageBox::warning(this, tr("无法打开视频"), tr("请检查视频文件和默认播放器。"));
}
