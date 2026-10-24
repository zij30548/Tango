#ifndef FOCUSDIALOG_H
#define FOCUSDIALOG_H

#include "focuscore.h"
#include <QDialog>

class QHideEvent;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QWidget;

class FocusDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FocusDialog(const QVector<FocusTrack> &queue,
                         const QVector<FocusTrack> &favorites,
                         const QVector<FocusRule> &rules,
                         QWidget *parent = nullptr);

    void setEmbedded(bool embedded);
    void resetForAccount();
    FocusRequest request() const;
    void setCandidates(const QVector<FocusTrack> &queue,
                       const QVector<FocusTrack> &favorites);
    void setRules(const QVector<FocusRule> &rules);
    void setRequest(const FocusRequest &request);
    void setPlan(const FocusPlan &plan);
    void setStatus(const QString &message, bool error = false);
    void setModelAvailable(bool available);
    void setParserStatus(const QString &text);
    void setBusy(bool busy);
    void setSessionStatus(const QString &state, int remainingSeconds,
                          bool active, bool paused);

    // Capture this before asynchronous parsing; reject its result if the user
    // has changed the request, candidates, or saved rules in the meantime.
    quint64 generation() const { return revision; }
    bool isCurrentGeneration(quint64 value) const { return value == revision; }

public slots:
    void reject() override;

protected:
    void hideEvent(QHideEvent *event) override;

signals:
    void cancelRequested();
    void previewRequested(FocusRequest request);
    void parseRequested(QString text, FocusRequest base);
    void startRequested(FocusPlan plan);
    void rememberRequested(FocusRequest request);
    void deleteRuleRequested(qint64 id);
    void setTrackTags(QString trackKey, int instrumental, int live);
    void pauseResumeRequested();
    void endSessionRequested();
    void avoidCurrentTrackRequested();

private:
    const QVector<FocusTrack> &currentCandidates() const;
    void invalidatePreview();
    void updateControls();
    void updateLibrary();
    void updateRules();
    void updateTagControls();
    void requestPreview();
    QString sceneLabel(const QString &scene) const;

    QVector<FocusTrack> queueTracks;
    QVector<FocusTrack> favoriteTracks;
    QVector<FocusRule> savedRules;
    FocusPlan displayedPlan;
    quint64 revision = 0;
    bool embedded = false;
    bool previewValid = false;
    bool applyingRequest = false;
    bool busy = false;
    bool sessionActive = false;

    QComboBox *scene;
    QSpinBox *minutes;
    QComboBox *source;
    QLineEdit *instruction;
    QCheckBox *excludeLive;
    QCheckBox *instrumentalOnly;
    QLineEdit *excludedArtists;
    QLabel *parserMode;
    QLabel *status;
    QLabel *planSummary;
    QLabel *planNotes;
    QPushButton *parseButton;
    QPushButton *previewButton;
    QPushButton *startButton;
    QTableWidget *planTable;
    QTableWidget *libraryTable;
    QComboBox *instrumentalTag;
    QComboBox *liveTag;
    QPushButton *saveTagsButton;
    QLabel *rulesHeading;
    QListWidget *rulesList;
    QPushButton *rememberButton;
    QPushButton *deleteRuleButton;
    QWidget *sessionPanel;
    QLabel *sessionLabel;
    QPushButton *pauseButton;
    QPushButton *endButton;
    QPushButton *avoidCurrentButton;
    QTabWidget *tabs;
};

#endif // FOCUSDIALOG_H
