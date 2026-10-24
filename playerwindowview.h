#ifndef PLAYERWINDOWVIEW_H
#define PLAYERWINDOWVIEW_H

#include <QAbstractTableModel>
#include <QIcon>
#include <QWidget>
#include <QUrl>
class QPushButton;
class QLabel;
class QLineEdit;
class QSlider;
class QTableView;
class QMainWindow;
class QStackedWidget;
class LyricsView;

QIcon playerIcon(const QString &name, const QColor &color = QColor("#cbd4cd"));

class QueueModel : public QAbstractTableModel
{
  public:
    explicit QueueModel(QObject *parent = nullptr) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void append(const QString &title, const QString &artist, bool local,
                const QUrl &mediaUrl = QUrl(), qint64 songId = 0);
    void setArtwork(int index, const QString &album, const QString &coverUrl);
    void setCurrentTrack(int index);
    void clear();

  private:
    struct Row
    {
        QString title, artist;
        bool local;
        QUrl mediaUrl;
        qint64 songId = 0;
        QString album, coverUrl;
    };
    QVector<Row> rows;
    int currentTrack = -1;
};

class ArtworkWidget : public QWidget
{
  public:
    explicit ArtworkWidget(QWidget *parent = nullptr);
    void loadCover(const QString &audioPath);
    void loadTrack(const QUrl &mediaUrl, qint64 songId = 0,
                   const QString &coverUrl = QString());

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    QImage cover;
    QUrl currentMedia;
    qint64 currentSongId = 0;
    QString currentCoverUrl;
};

class PlayerWindowView
{
  public:
    void setupUi(QMainWindow *window);
    void addPage(QWidget *page);
    void setPage(QWidget *page);
    void setNowPlaying(const QString &title, const QString &artist, const QUrl &mediaUrl,
                       qint64 songId = 0, const QString &coverUrl = QString());
    QPushButton *pushButton_AddSong, *pushButton_PlaySong, *pushButton_PreviousSong,
        *pushButton_PlayNextSong, *pushButton_SoundYesNo, *pushButton_AddLike, *pushButton_LikeSong,
        *pushButton_HistorySong, *pushButton_SignIn, *pushButton_About, *pushButton_SearchSong,
        *floatingLyricsButton, *importLyricsButton, *repeatButton, *shuffleButton,
        *nowPlayingButton, *moreButton, *neteaseAccountButton, *focusButton, *radioButton,
        *libraryButton, *miniPlayerButton, *queueButton;
    QLineEdit *lineEdit_InputSong;
    QSlider *horizontalSlider_PlayProgress, *horizontalSlider_Volume;
    QLabel *label_DisplaySongTitle, *label_DisplayUser, *artistLabel, *elapsedLabel, *durationLabel,
        *queueCountLabel, *statusLabel, *emptyQueueLabel, *focusStatusLabel,
        *miniTitleLabel, *miniArtistLabel;
    QStackedWidget *pageStack;
    QWidget *libraryPage, *nowPlayingPage, *contentToolbar;
    QTableView *queueView;
    QueueModel *queueModel;
    LyricsView *lyricsView;
    ArtworkWidget *artwork, *miniArtwork;
};
#endif
