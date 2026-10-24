#ifndef LIKESONG_H
#define LIKESONG_H

#include <QDialog>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <QDebug>
#include <QSqlError>
#include <QMenu>
#include <QAction>

namespace Ui {
class LikeSong;
}

class LikeSong : public QDialog
{
    Q_OBJECT

public:
    explicit LikeSong(QWidget *parent = nullptr);
    ~LikeSong();

    void loadFavoriteSongs();
    void showContextMenu(const QPoint &pos);

signals:
    void songSelected(const QString &songName);

private slots:
    void on_pushButton_Back_clicked();
    void onRemoveSongTriggered();
    void onSongDoubleClicked(int row, int column);
    void on_pushButton_PreviousPage_clicked();
    void on_pushButton_NextPage_clicked();

private:
    Ui::LikeSong *ui;
    static constexpr int pageSize = 500;
    qint64 currentPage = 0;
    qint64 totalPages = 1;
};

#endif // LIKESONG_H
