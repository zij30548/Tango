#include "mainwindow.h"
#include "aboutdialog.h"
#include "historysong.h"
#include "likesong.h"
#include "lyricswidget.h"
#include "neteaseclient.h"
#include "neteasedialogs.h"
#include "playerwindowview.h"
#include "ranklist.h"
#include "signin.h"
#include "focusdialog.h"
#include "focusmodelclient.h"
#include "focussession.h"
#include "appbackendclient.h"
#include "radioclient.h"
#include "musictaskclient.h"
#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTableView>
#include <QTextCodec>
#include <QTextEdit>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <climits>
#include <memory>

namespace
{
bool initializeProfileSchema(QSqlDatabase database, QString *error)
{
    error->clear();
    if (!database.transaction()) {
        *error = database.lastError().text();
        return false;
    }
    const QStringList statements = {
        "CREATE TABLE IF NOT EXISTS play_history (id INTEGER PRIMARY KEY AUTOINCREMENT, song_id "
        "INTEGER NOT NULL, song_name TEXT NOT NULL, artist_name TEXT NOT NULL DEFAULT '', "
        "media_url TEXT NOT NULL, play_time TEXT NOT NULL DEFAULT (datetime('now', 'localtime')))",
        "CREATE TABLE IF NOT EXISTS favorite_songs (id INTEGER PRIMARY KEY AUTOINCREMENT, song_id "
        "INTEGER NOT NULL, song_name TEXT NOT NULL, artist_name TEXT NOT NULL DEFAULT '', "
        "media_url TEXT NOT NULL UNIQUE)",
        "CREATE UNIQUE INDEX IF NOT EXISTS favorite_media_url ON favorite_songs(media_url)"};
    QSqlQuery query(database);
    for (const QString &statement : statements) {
        if (!query.exec(statement)) {
            *error = query.lastError().text();
            break;
        }
    }
    FocusStore store(database);
    if (error->isEmpty())
        store.initialize(error);
    if (error->isEmpty() && !database.commit())
        *error = database.lastError().text();
    if (!error->isEmpty())
        database.rollback();
    return error->isEmpty();
}
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new PlayerWindowView)
{
    NeteaseClient::setAccountProfile(currentProfile);
    ui->setupUi(this);
    setAcceptDrops(true);
    auto *moreMenu = new QMenu(this);
    moreMenu->addAction(tr("演示榜单（非实时）"), this,
                        &MainWindow::on_pushButton_RankList_clicked);
    moreMenu->addAction(tr("打开下载目录"), this, &MainWindow::on_pushButton_Download_clicked);
    moreMenu->addAction(tr("在线下载说明"), this, &MainWindow::on_pushButton_Download_2_clicked);
    moreMenu->addAction(tr("停止播放"), this, &MainWindow::on_pushButton_StopPlaySong_clicked);
    moreMenu->addSeparator();
    moreMenu->addAction(tr("在网易云打开当前歌曲"), this, &MainWindow::openOfficialSong);
    cloudLogoutAction = moreMenu->addAction(tr("退出网易云账号"), this, &MainWindow::logoutNetease);
    cloudLogoutAction->setObjectName("actionNeteaseLogout");
    auto *localLogout = moreMenu->addAction(tr("退出探歌账户"), this,
                                           &MainWindow::logoutLocalAccount);
    localLogout->setObjectName("actionLocalLogout");
    ui->moreButton->setMenu(moreMenu);
    connect(ui->neteaseAccountButton, &QPushButton::clicked, this, &MainWindow::openNeteaseAccount);
    initializeDatabase();
    floatingLyrics = new FloatingLyricsWidget(this);
    floatingLyrics->setObjectName("floatingLyrics");
    p_PlayerObject = new QMediaPlayer(this);
    p_PlayerObject->setNotifyInterval(200);
    p_PlayerList = new QMediaPlaylist(this);
    // The playlist stores stable source identities. Only the currently authorized
    // media URL goes into QMediaPlayer; CDN URLs never enter favorites/history.
    p_PlayerObject->setVolume(ui->horizontalSlider_Volume->value());
    p_PlayerList->setPlaybackMode(QMediaPlaylist::Loop);
    connect(p_PlayerList, &QMediaPlaylist::currentIndexChanged, this,
            &MainWindow::updateCurrentSong);
    connect(p_PlayerObject, &QMediaPlayer::stateChanged, this,
            [this](QMediaPlayer::State state)
            {
                ui->pushButton_PlaySong->setIcon(playerIcon(
                    state == QMediaPlayer::PlayingState ? "pause" : "play", QColor("#172711")));
                ui->lyricsView->setPlaying(state == QMediaPlayer::PlayingState);
                floatingLyrics->setPlaying(state == QMediaPlayer::PlayingState);
                if (state == QMediaPlayer::PlayingState &&
                    preparedTrack == p_PlayerList->currentIndex())
                    recordCurrentSong();
                updateFocusPlaybackState();
            });
    connect(p_PlayerObject, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error), this,
            [this](QMediaPlayer::Error error)
            {
                if (error != QMediaPlayer::NoError)
                {
                    const QString reason = p_PlayerObject->errorString();
                    const int index = p_PlayerList->currentIndex();
                    // A backend failure must leave the controller retryable;
                    // Qt can otherwise remain nominally playing/paused while
                    // retaining an unusable media URL.
                    ++mediaGeneration;
                    playbackRequested = false;
                    resolvingTrack = false;
                    preparedTrack = -1;
                    p_PlayerObject->stop();
                    ui->statusLabel->setText(tr("播放失败：%1 · 点击播放重试").arg(reason));
                    if (isOnlineTrack(index))
                        emit onlinePlaybackFailed(tracks.at(index).id, reason);
                    if (index >= 0 && index < tracks.size())
                        failFocusTrack(tracks.at(index).url.toString(QUrl::FullyEncoded));
                }
            });
    connect(p_PlayerObject, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status)
            {
                if (status == QMediaPlayer::EndOfMedia)
                    handleEndOfMedia();
                updateFocusPlaybackState();
            });
    connect(p_PlayerObject, &QMediaPlayer::positionChanged, this,
            &MainWindow::HandleLCDNumerTimeChangeFunc);
    connect(p_PlayerObject, &QMediaPlayer::positionChanged, this,
            &MainWindow::HandlePositionChangeFunc);
    connect(p_PlayerObject, &QMediaPlayer::durationChanged, this,
            &MainWindow::HandleProgressTimeChangeFunc);
    connect(p_PlayerObject, &QMediaPlayer::durationChanged, this,
            &MainWindow::rememberTrackDuration);
    connect(p_PlayerObject, &QMediaPlayer::positionChanged, this,
            [this](qint64 position)
            {
                ui->lyricsView->setPosition(position + lyricsOffset);
                floatingLyrics->setPosition(position + lyricsOffset);
            });
    connect(ui->lyricsView, &LyricsView::seekRequested, this,
            [this](qint64 lyricTime)
            {
                const qint64 position = qMax<qint64>(0, lyricTime - lyricsOffset);
                p_PlayerObject->setPosition(p_PlayerObject->duration() > 0
                                                ? qMin(position, p_PlayerObject->duration())
                                                : position);
            });
    connect(ui->pushButton_AddSong, &QPushButton::clicked, this,
            &MainWindow::on_pushButton_AddSong_clicked);
    connect(ui->pushButton_PlaySong, &QPushButton::clicked, this,
            &MainWindow::on_pushButton_PlaySong_clicked);
    connect(ui->pushButton_PreviousSong, &QPushButton::clicked, this,
            &MainWindow::on_pushButton_PreviousSong_clicked);
    connect(ui->pushButton_PlayNextSong, &QPushButton::clicked, this,
            &MainWindow::on_pushButton_PlayNextSong_clicked);
    connect(ui->pushButton_SoundYesNo, &QPushButton::clicked, this,
            &MainWindow::on_pushButton_SoundYesNo_clicked);
    connect(ui->pushButton_AddLike, &QPushButton::clicked, this,
            &MainWindow::on_pushButton_AddLike_clicked);
    connect(ui->pushButton_LikeSong, &QPushButton::clicked, this,
            &MainWindow::on_pushButton_LikeSong_clicked);
    connect(ui->pushButton_HistorySong, &QPushButton::clicked, this,
            &MainWindow::on_pushButton_HistorySong_clicked);
    connect(ui->pushButton_SignIn, &QPushButton::clicked, this,
            &MainWindow::on_pushButton_SignIn_clicked);
    connect(ui->pushButton_About, &QPushButton::clicked, this,
            &MainWindow::on_pushButton_About_clicked);
    connect(ui->pushButton_SearchSong, &QPushButton::clicked, this,
            &MainWindow::on_pushButton_SearchSong_clicked);
    connect(ui->lineEdit_InputSong, &QLineEdit::returnPressed, this,
            &MainWindow::on_pushButton_SearchSong_clicked);
    connect(ui->horizontalSlider_Volume, &QSlider::valueChanged, this,
            &MainWindow::on_horizontalSlider_Volume_valueChanged);
    connect(ui->horizontalSlider_PlayProgress, &QSlider::valueChanged, this,
            &MainWindow::on_horizontalSlider_PlayProgress_valueChanged);
    connect(ui->queueView, &QTableView::doubleClicked, this,
            [this](const QModelIndex &index) { playTrack(index.row()); });
    connect(ui->floatingLyricsButton, &QPushButton::toggled, this,
            [this](bool visible)
            {
                if (visible && !floatingLyrics->isVisible())
                {
                    const QRect area = screen()->availableGeometry();
                    floatingLyrics->move(area.center().x() - floatingLyrics->width() / 2,
                                         area.bottom() - floatingLyrics->height() - 50);
                }
                floatingLyrics->setVisible(visible);
                if (visible)
                    floatingLyrics->raise();
            });
    connect(floatingLyrics, &FloatingLyricsWidget::visibilityChanged, ui->floatingLyricsButton,
            &QPushButton::setChecked);
    connect(ui->importLyricsButton, &QPushButton::clicked, this, &MainWindow::importLyrics);
    connect(ui->repeatButton, &QPushButton::clicked, this,
            [this]()
            {
                if (ui->shuffleButton->isChecked())
                {
                    ui->shuffleButton->setChecked(false);
                    repeatMode = 0;
                }
                else
                    repeatMode = (repeatMode + 1) % 3;
                updatePlaybackMode();
            });
    connect(ui->shuffleButton, &QPushButton::clicked, this, &MainWindow::updatePlaybackMode);
    connect(ui->nowPlayingButton, &QPushButton::clicked, this,
            [this]()
            {
                showContentPage(ui->nowPlayingPage, ui->nowPlayingButton);
            });
    connect(ui->miniPlayerButton, &QPushButton::clicked, this,
            [this] { showContentPage(ui->nowPlayingPage, ui->nowPlayingButton); });
    connect(ui->libraryButton, &QPushButton::clicked, this,
            [this] { showContentPage(ui->libraryPage, ui->libraryButton); });
    connect(ui->queueButton, &QPushButton::clicked, this,[this] {
        showContentPage(ui->libraryPage, ui->libraryButton);
        ui->queueView->scrollTo(ui->queueModel->index(p_PlayerList->currentIndex(), 0));
    });
    connect(p_PlayerObject, &QMediaPlayer::mutedChanged, this, [this](bool muted)
            { ui->pushButton_SoundYesNo->setIcon(playerIcon(muted ? "muted" : "volume")); });
    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(space, &QShortcut::activated, this, &MainWindow::on_pushButton_PlaySong_clicked);
    // 输入文字或操作聚焦按钮时，空格交还给控件，避免聊天输入和键盘导航被播放快捷键抢走。
    connect(qApp, &QApplication::focusChanged, this, [this, space](QWidget *, QWidget *focused) {
        bool editingOrButton = false;
        for (auto *widget = focused; widget && widget != this; widget = widget->parentWidget()) {
            if (qobject_cast<QAbstractButton *>(widget) || qobject_cast<QLineEdit *>(widget) ||
                qobject_cast<QTextEdit *>(widget) || qobject_cast<QPlainTextEdit *>(widget)) editingOrButton = true;
        }
        space->setEnabled(!editingOrButton);
    });
    auto *open = new QShortcut(QKeySequence::Open, this);
    connect(open, &QShortcut::activated, this, &MainWindow::on_pushButton_AddSong_clicked);
    setupFocus();
    ui->pushButton_About->setVisible(!AppBackendClient::enabled());
    connect(AppBackendClient::instance(), &AppBackendClient::authChanged, this,
            [this](const QString &id, const QString &name) {
                if (focusModel)
                    focusModel->cancel();
                if (!id.isEmpty())
                    updateUsername(name);
                else
                    switchToGuestProfile();
                if (focusDialog) {
                    focusDialog->resetForAccount();
                    focusDialog->setBusy(false);
                    FocusStore store(QSqlDatabase::database());
                    focusDialog->setCandidates(focusCandidates("queue"), focusCandidates("favorites"));
                    focusDialog->setRules(store.rules(currentProfile));
                    updateBackendFocusStatus();
                }
                if (AppBackendClient::developmentMode()) {
                    ui->pushButton_SignIn->hide();
                    ui->label_DisplayUser->setText(id.isEmpty() ? tr("本机开发测试 · 未连接") : tr("本机开发测试"));
                    if (developmentRetryButton) {
                        developmentRetryButton->setVisible(id.isEmpty());
                        developmentRetryButton->setEnabled(!developmentBootstrapPending);
                    }
                }
            });
    connect(AppBackendClient::instance(), &AppBackendClient::agentStatusChanged, this,
            [this](bool, const QString &) { updateBackendFocusStatus(); });
    setupRadio();
    if (AppBackendClient::developmentMode()) {
        ui->pushButton_SignIn->hide();
        ui->pushButton_About->hide();
        localLogout->setVisible(false);
        developmentRetryButton = new QPushButton(tr("重试本机开发连接"), ui->pushButton_SignIn->parentWidget());
        developmentRetryButton->setObjectName(QStringLiteral("button_DevelopmentRetry"));
        developmentRetryButton->setAutoDefault(false);
        developmentRetryButton->hide();
        if (auto *sidebar = qobject_cast<QBoxLayout *>(ui->pushButton_SignIn->parentWidget()->layout()))
            sidebar->insertWidget(sidebar->indexOf(ui->pushButton_SignIn), developmentRetryButton);
        connect(developmentRetryButton, &QPushButton::clicked, this, &MainWindow::bootstrapDevelopmentSession);
        ui->label_DisplayUser->setText(tr("本机开发测试 · 连接中"));
        QTimer::singleShot(0, this, &MainWindow::bootstrapDevelopmentSession);
    }
}

