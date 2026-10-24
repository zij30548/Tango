#ifndef NETEASEDIALOGS_H
#define NETEASEDIALOGS_H

#include <QDialog>
#include "neteasetypes.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTableWidget;

class NetEaseLoginDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NetEaseLoginDialog(QWidget *parent = nullptr);

    void setAccount(bool loggedIn, const QString &nickname, int vipType);
    void setQrImage(const QString &dataUrl);
    void setStatus(const QString &text);
    void setBusy(bool busy);

signals:
    void refreshQrRequested();
    void logoutRequested();

private:
    QStackedWidget *accountPages;
    QLabel *qrImage;
    QLabel *nicknameLabel;
    QLabel *vipLabel;
    QLabel *statusLabel;
    QProgressBar *progress;
    QPushButton *refreshButton;
    QPushButton *logoutButton;
};

class OnlineSearchDialog : public QDialog
{
    Q_OBJECT

public:
    explicit OnlineSearchDialog(QWidget *parent = nullptr);

    void setResults(const QVector<OnlineSong> &songs, int total, const QString &query);
    void setStatus(const QString &text);
    void setBusy(bool busy);
    static constexpr int PageSize = 30;

signals:
    void songActivated(OnlineSong song);
    void pageRequested(int offset);
    void officialPageRequested(qint64 id);

private:
    void activateSelectedSong();
    void updateControls();
    void requestPage(int offset);

    QVector<OnlineSong> results;
    QString currentQuery;
    int totalResults = 0;
    int currentOffset = 0;
    int requestedOffset = 0;
    bool busy = false;
    QLabel *queryLabel;
    QLabel *statusLabel;
    QLabel *pageLabel;
    QTableWidget *table;
    QPushButton *previousButton;
    QPushButton *nextButton;
    QPushButton *playButton;
    QPushButton *officialButton;
    QProgressBar *progress;
};

#endif // NETEASEDIALOGS_H
