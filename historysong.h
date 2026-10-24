#ifndef HISTORYSONG_H
#define HISTORYSONG_H

#include <QDialog>

namespace Ui {
class HistorySong;
}

class HistorySong : public QDialog
{
    Q_OBJECT

public:
    explicit HistorySong(QWidget *parent = nullptr);
    ~HistorySong();
    void DisplayHistory();

signals:
    void historyCleared();
    void songSelected(const QString &source);

private slots:
    void on_pushButton_Back_clicked();
    void on_pushButton_DeleteHistory_clicked();
    void on_pushButton_PreviousPage_clicked();
    void on_pushButton_NextPage_clicked();
    void onSongDoubleClicked(int row, int column);

private:
    void clearHistory();
    static constexpr int pageSize = 500;
    qint64 currentPage = 0;
    qint64 totalPages = 1;

    Ui::HistorySong *ui;
};

#endif