MainWindow::~MainWindow()
{
    ++developmentBootstrapGeneration;
    delete developmentBootstrapOperation.data();
    developmentBootstrapOperation = nullptr;
    cancelSearch();
    if (radioClient) radioClient->cancel();
    if (musicTaskClient) {
        musicTaskClient->disconnect(this);
        delete musicTaskClient;
        musicTaskClient = nullptr;
    }
    if (focusModel) focusModel->cancel();
    // Stop controllers while their receiver and its view still exist. QObject
    // child destruction happens after MainWindow's members are destroyed.
    if (focusSession) {
        focusSession->disconnect(this);
        focusSession->cancel();
        delete focusSession;
        focusSession = nullptr;
    }
    if (p_PlayerObject) {
        p_PlayerObject->disconnect(this);
        p_PlayerObject->stop();
        delete p_PlayerObject;
        p_PlayerObject = nullptr;
    }
    if (p_PlayerList) {
        p_PlayerList->disconnect(this);
        delete p_PlayerList;
        p_PlayerList = nullptr;
    }
    delete ui;
}

void MainWindow::showContentPage(QWidget *page, QPushButton *navButton)
{
    if(!page)return;
    if(ui->pageStack->indexOf(page)<0) {
        if(auto *dialog=qobject_cast<QDialog *>(page)) {
            dialog->setWindowFlags(Qt::Widget);
            dialog->setModal(false);
            dialog->setMinimumSize(0,0);
            dialog->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
            connect(dialog,&QDialog::finished,this,[this,page](int) {
                if(ui->pageStack->currentWidget()==page)
                    showContentPage(ui->libraryPage,ui->libraryButton);
            });
        }
        ui->addPage(page);
    }
    ui->setPage(page);
    if(navButton) {navButton->setCheckable(true);navButton->setChecked(true);}
}

