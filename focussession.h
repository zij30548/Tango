#ifndef FOCUSSESSION_H
#define FOCUSSESSION_H

#include "focuscore.h"
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>

// The model never drives playback. This small controller owns the bounded,
// deterministic execution of a validated plan.
class FocusSession : public QObject
{
    Q_OBJECT
public:
    using Clock = std::function<qint64()>;
    explicit FocusSession(QObject *parent = nullptr, Clock clock = {});
    bool start(const FocusPlan &plan);
    bool isActive() const { return active; }
    bool isRunning() const { return running; }
    bool isPaused() const { return paused; }
    int remainingSeconds() const;
    QString currentKey() const;
    FocusPlan plan() const { return currentPlan; }
    quint64 generation() const { return revision; }

public slots:
    void setPlaybackRunning(bool value);
    void setPaused(bool value);
    void next(bool failed = false);
    void previous();
    void cancel(const QString &reason = QString());
    void tick();

signals:
    void trackRequested(QString key);
    void changed();
    void finished(QString reason, bool completed);

private:
    void settle();
    void finish(const QString &reason, bool completed);
    Clock clock;
    QTimer timer;
    FocusPlan currentPlan;
    QSet<QString> failedKeys;
    bool active = false;
    bool running = false;
    bool paused = false;
    int cursor = -1;
    qint64 targetMs = 0;
    qint64 playedMs = 0;
    qint64 lastTick = 0;
    quint64 revision = 0;
};
#endif
