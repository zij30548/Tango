#ifndef RANKLIST_H
#define RANKLIST_H

#include <QDialog>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QTableWidgetItem>
#include <QAction>
#include <QMenu>

namespace Ui {
class RankList;
}

class RankList : public QDialog
{
    Q_OBJECT

public:
    explicit RankList(QWidget *parent = nullptr);
    ~RankList();



    void updateRankList(int type); // 修改为接受 type 参数

signals:
    void songSelected1(const QString &songName);
private slots:
    void on_pushButton_Back_clicked();
    void onNetworkReply(QNetworkReply *reply);
    void onSongDoubleClicked(int row, int column);
    void onCustomContextMenuRequested(const QPoint &pos);
    void playMV();

private:
    Ui::RankList *ui;
    QNetworkAccessManager *manager = nullptr;
    QMenu *contextMenu;
    QAction *playMVAction;
};

#endif // RANKLIST_H