void MainWindow::cancelSearch()
{
    ++searchGeneration;
    delete searchOperation.data();searchOperation=nullptr;
    if(searchPage)searchPage->setBusy(false);
}

bool MainWindow::eventFilter(QObject *watched,QEvent *event)
{
    if(searchPage && watched==searchPage && event->type()==QEvent::Hide)cancelSearch();
    return QMainWindow::eventFilter(watched,event);
}

void MainWindow::refreshProfilePages()
{
    cancelSearch();searchQuery.clear();ui->lineEdit_InputSong->clear();
    if(searchPage) {searchPage->setResults({},0,{});searchPage->setStatus(tr("输入歌名或歌手，重新搜索。"));}
    if(historyPage)historyPage->DisplayHistory();
    if(favoritesPage)favoritesPage->loadFavoriteSongs();
}

void MainWindow::HandleLCDNumerTimeChangeFunc(qint64 SongDuration)
{
    const qint64 seconds = SongDuration / 1000;
    ui->elapsedLabel->setText(
        QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0')));
}

void MainWindow::HandleProgressTimeChangeFunc(qint64 SongDuration)
{
    const qint64 seconds = SongDuration / 1000;
    ui->durationLabel->setText(
        QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0')));
    const QSignalBlocker blocker(ui->horizontalSlider_PlayProgress);
    ui->horizontalSlider_PlayProgress->setMaximum(
        static_cast<int>(qBound<qint64>(0, SongDuration, INT_MAX)));
}

void MainWindow::HandlePositionChangeFunc(qint64 position)
{
    if (ui->horizontalSlider_PlayProgress->isSliderDown())
        return;
    const QSignalBlocker blocker(ui->horizontalSlider_PlayProgress);
    ui->horizontalSlider_PlayProgress->setValue(
        static_cast<int>(qBound<qint64>(0, position, INT_MAX)));
}

void MainWindow::on_pushButton_About_clicked()
{
    AboutDialog dialog(this);
    dialog.exec();
}

void MainWindow::on_pushButton_AddSong_clicked()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("请选择音频文件"), QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
        tr("音频文件 (*.mp3 *.wav *.m4a *.aac *.aiff *.aif *.flac *.ogg);;所有文件 (*)"));
    addLocalFiles(files);
}

void MainWindow::on_pushButton_PlaySong_clicked()
{
    if (p_PlayerList->isEmpty())
    {
        ui->statusLabel->setText(tr("请先添加本地音乐或搜索歌曲。"));
        return;
    }
    if (p_PlayerObject->state() == QMediaPlayer::PlayingState ||
        (resolvingTrack && playbackRequested))
    {
        if(radioPlayback)radioPaused=true;
        if (focusSession && focusSession->isActive())
            focusSession->setPaused(true);
        playbackRequested = false;
        p_PlayerObject->pause();
        ui->statusLabel->setText(tr("已暂停"));
        updateFocusPlaybackState();
        updateFocusStatus();
        return;
    }
    if (focusSession && focusSession->isActive())
        focusSession->setPaused(false);
    if(radioPlayback)radioPaused=false;
    playTrack(p_PlayerList->currentIndex() < 0 ? 0 : p_PlayerList->currentIndex());
}

void MainWindow::on_pushButton_StopPlaySong_clicked()
{
    endRadioPlayback(tr("电台已停止，本次歌单仍保留。"));
    if (focusSession)
        focusSession->cancel(tr("已停止专注计划"));
    playbackRequested = false;
    resolvingTrack = false;
    ++mediaGeneration;
    p_PlayerObject->stop();
}

void MainWindow::on_pushButton_PreviousSong_clicked()
{
    if(radioPlayback){advanceRadio(-1);return;}
    if (focusSession && focusSession->isActive()) {
        focusSession->previous();
        return;
    }
    const int count = p_PlayerList->mediaCount();
    if (!count)
        return;
    if (p_PlayerList->playbackMode() == QMediaPlaylist::Random)
        p_PlayerList->previous();
    else
        p_PlayerList->setCurrentIndex(
            p_PlayerList->currentIndex() <= 0 ? count - 1 : p_PlayerList->currentIndex() - 1);
    playTrack(p_PlayerList->currentIndex(), true);
}

void MainWindow::on_pushButton_PlayNextSong_clicked()
{
    if(radioPlayback){advanceRadio(1);return;}
    if (focusSession && focusSession->isActive()) {
        focusSession->next();
        return;
    }
    const int count = p_PlayerList->mediaCount();
    if (!count)
        return;
    if (p_PlayerList->playbackMode() == QMediaPlaylist::Random)
        p_PlayerList->next();
    else
        p_PlayerList->setCurrentIndex((p_PlayerList->currentIndex() + 1) % count);
    playTrack(p_PlayerList->currentIndex(), true);
}

void MainWindow::on_pushButton_SearchSong_clicked()
{
    const QString query = ui->lineEdit_InputSong->text().trimmed();
    if (query.isEmpty())
        return;
    if(!searchPage) {
        searchPage=new OnlineSearchDialog(this);
        searchPage->setObjectName(QStringLiteral("searchPage"));
        searchPage->installEventFilter(this);
        if(auto *back=searchPage->findChild<QPushButton *>("button_OnlineSearchClose"))back->setText(tr("返回音乐库"));
        connect(searchPage,&OnlineSearchDialog::pageRequested,this,&MainWindow::fetchSearchPage);
        connect(searchPage,&OnlineSearchDialog::songActivated,this,&MainWindow::playOnlineSong);
        connect(searchPage,&OnlineSearchDialog::officialPageRequested,searchPage,[](qint64 id) {
            QDesktopServices::openUrl(QUrl(QString("https://music.163.com/song?id=%1").arg(id)));
        });
    }
    showContentPage(searchPage);
    searchQuery=query;
    fetchSearchPage(0);
}

void MainWindow::fetchSearchPage(int offset)
{
    if(!searchPage || searchQuery.isEmpty() || offset<0)return;
    cancelSearch();
    searchOperation=new QObject(searchPage);
    const quint64 generation=searchGeneration;
    const QString query=searchQuery;
    searchPage->setBusy(true);searchPage->setStatus(tr("正在搜索：%1").arg(query));
    QUrlQuery parameters;parameters.addQueryItem("keywords",query);
    parameters.addQueryItem("limit","30");parameters.addQueryItem("offset",QString::number(offset));
    netease()->get("/search",parameters,searchOperation,[this,generation,query](const QJsonObject &data,const QString &error) {
        if(generation!=searchGeneration || !searchPage)return;
        searchPage->setBusy(false);
        if(!error.isEmpty()){searchPage->setStatus(error);return;}
        QVector<OnlineSong> songs;
        for(const auto &value:data.value("songs").toArray()) {
            const auto entry=value.toObject();OnlineSong song;
            song.id=entry.value("id").toVariant().toLongLong();song.title=entry.value("title").toString();
            song.artist=entry.value("artist").toString();song.durationMs=entry.value("durationMs").toInt();
            song.fee=entry.value("fee").toInt();song.album=entry.value("album").toString();
            song.coverUrl=entry.value("coverUrl").toString();
            if(song.id>0 && song.id<=9007199254740991LL && !song.title.isEmpty() && songs.size()<30)songs.append(song);
        }
        searchPage->setResults(songs,data.value("total").toInt(songs.size()),query);
    });
}

