#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "neteasetypes.h"
#include "focuscore.h"
#include "radiotypes.h"
#include <QSet>
#include <QHash>
#include <QJsonObject>
#include <QDeadlineTimer>
#include <QMainWindow>
#include <QMediaPlayer>
#include <QMediaPlaylist>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVector>

class NeteaseClient;
class QAction;
class PlayerWindowView;
class FloatingLyricsWidget;
class QDragEnterEvent;
class QDropEvent;
class FocusSession;
class FocusDialog;
class FocusModelClient;
class RadioClient;
class RadioDialog;
class MusicTaskClient;
class ConversationClient;
class OnlineSearchDialog;
class HistorySong;
class LikeSong;
class RankList;
class QPushButton;
class QEvent;
class QTimer;
struct RadioResult;

class MainWindow : public QMainWindow
{
    Q_OBJECT
    // 原生回归只注入私有截止时间，不提供产品侧绕过有效期的入口。
    friend class LookupContextFlowTest;

  public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    bool isSign = false;
    void addLocalFiles(const QStringList &files);
    bool startFocusPlan(const FocusPlan &plan);
    QVector<FocusTrack> focusCandidates(const QString &source) const;
    QString accountProfile() const { return currentProfile; }
    bool startRadio(const QVector<RadioRecommendation> &songs);

    QMediaPlayer *p_PlayerObject = nullptr;
    QMediaPlaylist *p_PlayerList = nullptr;

  signals:
    void songLiked(QString songInfo);
    void onlinePlaybackResolved(qint64 id, bool trial);
    void onlinePlaybackFailed(qint64 id, QString reason);

  public slots:
    void HandleLCDNumerTimeChangeFunc(qint64 SongDuration);
    void HandleProgressTimeChangeFunc(qint64 SongDuration);
    void HandlePositionChangeFunc(qint64 position);
    void onSongSelected(const QString &songName);
    void onSongSelected1(const QString &songName);
    void updateUsername(QString username);
    void playOnlineSong(const OnlineSong &song);

  private slots:
    void on_pushButton_About_clicked();
    void on_pushButton_AddSong_clicked();
    void on_pushButton_PlaySong_clicked();
    void on_pushButton_StopPlaySong_clicked();
    void on_pushButton_PreviousSong_clicked();
    void on_pushButton_PlayNextSong_clicked();
    void on_pushButton_SearchSong_clicked();
    void on_pushButton_SoundYesNo_clicked();
    void on_horizontalSlider_Volume_valueChanged(int value);
    void on_horizontalSlider_PlayProgress_valueChanged(int value);
    void on_pushButton_LikeSong_clicked();
    void on_pushButton_HistorySong_clicked();
    void on_pushButton_AddLike_clicked();
    void on_pushButton_RankList_clicked();
    void on_pushButton_SignIn_clicked();
    void on_pushButton_Download_2_clicked();
    void on_pushButton_Download_clicked();
    void openNeteaseAccount();
    void logoutNetease();
    void openFocus();
    void logoutLocalAccount();
    void openRadio();
    void bootstrapDevelopmentSession();

