#ifndef RADIODIALOG_H
#define RADIODIALOG_H

#include "radiotypes.h"
#include "assistanttypes.h"
#include <QDialog>
#include <QPair>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QHash>
#include <QImage>
#include <QPointer>

class QLabel;
class QCloseEvent;
class QHideEvent;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTextBrowser;
class QWidget;
class CoverArtCache;

// 视图只展示状态和发送显式意图；播放、存储与异步请求仍由主窗口核验账号及版本。
class RadioDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RadioDialog(QWidget *parent = nullptr, CoverArtCache *artwork = nullptr);
    void setEmbedded(bool embedded);
    void setAvailability(bool loggedIn, bool configured, const QString &status);
    void setAccountUsage(const QJsonObject &usage);
    void setBusy(bool busy, const QString &stage = QString());
    void appendMessage(const QString &speaker, const QString &text);
    void setRecommendations(const QString &title, const QString &note,
                            const QVector<RadioRecommendation> &recommendations, int visibleLimit = 12);
    void setPlaylistCandidates(const QJsonArray &playlists);
    void setPreference(const QString &summary);
    void resetForAccount();
    QVector<RadioRecommendation> recommendations() const;
    void setPlaybackStatus(const QString &status);
    void setArtistChoices(const QVector<AssistantArtist> &artists);
    void clearArtistChoices();
    void reviewPreference(const QString &proposal);
    void setPreferenceProposal(const QString &actionId, const QString &operation, const QString &proposal);
    void clearPreferenceProposal(const QString &actionId = QString());
    void setRouteObservationAvailable(const QString &messageId);
    void setTaskMode(bool enabled);
    void setTaskSnapshot(const QJsonObject &snapshot);
    void setConversation(const QJsonArray &conversation);
    void setConversationMode(bool enabled);
    void setEarlierMessagesAvailable(bool available);
    void setConversationsClearing(bool clearing);
    quint64 currentSubmissionId() const;
    void submissionAccepted(const QString &instruction, quint64 submissionId = 0);
    void submissionFailed(const QString &instruction, quint64 submissionId = 0);
    // 仅在原话已写入会话后调用，不能跳过消息持久化或自动确认。
    bool executeChatAction(const QString &instruction);

public slots:
    void reject() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;

signals:
    void generateRequested(QString instruction);
    void earlierMessagesRequested();
    void clearConversationsRequested();
    void cancelRequested();
    void playRequested(QVector<RadioRecommendation> recommendations);
    void trackActivated(OnlineSong song);
    void playlistActivated(qint64 id, QString name);
    void rememberRequested();
    void clearPreferenceRequested();
    void loginRequested();
    void avoidArtistRequested(QString artist);
    void artistSelected(AssistantArtist artist);
    void preferenceEdited(QString preference);
    void preferenceProposalDecided(QString actionId, bool confirmed, QString preference);
    void routeObservationRequested(QString messageId);
    void taskActionRequested(QString action);

private:
    void updateControls();
    void submit(const QString &instruction);
    QString selectedArtist() const;
    void editPreference();
    void renderConversation(const QString &text);
    QString chatContext(bool includeSongs = true) const;
    QString constraintWarningText() const;
    bool canActivateTrack() const;
    QString conversationHtml();
    QString artworkHtml(const QString &kind, int row, const QString &coverUrl);
    QPointer<CoverArtCache> artworkCache;
    QHash<QString, QImage> conversationArtwork;
    bool artworkRefreshScheduled = false;
    QJsonArray playlistCandidates;
    QHash<QString, QJsonObject> conversationPlaylists;

    bool embedded = false;
    bool loggedIn = false;
    bool configured = false;
    bool busy = false;
    bool closing = false;
    bool taskMode = false;
    bool conversationMode = false;
    bool submissionPending = false;
    QString pendingInstruction;
    int pendingDraftRevision = -1;
    quint64 submissionSerial = 0;
    quint64 pendingSubmissionId = 0;
    quint64 pendingAccountRevision = 0;
    bool taskCanUndo = false;
    bool taskHasLocks = false;
    QSet<qint64> taskLockedIds;
    bool taskConnectionInterrupted = false;
    bool taskLocalResumeRequired = false;
    QString taskStatus;
    QStringList taskConstraintWarnings;
    QWidget *taskPanel;
    QLabel *taskSummary;
    QLabel *taskChecks;
    QLabel *taskBudget;
    QPushButton *taskRestore;
    QPushButton *taskNew;
    QPushButton *taskPause;
    QPushButton *taskResume;
    QPushButton *taskLock;
    QPushButton *taskUndo;
    quint64 accountRevision = 0;
    QString availabilityStatus;
    QString preferenceSummary;
    QString proposalActionId, proposalOperation, routeObservationMessage;
    bool proposalDecisionPending = false;
    QWidget *preferenceProposalPanel;
    QLineEdit *proposedPreference;
    QPushButton *confirmProposalButton, *cancelProposalButton, *routeObserveButton;
    QLabel *proposalNote;
    QVector<RadioRecommendation> songs;
    QVector<AssistantArtist> artists;
    QVector<QPair<QString, QString>> messages;
    QString conversationText;
    QString renderedConversationHtml;
    bool conversationsClearing = false;
    quint64 recommendationRevision = 0;
    QHash<QString, OnlineSong> conversationTracks;
    bool earlierAvailable = false;

    QLabel *availability;
    QLabel *quota;
    QLabel *preference;
    QLabel *playlistTitle;
    QLabel *playlistNote;
    QLabel *stage;
    QLabel *playback;
    QPlainTextEdit *transcript;
    QTextBrowser *conversationView;
    QWidget *conversationToolbar;
    QPushButton *clearConversationsButton;
    QPlainTextEdit *input;
    QLabel *inputCounter;
    QPushButton *earlierMessages;
    QPushButton *newConversationButton;
    QWidget *resultsPanel;
    QWidget *composerPanel;
    QProgressBar *progress;
    QTableWidget *table;
    QWidget *preferencePanel;
    QWidget *artistPanel;
    QComboBox *artistCombo;
    QPushButton *loginButton;
    QPushButton *sendButton;
    QPushButton *cancelButton;
    QPushButton *playButton;
    QPushButton *rememberButton;
    QPushButton *clearPreferenceButton;
    QPushButton *editPreferenceButton;
    QPushButton *lookupArtistButton;
    QPushButton *avoidArtistButton;
};

#endif