void MainWindow::on_pushButton_SoundYesNo_clicked()
{
    const bool muted = !p_PlayerObject->isMuted();
    p_PlayerObject->setMuted(muted);
    ui->pushButton_SoundYesNo->setIcon(playerIcon(muted ? "muted" : "volume"));
}

void MainWindow::on_horizontalSlider_Volume_valueChanged(int value)
{
    if (!p_PlayerObject)
        return;
    p_PlayerObject->setVolume(value);
    if (value > 0)
    {
        p_PlayerObject->setMuted(false);
        ui->pushButton_SoundYesNo->setIcon(playerIcon("volume"));
    }
}

void MainWindow::on_horizontalSlider_PlayProgress_valueChanged(int value)
{
    if (p_PlayerObject && p_PlayerObject->duration() > 0)
        p_PlayerObject->setPosition(value);
}

void MainWindow::on_pushButton_LikeSong_clicked()
{
    if(!favoritesPage) {
        favoritesPage=new LikeSong(this);
        favoritesPage->setObjectName(QStringLiteral("favoritesPage"));
        connect(favoritesPage,&LikeSong::songSelected,this,&MainWindow::onSongSelected);
    }
    favoritesPage->loadFavoriteSongs();
    showContentPage(favoritesPage,ui->pushButton_LikeSong);
}

void MainWindow::on_pushButton_HistorySong_clicked()
{
    if(!historyPage) {
        historyPage=new HistorySong(this);
        historyPage->setObjectName(QStringLiteral("historyPage"));
        connect(historyPage,&HistorySong::songSelected,this,&MainWindow::onSongSelected1);
        connect(historyPage,&HistorySong::historyCleared,this,[this]{lastHistoryUrl.clear();});
    }
    historyPage->DisplayHistory();
    showContentPage(historyPage,ui->pushButton_HistorySong);
}

void MainWindow::on_pushButton_AddLike_clicked()
{
    const int index = p_PlayerList->currentIndex();
    if (index < 0 || index >= tracks.size())
    {
        QMessageBox::information(this, tr("收藏歌曲"), tr("请先选择一首歌曲。"));
        return;
    }
    const Track &track = tracks.at(index);
    if (isOnlineTrack(index))
    {
        QSqlQuery existing;
        existing.prepare("SELECT 1 FROM favorite_songs WHERE song_id = ? AND media_url LIKE "
                         "'http%music.163.com/%' LIMIT 1");
        existing.addBindValue(track.id);
        if (existing.exec() && existing.next())
        {
            ui->statusLabel->setText(tr("这首歌曲已在喜欢的音乐中。"));
            ui->pushButton_AddLike->setIcon(playerIcon("heart", QColor("#b4d9a0")));
            return;
        }
    }
    QSqlQuery query;
    query.prepare("INSERT OR IGNORE INTO favorite_songs (song_id, song_name, artist_name, "
                  "media_url) VALUES (?, ?, ?, ?)");
    query.addBindValue(track.id);
    query.addBindValue(track.title);
    query.addBindValue(track.artist);
    query.addBindValue(track.url.toString(QUrl::FullyEncoded));
    if (!query.exec())
    {
        QMessageBox::warning(this, tr("收藏失败"), query.lastError().text());
        return;
    }
    const bool added = query.numRowsAffected() > 0;
    emit songLiked(track.title);
    if(favoritesPage && favoritesPage->isVisible())favoritesPage->loadFavoriteSongs();
    ui->pushButton_AddLike->setIcon(playerIcon("heart", QColor("#b4d9a0")));
    ui->statusLabel->setText(added ? tr("已收藏 · 可在“喜欢的音乐”中双击播放。")
                                   : tr("这首歌曲已在喜欢的音乐中。"));
}

void MainWindow::on_pushButton_RankList_clicked()
{
    if(!rankingsPage) {
        rankingsPage=new RankList(this);
        rankingsPage->setObjectName(QStringLiteral("rankingsPage"));
        connect(rankingsPage,&RankList::songSelected1,this,&MainWindow::onSongSelected1);
    }
    showContentPage(rankingsPage);
}

void MainWindow::on_pushButton_SignIn_clicked()
{
    if (AppBackendClient::developmentMode()) { bootstrapDevelopmentSession(); return; }
    SignIn dialog(this);
    connect(&dialog, &SignIn::userLoggedIn, this, &MainWindow::updateUsername);
    dialog.exec();
}
void MainWindow::bootstrapDevelopmentSession()
{
    if (!AppBackendClient::developmentMode() || developmentBootstrapPending) return;
    developmentBootstrapPending = true;
    const quint64 generation = ++developmentBootstrapGeneration;
    delete developmentBootstrapOperation.data();
    developmentBootstrapOperation = new QObject(this);
    ui->pushButton_SignIn->hide();
    ui->label_DisplayUser->setText(tr("本机开发测试 · 连接中"));
    if (developmentRetryButton) { developmentRetryButton->hide(); developmentRetryButton->setEnabled(false); }
    auto *backend = AppBackendClient::instance();
    backend->bootstrapDevelopmentSession(developmentBootstrapOperation,
        [this, generation](const QString &error) {
            if (generation != developmentBootstrapGeneration) return;
            developmentBootstrapPending = false;
            auto *backend = AppBackendClient::instance();
            if (!error.isEmpty() || !backend->authenticated()) {
                ui->label_DisplayUser->setText(tr("本机开发测试 · 未连接"));
                ui->statusLabel->setText(error.isEmpty() ? tr("本机开发身份未就绪，请重试连接。") : error);
                if (developmentRetryButton) { developmentRetryButton->show(); developmentRetryButton->setEnabled(true); }
                return;
            }
            updateUsername(backend->username());
            if (!backend->authenticated()) {
                ui->label_DisplayUser->setText(tr("本机开发测试 · 未连接"));
                ui->statusLabel->setText(tr("无法打开本机开发数据空间，请检查开发启动配置。"));
                if (developmentRetryButton) { developmentRetryButton->show(); developmentRetryButton->setEnabled(true); }
                return;
            }
            ui->label_DisplayUser->setText(tr("本机开发测试"));
            ui->statusLabel->setText(tr("本机开发测试已就绪，可以直接打开音乐助手；生成仍受后台额度保护。"));
            if (developmentRetryButton) developmentRetryButton->hide();
            // Configuration/status and observe-only restore never start a model.
            backend->refreshAgentStatus(developmentBootstrapOperation,
                [this, generation](const QString &statusError) {
                    if (generation != developmentBootstrapGeneration || statusError.isEmpty()) return;
                    ui->statusLabel->setText(tr("已连接本机开发身份，AI 状态暂不可用，请稍后重试。"));
                    if (developmentRetryButton) { developmentRetryButton->show(); developmentRetryButton->setEnabled(true); }
                });
        });
}

void MainWindow::on_pushButton_Download_2_clicked()
{
    QMessageBox::information(
        this, tr("音乐下载"),
        tr("当前支持在线播放与本地文件。歌曲下载请在网易云官方客户端按账号权益操作。"));
}