  private:
    struct Track
    {
        QUrl url;
        QString title;
        QString artist;
        qint64 id;
        QString album, coverUrl;
    };
    bool initializeDatabase();
    bool openProfileDatabase(const QString &profile, QString *error = nullptr);
    bool openTemporaryGuestDatabase(QString *error);
    QString currentProfile = QStringLiteral("guest");
    FocusSession *focusSession = nullptr;
    QPointer<FocusDialog> focusDialog;
    FocusModelClient *focusModel = nullptr;
    RadioClient *radioClient = nullptr;
    MusicTaskClient *musicTaskClient = nullptr;
    ConversationClient *conversationClient = nullptr;
    QString restoredConversationProfile, visibleConversationId;
    bool radioConversationsClearing = false;
    QString pendingRadioInstruction, activeLegacyConversationTurn, preferenceConversationTurn;
    quint64 pendingRadioSubmission = 0, radioAccountEpoch = 0;
    QTimer *radioUsageTimer = nullptr;
    QPointer<QObject> radioUsageOperation;
    bool radioUsageRefreshPending = false;
    bool legacyAssistantBusy = false;
    bool radioResultIsTask = false;
    struct RoutedRadioTurn {
        QString instruction, preferenceBase, confirmationTurn;
        QJsonObject plan;
        QHash<QString, QString> actionStates;
        qint64 baseRevision = 0;
        bool restored = false;
    };
    QHash<QString, RoutedRadioTurn> routedRadioTurns;
    QHash<QString, QString> routePreferenceBases;
    QHash<QString, qint64> routePreferenceRevisions;
    QHash<QString, QJsonObject> pendingRadioRouteRestores;
    QStringList radioRouteRestoreOrder;
    QSet<QString> observedRadioRequests;
    QSet<QString> observedRadioAdoptionRequests;
    QHash<QString, QPair<QString, int>> radioAdoptionRequests;
    QStringList routedRadioOrder;
    QString pendingRouteMemoryTurn, pendingRouteMemoryAction;
    QHash<QString, QPair<QString, QString>> routedRadioRequests;
    QPointer<QObject> radioLookupOperation;
    quint64 radioLookupGeneration = 0;
    QJsonObject radioLookupContext;
    QVector<RadioRecommendation> radioLookupPool;
    QHash<QString, QJsonObject> radioLookupInputs;
    QString radioLookupContextProfile, radioLookupContextDevice, radioLookupContextConversation, radioLookupContextLastTurn;
    QDeadlineTimer radioLookupContextDeadline;
    QByteArray pendingClearQueueDigest;
    QString pendingClearQueueConversation;
    QSet<QString> syncedConversationTasks;
    QHash<QString, QString> conversationTaskTurns;
    struct PendingConversationTask {
        QString taskId, turnId, instruction;
        int revision = 0;
    } pendingConversationTask;
    QString restoredTaskProfile;
    QString pendingAdoptTask;
    int pendingAdoptVersion = 0;
    QPointer<RadioDialog> radioDialog;
    QStringList radioHistory;
    QVector<qint64> radioSeen;
    QVector<RadioRecommendation> radioRecommendations;
    QString radioTitle, radioNote, radioPlaybackStatus;
    bool radioPlayback = false;
    bool radioPaused = false;
    QSet<qint64> radioFailed;
    void setupRadio();
    void advanceRadio(int step, bool failed = false);
    void failRadioTrack(qint64 id);
    void endRadioPlayback(const QString &reason = QString());
    void updateRadioAvailability();
    void scheduleRadioUsageRefresh();
    void refreshRadioUsage();
    void applyAssistantResult(const RadioResult &, const QString &error, const QString &request,
                              const QString &conversationTurn = QString());
    void applyMusicTaskSnapshot(const QJsonObject &snapshot);
    void restoreRadioConversation();
    void applyConversationSnapshot(const QJsonObject &snapshot);
    void clearRadioConversations();
    void discardRadioConversationExecution();
    void restoreRadioRoutes();
    void restoreNextRadioRoute();
    void applyRestoredRadioRoute(const QString &turnId, const QJsonObject &plan);
    bool setRadioRouteAction(const QString &turnId, const QString &actionId, const QString &state);
    void observeRadioRequests();
    void executeRadioInstruction(const QString &instruction, const QString &turnId = QString(),
                                 const QString &dispatchHint = QString());
    void applyRadioRoute(const QString &turnId, const QJsonObject &plan);
    void continueRadioRoute(const QString &turnId);
    void lookupRadioSong(const QString &turnId, const QString &actionId, const QJsonObject &parameters);
    void lookupRadioArtist(const QString &turnId, const QString &actionId, const QJsonObject &parameters,
                           const QJsonObject &resolved = {});
    void lookupRadioPlaylists(const QString &turnId, const QString &actionId, const QJsonObject &parameters);
    void openRadioPlaylist(qint64 id, const QString &name);
    void refineRadioSong(const QString &turnId, const QString &actionId, const QJsonObject &parameters);
    void clearRadioLookupContext(bool clearPreview = false);
    QJsonObject radioLookupContextFor(const QString &turnId) const;
    bool saveRadioLookupContext(const QString &turnId, const QString &actionId,
                               const QVector<RadioRecommendation> &pool, bool initial);
    bool executeRadioChatCommand(const QString &request, const QString &turnId);
    QByteArray radioQueueDigest() const;
    void decideRadioPreference(const QString &actionId, bool confirmed, const QString &preference);
    void appendRadioReply(const QString &text, const QString &turnId = QString());
    QString latestConversationTurn() const;
    QString resolveTaskConversationTurn(const QJsonObject &snapshot, bool bindPending = true);
    void adoptMusicTask();
    QHash<QString, qint64> trackDurations;
    bool loadingFocusTrack = false;
    void setupFocus();
    void updateFocusStatus();
    void updateFocusPlaybackState();
    void failFocusTrack(const QString &key);
    void rememberTrackDuration(qint64 duration);
    void switchToGuestProfile();
    void updateBackendFocusStatus();
    int appendTrack(const QUrl &url, const QString &title, const QString &artist, qint64 id);
    void updateCurrentSong(int index);
    void recordCurrentSong();
    QVector<Track> tracks;
    QHash<QString, int> trackIndexes;
    QString lastHistoryUrl;
    void loadTrackLyrics(const QUrl &url);
    QString lyricSettingsKey(const QUrl &url) const;
    void importLyrics();
    bool readLyricsFile(const QString &path);
    void updatePlaybackMode();
    PlayerWindowView *ui;
    QPushButton *developmentRetryButton = nullptr;
    bool developmentBootstrapPending = false;
    quint64 developmentBootstrapGeneration = 0;
    QPointer<QObject> developmentBootstrapOperation;
    QPointer<OnlineSearchDialog> searchPage;
    QPointer<HistorySong> historyPage;
    QPointer<LikeSong> favoritesPage;
    QPointer<RankList> rankingsPage;
    QPointer<QObject> searchOperation;
    QString searchQuery;
    quint64 searchGeneration = 0;
    void showContentPage(QWidget *page, QPushButton *navButton = nullptr);
    void fetchSearchPage(int offset);
    void cancelSearch();
    void refreshProfilePages();
    FloatingLyricsWidget *floatingLyrics = nullptr;
    NeteaseClient *cloudClient = nullptr;
    NeteaseClient *netease();
    void playTrack(int index, bool forceRefresh = false, bool autoplay = true);
    void prepareTrack(int index);
    void handleEndOfMedia();
    void applyCloudAccount(const QJsonObject &account);
    void invalidateCloudPlayback();
    void openOfficialSong();
    bool isOnlineTrack(int index) const;
    int mediaGeneration = 0;
    int preparedTrack = -1;
    bool playbackRequested = false;
    bool resolvingTrack = false;
    qint64 resolvedAt = 0;
    qint64 lyricsOffset = 0;
    bool activeTrial = false;
    QString cloudNickname;
    bool cloudLoggedIn = false;
    QAction *cloudLogoutAction = nullptr;
    int repeatMode = 0;
    QString lyricsTrackUrl;
    QHash<QString, QString> lyricFiles;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};
#endif // MAINWINDOW_H
