#include "focussession.h"
#include <chrono>

FocusSession::FocusSession(QObject *parent, Clock clock) : QObject(parent), clock(std::move(clock))
{
    if (!this->clock)
        this->clock = []() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        };
    timer.setInterval(1000);
    connect(&timer, &QTimer::timeout, this, &FocusSession::tick);
}

bool FocusSession::start(const FocusPlan &plan)
{
    if (!plan.error.isEmpty() || plan.tracks.isEmpty() || plan.tracks.size() > 1000 ||
        plan.request.minutes < 1 || plan.request.minutes > 180)
        return false;
    QSet<QString> keys;
    for (const auto &track : plan.tracks) {
        if (track.key.isEmpty() || track.url.isEmpty() || keys.contains(track.key))
            return false;
        keys.insert(track.key);
    }
    if (active)
        finish(tr("已替换专注计划"), false);
    currentPlan = plan;
    targetMs = qint64(plan.request.minutes) * 60000;
    playedMs = 0;
    lastTick = clock();
    cursor = 0;
    failedKeys.clear();
    active = true;
    running = false;
    paused = false;
    ++revision;
    emit changed();
    emit trackRequested(currentKey());
    return true;
}

int FocusSession::remainingSeconds() const
{
    return int(qMax<qint64>(0, targetMs - playedMs + 999) / 1000);
}

QString FocusSession::currentKey() const
{
    return cursor >= 0 && cursor < currentPlan.tracks.size() ? currentPlan.tracks.at(cursor).key
                                                          : QString();
}

void FocusSession::settle()
{
    const qint64 now = clock();
    if (active && running) {
        // A sleeping/suspended desktop must not consume an entire focus plan.
        playedMs += qBound<qint64>(0, now - lastTick, 2000);
        if (playedMs >= targetMs) {
            playedMs = targetMs;
            finish(tr("专注完成，休息一下吧。"), true);
        }
    }
    lastTick = now;
}

void FocusSession::setPlaybackRunning(bool value)
{
    if (!active)
        return;
    settle();
    if (!active)
        return;
    running = value && !paused;
    if (running)
        timer.start();
    else
        timer.stop();
    emit changed();
}

void FocusSession::setPaused(bool value)
{
    if (!active)
        return;
    paused = value;
    if (paused)
        setPlaybackRunning(false);
    else
        emit changed();
}

void FocusSession::tick()
{
    if (!active)
        return;
    settle();
    if (active)
        emit changed();
}

void FocusSession::next(bool failed)
{
    if (!active)
        return;
    setPlaybackRunning(false);
    if (!active)
        return;
    if (failed)
        failedKeys.insert(currentKey());
    const int count = currentPlan.tracks.size();
    for (int attempts = 0; attempts < count; ++attempts) {
        cursor = (cursor + 1) % count;
        if (!failedKeys.contains(currentKey())) {
            ++revision;
            emit changed();
            emit trackRequested(currentKey());
            return;
        }
    }
    finish(tr("计划中的歌曲均不可播放，已结束；请补充本地音乐或检查账号权益。"), false);
}

void FocusSession::previous()
{
    if (!active)
        return;
    setPlaybackRunning(false);
    if (!active)
        return;
    const int count = currentPlan.tracks.size();
    for (int attempts = 0; attempts < count; ++attempts) {
        cursor = (cursor - 1 + count) % count;
        if (!failedKeys.contains(currentKey())) {
            ++revision;
            emit changed();
            emit trackRequested(currentKey());
            return;
        }
    }
    finish(tr("计划中没有可播放的歌曲。"), false);
}

void FocusSession::cancel(const QString &reason)
{
    if (active)
        finish(reason.isEmpty() ? tr("专注计划已结束") : reason, false);
}

void FocusSession::finish(const QString &reason, bool completed)
{
    active = false;
    running = false;
    ++revision;
    timer.stop();
    emit changed();
    emit finished(reason, completed);
}