void MainWindow::onSongSelected(const QString &songName)
{
    const QUrl source(songName);
    if (source.isLocalFile())
    {
        addLocalFiles({source.toLocalFile()});
        return;
    }
    if (source.scheme() == "https" || source.scheme() == "http")
    {
        QSqlQuery query;
        query.prepare(
            "SELECT song_id, song_name, artist_name FROM favorite_songs WHERE media_url = ?");
        query.addBindValue(songName);
        if (query.exec() && query.next())
        {
            OnlineSong song;
            song.id = query.value(0).toLongLong();
            song.title = query.value(1).toString();
            song.artist = query.value(2).toString();
            if (song.id <= 0)
                song.id = QUrlQuery(source).queryItemValue("id").toLongLong();
            if (song.id > 0)
            {
                playOnlineSong(song);
                return;
            }
        }
        const qint64 id = QUrlQuery(source).queryItemValue("id").toLongLong();
        if (source.host() == "music.163.com" && id > 0)
        {
            OnlineSong song;
            song.id = id;
            song.title = tr("网易云歌曲 %1").arg(id);
            playOnlineSong(song);
            return;
        }
        ui->statusLabel->setText(tr("无法识别收藏的在线歌曲，请重新搜索。"));
        return;
    }
    ui->lineEdit_InputSong->setText(songName);
    on_pushButton_SearchSong_clicked();
}
void MainWindow::updateUsername(QString username)
{
    username = username.trimmed();
    if (username.isEmpty())
        return;
    QString profile = "local:" + username;
    if (AppBackendClient::enabled()) {
        auto *backend = AppBackendClient::instance();
        if (!backend->authenticated())
            return;
        username = backend->username();
        profile = "server:" + backend->serviceIdentity() + ":" + backend->userId();
    }
    if (profile == currentProfile)
        return;
    if (profile != currentProfile && !openProfileDatabase(profile)) {
        if (AppBackendClient::enabled())
            AppBackendClient::instance()->logout(this);
        return;
    }
    if (focusSession)
        focusSession->cancel(tr("账户已切换，专注计划已结束"));
    currentProfile = profile;
    NeteaseClient::setAccountProfile(currentProfile);
    invalidateCloudPlayback();
    applyCloudAccount({});
    lastHistoryUrl.clear();
    isSign = true;
    ui->label_DisplayUser->setText(AppBackendClient::developmentMode() ? tr("本机开发测试") : "用户 " + username);
    ui->pushButton_SignIn->setText(tr("  切换探歌账户"));
    if (p_PlayerList->currentIndex() >= 0)
        updateCurrentSong(p_PlayerList->currentIndex());
    refreshProfilePages();
}
void MainWindow::onSongSelected1(const QString &sourceText)
{
    const QUrl source(sourceText);
    if (source.isLocalFile()) {
        // Resolve the stored source, never a display title: different files can
        // have identical names, and a file may have moved since it was played.
        addLocalFiles({source.toLocalFile()});
        return;
    }
    if (source.scheme() == "https" || source.scheme() == "http") {
        bool validId = false;
        const qint64 id = QUrlQuery(source).queryItemValue("id").toLongLong(&validId);
        if (source.host() != "music.163.com" || source.path() != "/song" ||
            !validId || id <= 0 || id > 9007199254740991LL) {
            ui->statusLabel->setText(tr("这条播放记录的在线地址已失效，请重新搜索歌曲。"));
            return;
        }
        OnlineSong song;
        song.id = id;
        song.title = tr("网易云歌曲 %1").arg(id);
        QSqlQuery query;
        query.prepare("SELECT song_name, artist_name FROM play_history "
                      "WHERE media_url = ? ORDER BY id DESC LIMIT 1");
        query.addBindValue(sourceText);
        if (query.exec() && query.next()) {
            song.title = query.value(0).toString();
            song.artist = query.value(1).toString();
        }
        // playOnlineSong always refreshes account permissions and the stream
        // URL. History only stores the stable NetEase identity, never the CDN.
        playOnlineSong(song);
        return;
    }
    // The legacy demonstration chart still supplies titles rather than URLs.
    onSongSelected(sourceText);
}
void MainWindow::on_pushButton_Download_clicked()
{
    QString folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (folder.isEmpty())
        folder = QDir::homePath();
    QDir().mkpath(folder);
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

bool MainWindow::initializeDatabase()
{
    if (openProfileDatabase(currentProfile))
        return true;
    QString error;
    const bool opened = openTemporaryGuestDatabase(&error);
    ui->label_DisplayUser->setText(tr("访客 · 临时空间，不保存数据"));
    ui->statusLabel->setText(opened ? tr("本机数据暂不可用，正在使用临时访客空间；关闭软件后数据不保留。")
                                   : tr("本机数据暂不可用，访客数据功能已停用：%1").arg(error));
    return opened;
}

bool MainWindow::openProfileDatabase(const QString &profile, QString *failure)
{
    if (failure)
        failure->clear();
    const auto fail = [this, failure](const QString &title, const QString &message) {
        if (failure)
            *failure = message;
        else
            QMessageBox::warning(this, title, message);
        return false;
    };
    QString dataDir = qEnvironmentVariable("MUSICPLAYER_DATA_DIR");
    if (dataDir.isEmpty())
        dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dataDir.isEmpty() || !QDir().mkpath(dataDir))
    {
        return fail(tr("数据目录不可用"), tr("无法创建应用数据目录，历史与收藏将不可用。"));
    }
    QSqlDatabase db = QSqlDatabase::contains()
                          ? QSqlDatabase::database(QSqlDatabase::defaultConnection, false)
                          : QSqlDatabase::addDatabase("QSQLITE");
    const QString previousPath = db.databaseName();
    QString path = QDir(dataDir).filePath("history.db");
    if (profile != "guest") {
        const QString folder = QDir(dataDir).filePath("profiles");
        if (!QDir().mkpath(folder))
            return fail(tr("账户数据不可用"), tr("无法创建账户数据目录。"));
        const QString identity = QString::fromLatin1(QCryptographicHash::hash(
            profile.toUtf8(), QCryptographicHash::Sha256).toHex());
        path = QDir(folder).filePath(identity + ".db");
    }
    const QFileInfo file(path);
    if ((file.exists() && !file.isWritable()) || !QFileInfo(file.absolutePath()).isWritable())
        return fail(tr("账户数据不可用"), tr("数据文件或所在目录不可写。"));
    // Validate and migrate the target on its own connection. A failed switch
    // must never leave the old account label pointing at another account's DB.
    const QString validationName = "profile-validation-" + QUuid::createUuid().toString();
    QString error;
    {
        QSqlDatabase candidate = QSqlDatabase::addDatabase("QSQLITE", validationName);
        candidate.setDatabaseName(path);
        if (!candidate.open()) {
            error = candidate.lastError().text();
        } else {
            initializeProfileSchema(candidate, &error);
        }
        candidate.close();
    }
    QSqlDatabase::removeDatabase(validationName);
    if (!error.isEmpty())
        return fail(tr("账户数据不可用"), error);
    db.close();
    db.setDatabaseName(path);
    if (!db.open()) {
        const QString failure = db.lastError().text();
        db.setDatabaseName(previousPath);
        if (!previousPath.isEmpty())
            db.open();
        return fail(tr("数据库不可用"), failure);
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool MainWindow::openTemporaryGuestDatabase(QString *error)
{
    QSqlDatabase db = QSqlDatabase::contains()
                          ? QSqlDatabase::database(QSqlDatabase::defaultConnection, false)
                          : QSqlDatabase::addDatabase("QSQLITE");
    db.close();
    // Change the stored path even when opening fails. Later calls to
    // QSqlDatabase::database() must never reopen the account that just logged out.
    db.setDatabaseName(QStringLiteral(":memory:"));
    if (!db.open()) {
        *error = db.lastError().text();
        return false;
    }
    if (!initializeProfileSchema(db, error)) {
        db.close();
        return false;
    }
    return true;
}

int MainWindow::appendTrack(const QUrl &url, const QString &title, const QString &artist, qint64 id)
{
    const QString key = url.toString(QUrl::FullyEncoded);
    const auto existing = trackIndexes.constFind(key);
    if (existing != trackIndexes.cend())
        return existing.value();
    const int index = tracks.size();
    tracks.append({url, title, artist, id, {}, {}});
    trackIndexes.insert(key, index);
    ui->queueModel->append(title, artist, url.isLocalFile(), url, id);
    ui->queueCountLabel->setText(tr("%1 首歌曲").arg(tracks.size()));
    ui->emptyQueueLabel->hide();
    p_PlayerList->addMedia(url);
    return index;
}

void MainWindow::addLocalFiles(const QStringList &files)
{
    if(!files.isEmpty())endRadioPlayback(tr("已切换播放内容，可重新开始这批电台音乐。"));
    int first = -1;
    QStringList missing;
    for (const QString &file : files)
    {
        const QFileInfo info(file);
        if (!info.isFile() || !info.isReadable())
        {
            missing.append(file);
            continue;
        }
        const QUrl url = QUrl::fromLocalFile(info.canonicalFilePath());
        const int id = -static_cast<int>((qHash(url.toString()) & 0x3fffffffU) + 1U);
        const int index = appendTrack(url, info.completeBaseName(), tr("本地音乐"), id);
        if (first < 0)
            first = index;
    }
    if (first >= 0)
    {
        playTrack(first);
        ui->statusLabel->setText(
            tr("已加入 %1 首歌曲 · 本地播放").arg(files.size() - missing.size()));
    }
    if (!missing.isEmpty())
        QMessageBox::warning(this, tr("文件不可用"),
                             tr("以下文件不存在或不可读：\n%1").arg(missing.join('\n')));
}

void MainWindow::updateCurrentSong(int index)
{
    if (index < 0 || index >= tracks.size())
        return;
    const Track &track = tracks.at(index);
    const QString source = track.url.isLocalFile() ? tr("本地音乐") : tr("网易云音乐");
    const QString artist=track.artist.isEmpty() || track.artist==source
        ? source : track.artist+QStringLiteral("  ·  ")+source;
    ui->setNowPlaying(track.title,artist,track.url,track.id,track.coverUrl);
    ui->label_DisplaySongTitle->setToolTip(track.title);
    ui->queueModel->setCurrentTrack(index);
    ui->queueView->selectRow(index);
    loadTrackLyrics(track.url);
    ui->lyricsView->setTrackTitle(track.title);
    floatingLyrics->setTrackTitle(track.title);
    QSqlQuery favorite;
    favorite.prepare("SELECT 1 FROM favorite_songs WHERE media_url = ? OR (song_id = ? AND song_id "
                     "> 0 AND media_url LIKE 'http%music.163.com/%') LIMIT 1");
    favorite.addBindValue(track.url.toString(QUrl::FullyEncoded));
    favorite.addBindValue(track.id);
    const bool liked = favorite.exec() && favorite.next();
    ui->pushButton_AddLike->setIcon(playerIcon("heart", QColor(liked ? "#b4d9a0" : "#cbd4cd")));
}

void MainWindow::recordCurrentSong()
{
    const int index = p_PlayerList->currentIndex();
    if (index < 0 || index >= tracks.size() || preparedTrack != index)
        return;
    const Track &track = tracks.at(index);
    const QString url = track.url.toString(QUrl::FullyEncoded);
    if (lastHistoryUrl == url)
    {
        // History can also be cleared by another open window. Confirm the
        // cached row still exists before suppressing a duplicate state event.
        QSqlQuery exists;
        exists.prepare("SELECT 1 FROM play_history WHERE media_url = ? LIMIT 1");
        exists.addBindValue(url);
        if (exists.exec() && exists.next())
            return;
    }
    QSqlQuery query;
    query.prepare("INSERT INTO play_history (song_id, song_name, artist_name, media_url) VALUES "
                  "(?, ?, ?, ?)");
    query.addBindValue(track.id);
    query.addBindValue(track.title);
    query.addBindValue(track.artist);
    query.addBindValue(url);
    if (query.exec()) {
        lastHistoryUrl = url;
        if (historyPage && historyPage->isVisible())
            historyPage->DisplayHistory();
    } else
        qWarning() << "Could not save play history:" << query.lastError();
}

void MainWindow::updatePlaybackMode()
{
    const bool shuffle = ui->shuffleButton->isChecked();
    p_PlayerList->setPlaybackMode(shuffle           ? QMediaPlaylist::Random
                                  : repeatMode == 0 ? QMediaPlaylist::Loop
                                  : repeatMode == 1 ? QMediaPlaylist::CurrentItemInLoop
                                                    : QMediaPlaylist::Sequential);
    ui->repeatButton->setChecked(!shuffle && repeatMode != 2);
    ui->repeatButton->setText(repeatMode == 1 ? "1" : "");
    ui->statusLabel->setText(shuffle           ? tr("随机播放")
                             : repeatMode == 0 ? tr("列表循环")
                             : repeatMode == 1 ? tr("单曲循环")
                                               : tr("顺序播放"));
}

bool MainWindow::readLyricsFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024)
        return false;
    const QByteArray bytes = file.readAll();
    QTextCodec::ConverterState state;
    QString text =
        QTextCodec::codecForName("UTF-8")->toUnicode(bytes.constData(), bytes.size(), &state);
    if (state.invalidChars)
        text = QTextCodec::codecForName("GB18030")->toUnicode(bytes);
    const LyricsDocument document = LyricsDocument::fromLrc(text);
    if (document.isEmpty())
        return false;
    ui->lyricsView->setDocument(document);
    floatingLyrics->setDocument(document);
    ui->lyricsView->setPosition(p_PlayerObject->position() + lyricsOffset);
    floatingLyrics->setPosition(p_PlayerObject->position() + lyricsOffset);
    return !ui->lyricsView->document().isEmpty();
}

void MainWindow::loadTrackLyrics(const QUrl &url)
{
    const QString key = url.toString(QUrl::FullyEncoded);
    if (key == lyricsTrackUrl)
        return;
    lyricsTrackUrl = key;
    ui->lyricsView->setLyrics(QString());
    floatingLyrics->setDocument(ui->lyricsView->document());
    if (!lyricFiles.contains(key))
    {
        QSettings links(QDir(NeteaseClient::dataDirectory()).filePath("lyric-links.ini"),
                        QSettings::IniFormat);
        const QString saved = links.value(lyricSettingsKey(url)).toString();
        if (!saved.isEmpty())
            lyricFiles.insert(key, saved);
    }
    if (lyricFiles.contains(key))
    {
        if (readLyricsFile(lyricFiles.value(key)))
            return;
        lyricFiles.remove(key);
    }
    if (!url.isLocalFile())
        return;
    const QFileInfo audio(url.toLocalFile());
    readLyricsFile(audio.dir().filePath(audio.completeBaseName() + ".lrc"));
}

void MainWindow::importLyrics()
{
    const int index = p_PlayerList->currentIndex();
    if (index < 0 || index >= tracks.size())
    {
        ui->statusLabel->setText(tr("请先添加并选择一首歌曲，再导入歌词。"));
        return;
    }
    const QString file =
        QFileDialog::getOpenFileName(this, tr("导入同步歌词"), tracks.at(index).url.toLocalFile(),
                                     tr("LRC 歌词 (*.lrc);;所有文件 (*)"));
    if (file.isEmpty())
        return;
    if (p_PlayerList->currentIndex() != index)
    {
        ui->statusLabel->setText(tr("歌曲已切换，请为当前歌曲重新选择歌词。"));
        return;
    }
    if (readLyricsFile(file))
    {
        lyricFiles.insert(tracks.at(index).url.toString(QUrl::FullyEncoded), file);
        QSettings links(QDir(NeteaseClient::dataDirectory()).filePath("lyric-links.ini"),
                        QSettings::IniFormat);
        links.setValue(lyricSettingsKey(tracks.at(index).url), QFileInfo(file).absoluteFilePath());
        links.sync();
        QFile::setPermissions(links.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        ui->statusLabel->setText(links.status() == QSettings::NoError
                                     ? tr("歌词已同步，关联已保存 · 下次打开同一首歌会自动加载")
                                     : tr("歌词已同步，但关联无法保存到磁盘。"));
    }
    else
        ui->statusLabel->setText(tr("无法读取有效的 LRC 时间戳歌词，请检查文件（最大 2 MB）。"));
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList files;
    for (const auto &url : event->mimeData()->urls())
        if (url.isLocalFile())
            files.append(url.toLocalFile());
    if (!files.isEmpty())
    {
        addLocalFiles(files);
        event->acceptProposedAction();
    }
}

NeteaseClient *MainWindow::netease()
{
    if (!cloudClient)
        cloudClient = new NeteaseClient(this);
    return cloudClient;
}

bool MainWindow::isOnlineTrack(int index) const
{
    return index >= 0 && index < tracks.size() && tracks.at(index).id > 0 &&
           !tracks.at(index).url.isLocalFile();
}

void MainWindow::playOnlineSong(const OnlineSong &song)
{
    if (song.id <= 0 || song.id > 9007199254740991LL)
    {
        ui->statusLabel->setText(tr("歌曲 ID 无效，请重新搜索。"));
        return;
    }
    const QUrl canonical(QString("https://music.163.com/song?id=%1").arg(song.id));
    endRadioPlayback(tr("已手动选曲，可重新开始这批电台音乐。"));
    const int index = appendTrack(canonical, song.title, song.artist, song.id);
    if (!song.coverUrl.isEmpty() || !song.album.isEmpty()) {
        tracks[index].album = song.album;
        tracks[index].coverUrl = song.coverUrl;
        ui->queueModel->setArtwork(index, song.album, song.coverUrl);
    }
    playTrack(index, true);
}

void MainWindow::playTrack(int index, bool forceRefresh, bool autoplay)
{
    if (index < 0 || index >= tracks.size())
        return;
    if (focusSession && focusSession->isActive() &&
        (focusSession->currentKey() != tracks.at(index).url.toString(QUrl::FullyEncoded) ||
         (forceRefresh && !loadingFocusTrack)))
        focusSession->cancel(tr("你已手动选曲，专注计划已结束"));
    playbackRequested = autoplay;
    const bool same = p_PlayerList->currentIndex() == index;
    const bool fresh =
        !isOnlineTrack(index) || QDateTime::currentMSecsSinceEpoch() - resolvedAt < 10 * 60 * 1000;
    if (same && !forceRefresh && preparedTrack == index && !resolvingTrack && fresh &&
        p_PlayerObject->state() == QMediaPlayer::PausedState)
    {
        ui->statusLabel->setText(activeTrial ? tr("继续播放 · 仅试听") : tr("继续播放"));
        p_PlayerObject->play();
        return;
    }
    if (!same)
        p_PlayerList->setCurrentIndex(index);
    else
        updateCurrentSong(index);
    prepareTrack(index);
}

void MainWindow::prepareTrack(int index)
{
    const int generation = ++mediaGeneration;
    const Track track = tracks.at(index);
    resolvingTrack = false;
    preparedTrack = -1;
    lyricsOffset = 0;
    activeTrial = false;
    p_PlayerObject->stop();
    p_PlayerObject->setMedia(QMediaContent());
    ui->elapsedLabel->setText("0:00");
    ui->durationLabel->setText("0:00");
    {
        const QSignalBlocker blocker(ui->horizontalSlider_PlayProgress);
        ui->horizontalSlider_PlayProgress->setRange(0, 0);
    }
    if (!isOnlineTrack(index))
    {
        p_PlayerObject->setMedia(track.url);
        preparedTrack = index;
        if (playbackRequested)
            p_PlayerObject->play();
        return;
    }
    resolvingTrack = true;
    ui->statusLabel->setText(tr("正在获取账号授权的播放地址…"));
    QUrlQuery parameters;
    parameters.addQueryItem("id", QString::number(track.id));
    parameters.addQueryItem("level", "standard");
    netease()->get(
        "/song/url", parameters, this,
        [this, index, generation, track](const QJsonObject &data, const QString &error)
        {
            if (generation != mediaGeneration || p_PlayerList->currentIndex() != index)
                return;
            resolvingTrack = false;
            const QUrl stream(data.value("url").toString());
            if (!error.isEmpty() || !NeteaseClient::isAllowedStreamUrl(stream))
            {
                playbackRequested = false;
                const QString reason =
                    error.isEmpty()
                        ? data.value("message").toString(tr("当前账号未获得这首歌的播放权限。"))
                        : error;
                ui->statusLabel->setText(reason + tr(" · 可在“更多功能”中打开网易云确认权益"));
                ui->artistLabel->setText(track.artist + tr(" / 网易云 · 暂不可播"));
                ui->miniArtistLabel->setText(ui->artistLabel->text());
                emit onlinePlaybackFailed(track.id, reason);
                failFocusTrack(track.url.toString(QUrl::FullyEncoded));
                return;
            }
            const bool trial = data.value("trial").toBool();
            activeTrial = trial;
            if(trial && radioPlayback) {
                ui->statusLabel->setText(tr("这首仅能试听，电台将跳过它。"));
                failRadioTrack(track.id);return;
            }
            if (trial && focusSession && focusSession->isActive()) {
                playbackRequested = false;
                ui->statusLabel->setText(tr("这首仅能试听，专注计划将跳过它。"));
                failFocusTrack(track.url.toString(QUrl::FullyEncoded));
                return;
            }
            lyricsOffset =
                trial ? qMax<qint64>(0, data.value("trialStartMs").toVariant().toLongLong()) : 0;
            // The account service decides the returned entitlement. A catalog fee
            // flag or an HTTP 200 response must never be treated as full access.
            ui->artistLabel->setText(
                track.artist + (trial ? tr(" / 网易云 · 仅试听") : tr(" / 网易云 · 账号授权")));
            ui->miniArtistLabel->setText(ui->artistLabel->text());
            ui->statusLabel->setText(trial ? tr("仅试听 · 服务未返回完整版，请在网易云确认该曲权益")
                                           : tr("已获取当前账号的完整播放地址"));
            p_PlayerObject->setMedia(stream);
            preparedTrack = index;
            resolvedAt = QDateTime::currentMSecsSinceEpoch();
            emit onlinePlaybackResolved(track.id, trial);
            if (playbackRequested)
                p_PlayerObject->play();
            if (!lyricFiles.contains(track.url.toString(QUrl::FullyEncoded)))
            {
                QUrlQuery lyricQuery;
                lyricQuery.addQueryItem("id", QString::number(track.id));
                netease()->get(
                    "/lyrics", lyricQuery, this,
                    [this, generation](const QJsonObject &lyrics, const QString &failure)
                    {
                        if (generation != mediaGeneration || !failure.isEmpty() ||
                            lyricFiles.contains(lyricsTrackUrl))
                            return;
                        const LyricsDocument document =
                            LyricsDocument::fromOnlineLyrics(lyrics.value("lyric").toString(),lyrics.value("yrc").toString());
                        ui->lyricsView->setDocument(document);
                        floatingLyrics->setDocument(document);
                        ui->lyricsView->setPosition(p_PlayerObject->position() + lyricsOffset);
                        floatingLyrics->setPosition(p_PlayerObject->position() + lyricsOffset);
                    });
            }
        });
}

void MainWindow::handleEndOfMedia()
{
    if (!playbackRequested || resolvingTrack || preparedTrack != p_PlayerList->currentIndex())
        return;
    if(radioPlayback){advanceRadio(1);return;}
    if (focusSession && focusSession->isActive()) {
        focusSession->next();
        return;
    }
    if (p_PlayerList->playbackMode() == QMediaPlaylist::CurrentItemInLoop)
    {
        playTrack(p_PlayerList->currentIndex(), true);
        return;
    }
    p_PlayerList->next();
    if (p_PlayerList->currentIndex() < 0)
    {
        playbackRequested = false;
        preparedTrack = -1;
        return;
    }
    playTrack(p_PlayerList->currentIndex(), true);
}

void MainWindow::invalidateCloudPlayback()
{
    endRadioPlayback(tr("网易云授权已变化，请确认账号后重新开始电台。"));
    if (isOnlineTrack(p_PlayerList->currentIndex()))
    {
        if (focusSession)
            focusSession->cancel(tr("音乐服务账户已变化，专注计划已结束"));
        ++mediaGeneration;
        playbackRequested = false;
        resolvingTrack = false;
        preparedTrack = -1;
        p_PlayerObject->stop();
        p_PlayerObject->setMedia(QMediaContent());
    }
}

void MainWindow::applyCloudAccount(const QJsonObject &account)
{
    const bool loggedIn = account.value("loggedIn").toBool();
    const QString nickname = account.value("nickname").toString();
    if (loggedIn != cloudLoggedIn || nickname != cloudNickname)
        invalidateCloudPlayback();
    cloudLoggedIn = loggedIn;
    cloudNickname = nickname;
    ui->neteaseAccountButton->setText(loggedIn ? tr("  网易云已连接") : tr("  连接网易云"));
    ui->neteaseAccountButton->setToolTip(loggedIn ? tr("网易云：%1 · 点击查看账户").arg(nickname)
                                                  : tr("扫码登录网易云，使用账号实际播放权益"));
}

void MainWindow::logoutNetease()
{
    invalidateCloudPlayback();
    netease()->post("/logout", {}, this,
                    [this](const QJsonObject &, const QString &error)
                    {
                        if (!error.isEmpty())
                        {
                            ui->statusLabel->setText(error);
                            return;
                        }
                        applyCloudAccount({});
                        ui->statusLabel->setText(tr("已退出网易云账号；本地音乐仍可播放。"));
                    });
}

void MainWindow::openOfficialSong()
{
    const int index = p_PlayerList->currentIndex();
    if (!isOnlineTrack(index))
    {
        ui->statusLabel->setText(tr("请先选择一首网易云歌曲。"));
        return;
    }
    QDesktopServices::openUrl(
        QUrl(QString("https://music.163.com/song?id=%1").arg(tracks.at(index).id)));
}

void MainWindow::openNeteaseAccount()
{
    NetEaseLoginDialog dialog(this);
    struct LoginState
    {
        QString key;
        int generation = 0;
        bool checking = false;
    };
    auto state = std::make_shared<LoginState>();
    QTimer poll(&dialog);
    poll.setInterval(2000);
    auto loadAccount = [this, &dialog, state, &poll](bool createIfLoggedOut)
    {
        const int expectedGeneration = state->generation;
        netease()->get("/account/status", {}, &dialog,
                       [this, &dialog, state, &poll, createIfLoggedOut,
                        expectedGeneration](const QJsonObject &account, const QString &error)
                       {
                           if (state->generation != expectedGeneration)
                               return;
                           if (!error.isEmpty())
                           {
                               dialog.setBusy(false);
                               dialog.setStatus(error);
                               return;
                           }
                           dialog.setBusy(false);
                           applyCloudAccount(account);
                           dialog.setAccount(account.value("loggedIn").toBool(),
                                             account.value("nickname").toString(),
                                             account.value("vipType").toInt());
                           if (account.value("loggedIn").toBool())
                           {
                               poll.stop();
                               state->key.clear();
                               return;
                           }
                           if (createIfLoggedOut)
                               emit dialog.refreshQrRequested();
                           else
                               dialog.setStatus(tr("登录尚未生效，请刷新二维码重试。"));
                       });
    };
    connect(
        &dialog, &NetEaseLoginDialog::refreshQrRequested, &dialog,
        [this, &dialog, state, &poll]()
        {
            poll.stop();
            state->key.clear();
            state->checking = false;
            const int generation = ++state->generation;
            dialog.setBusy(true);
            dialog.setStatus(tr("正在生成网易云登录二维码…"));
            netease()->post(
                "/login/qr", {}, &dialog,
                [&dialog, state, &poll, generation](const QJsonObject &data, const QString &error)
                {
                    if (generation != state->generation)
                        return;
                    dialog.setBusy(false);
                    if (!error.isEmpty())
                    {
                        dialog.setStatus(error);
                        return;
                    }
                    state->key = data.value("key").toString();
                    dialog.setQrImage(data.value("qrImage").toString());
                    if (state->key.isEmpty())
                    {
                        dialog.setStatus(tr("二维码未生成，请刷新重试。"));
                        return;
                    }
                    dialog.setStatus(tr("使用网易云音乐 App 扫码，并在手机上确认登录。"));
                    poll.start();
                });
        });
    connect(&poll, &QTimer::timeout, &dialog,
            [this, &dialog, state, &poll, loadAccount]()
            {
                if (state->key.isEmpty() || state->checking)
                    return;
                state->checking = true;
                const int generation = state->generation;
                netease()->post("/login/qr/check", {{"key", state->key}}, &dialog,
                                [this, &dialog, state, &poll, loadAccount,
                                 generation](const QJsonObject &data, const QString &error)
                                {
                                    if (generation != state->generation)
                                        return;
                                    state->checking = false;
                                    if (!error.isEmpty())
                                    {
                                        poll.stop();
                                        dialog.setStatus(error);
                                        return;
                                    }
                                    const int code = data.value("code").toInt();
                                    if (code == 803)
                                    {
                                        poll.stop();
                                        state->key.clear();
                                        invalidateCloudPlayback();
                                        dialog.setBusy(true);
                                        dialog.setStatus(tr("登录已确认，正在读取账号权益…"));
                                        loadAccount(false);
                                    }
                                    else if (code == 802)
                                        dialog.setStatus(tr("扫码成功，请在手机上确认登录。"));
                                    else if (code == 800)
                                    {
                                        poll.stop();
                                        dialog.setStatus(tr("二维码已过期，请点击刷新。"));
                                    }
                                    else if (code == 801)
                                        dialog.setStatus(tr("等待你使用网易云音乐 App 扫码…"));
                                    else
                                    {
                                        poll.stop();
                                        dialog.setStatus(tr("登录状态暂不可用，请刷新二维码。"));
                                    }
                                });
            });
    connect(&dialog, &NetEaseLoginDialog::logoutRequested, &dialog,
            [this, &dialog, state, &poll]()
            {
                ++state->generation;
                poll.stop();
                state->key.clear();
                invalidateCloudPlayback();
                dialog.setBusy(true);
                netease()->post("/logout", {}, &dialog,
                                [this, &dialog](const QJsonObject &, const QString &error)
                                {
                                    dialog.setBusy(false);
                                    if (!error.isEmpty())
                                    {
                                        dialog.setStatus(error);
                                        return;
                                    }
                                    applyCloudAccount({});
                                    dialog.setAccount(false, {}, 0);
                                    dialog.setStatus(
                                        tr("已退出网易云。点击刷新二维码可重新登录。"));
                                });
            });
    dialog.setBusy(true);
    QTimer::singleShot(0, &dialog, [loadAccount]() { loadAccount(true); });
    dialog.exec();
    poll.stop();
    ++state->generation;
}

QString MainWindow::lyricSettingsKey(const QUrl &url) const
{
    return QStringLiteral("lyrics/") +
           QString::fromLatin1(QCryptographicHash::hash(url.toString(QUrl::FullyEncoded).toUtf8(),
                                                        QCryptographicHash::Sha256)
                                   .toHex());
}
