#include "mainwindow.h"
#include "focusdialog.h"
#include "focusmodelclient.h"
#include "focussession.h"
#include "playerwindowview.h"
#include "appbackendclient.h"
#include "neteaseclient.h"
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSet>
#include <QSqlQuery>
#include <QTimer>

namespace {
QString timeLeft(int seconds)
{
    return QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
}

void MainWindow::setupFocus()
{
    focusSession = new FocusSession(this);
    focusSession->setObjectName("focusSession");
    connect(ui->focusButton, &QPushButton::clicked, this, &MainWindow::openFocus);
    connect(focusSession, &FocusSession::trackRequested, this, [this](const QString &key) {
        const auto plan = focusSession->plan();
        for (const auto &track : plan.tracks) {
            if (track.key != key)
                continue;
            if (track.url.isLocalFile() &&
                (!QFileInfo(track.url.toLocalFile()).isFile() ||
                 !QFileInfo(track.url.toLocalFile()).isReadable())) {
                failFocusTrack(key);
                return;
            }
            const int index = appendTrack(track.url, track.title, track.artist, track.songId);
            QScopedValueRollback<bool> dispatch(loadingFocusTrack, true);
            playTrack(index, true, !focusSession->isPaused());
            return;
        }
        failFocusTrack(key);
    });
    connect(focusSession, &FocusSession::changed, this, &MainWindow::updateFocusStatus);
    connect(focusSession, &FocusSession::finished, this,
            [this](const QString &reason, bool completed) {
                ++mediaGeneration;
                playbackRequested = false;
                resolvingTrack = false;
                p_PlayerObject->pause();
                ui->focusStatusLabel->setText(completed ? tr("专注完成 · 好好休息")
                                                       : tr("专注计划已结束"));
                ui->statusLabel->setText(reason);
                ui->shuffleButton->setEnabled(true);
                ui->repeatButton->setEnabled(true);
                if (focusDialog)
                    focusDialog->setStatus(reason, false);
            });
}

void MainWindow::updateFocusPlaybackState()
{
    if (!focusSession || !focusSession->isActive())
        return;
    const auto status = p_PlayerObject->mediaStatus();
    focusSession->setPlaybackRunning(
        p_PlayerObject->state() == QMediaPlayer::PlayingState &&
        preparedTrack == p_PlayerList->currentIndex() && !resolvingTrack && playbackRequested &&
        status != QMediaPlayer::StalledMedia && status != QMediaPlayer::LoadingMedia &&
        status != QMediaPlayer::EndOfMedia && status != QMediaPlayer::InvalidMedia);
}

void MainWindow::updateFocusStatus()
{
    const bool active = focusSession && focusSession->isActive();
    const int remaining = focusSession ? focusSession->remainingSeconds() : 0;
    const bool paused = active && focusSession->isPaused();
    const QString state = paused ? tr("已暂停")
                                : focusSession && focusSession->isRunning() ? tr("专注中")
                                                                          : tr("正在准备");
    if (active)
        ui->focusStatusLabel->setText(tr("%1 · 剩余 %2").arg(state, timeLeft(remaining)));
    if (focusDialog)
        focusDialog->setSessionStatus(state, remaining, active, paused);
}

void MainWindow::failFocusTrack(const QString &key)
{
    if (!focusSession || !focusSession->isActive() || focusSession->currentKey() != key)
        return;
    const quint64 generation = focusSession->generation();
    // Defer failures so a batch of missing/denied tracks cannot recurse through
    // the media backend, and reject errors from a replaced plan.
    QTimer::singleShot(0, focusSession, [this, generation, key]() {
        if (focusSession->isActive() && focusSession->generation() == generation &&
            focusSession->currentKey() == key)
            focusSession->next(true);
    });
}

QVector<FocusTrack> MainWindow::focusCandidates(const QString &source) const
{
    QVector<FocusTrack> candidates;
    QSet<QString> favorites;
    QSqlQuery query;
    query.setForwardOnly(true);
    if (query.exec("SELECT song_id, song_name, artist_name, media_url FROM favorite_songs "
                   "ORDER BY id DESC LIMIT 500")) {
        while (query.next()) {
            const QString key = query.value(3).toString();
            favorites.insert(key);
            if (source == "favorites") {
                FocusTrack track;
                track.key = key;
                track.url = QUrl(key);
                track.title = query.value(1).toString();
                track.artist = query.value(2).toString();
                track.songId = query.value(0).toLongLong();
                track.durationMs = trackDurations.value(key);
                track.favorite = true;
                candidates.append(track);
            }
        }
    }
    if (source != "favorites") {
        for (const auto &item : tracks) {
            if (candidates.size() >= 500)
                break;
            FocusTrack track;
            track.key = item.url.toString(QUrl::FullyEncoded);
            track.url = item.url;
            track.title = item.title;
            track.artist = item.artist;
            track.songId = item.id;
            track.durationMs = trackDurations.value(track.key);
            track.favorite = favorites.contains(track.key);
            candidates.append(track);
        }
    }
    return FocusStore(QSqlDatabase::database()).applyLabels(currentProfile, candidates);
}

bool MainWindow::startFocusPlan(const FocusPlan &plan)
{
    FocusStore store(QSqlDatabase::database());
    const FocusPlan checked = FocusPlanner::build(
        plan.request, focusCandidates(plan.request.source), store.rules(currentProfile, plan.request.scene));
    if (!checked.error.isEmpty() || checked.tracks.size() != plan.tracks.size())
        return false;
    for (int i = 0; i < checked.tracks.size(); ++i) {
        if (checked.tracks.at(i).key != plan.tracks.at(i).key)
            return false;
    }
    endRadioPlayback(tr("已切换到专注计划，本次电台歌单仍保留。"));
    const bool started = focusSession->start(checked);
    if (started) {
        ui->shuffleButton->setEnabled(false);
        ui->repeatButton->setEnabled(false);
        updateFocusStatus();
    }
    return started;
}

void MainWindow::rememberTrackDuration(qint64 duration)
{
    const int index = p_PlayerList->currentIndex();
    if (duration <= 0 || index < 0 || index >= tracks.size() || preparedTrack != index || activeTrial)
        return;
    const auto &item = tracks.at(index);
    const QString key = item.url.toString(QUrl::FullyEncoded);
    trackDurations.insert(key, duration);
    FocusTrack track;
    track.key = key;
    FocusStore store(QSqlDatabase::database());
    const auto labels = store.applyLabels(currentProfile, {track});
    if (!labels.isEmpty())
        store.setTrackLabels(currentProfile, key, labels.first().instrumental, labels.first().live,
                             duration);
}

void MainWindow::logoutLocalAccount()
{
    if (focusSession)
        focusSession->cancel(tr("已退出探歌账户，专注计划已结束"));
    if (AppBackendClient::enabled()) {
        if (focusModel)
            focusModel->cancel();
        AppBackendClient::instance()->logout(this, [this](const QString &error) {
            if (!AppBackendClient::instance()->authenticated() && !error.isEmpty())
                ui->statusLabel->setText(error);
        });
        // authChanged normally switches synchronously. Avoid doing it twice,
        // which would discard the temporary-space warning after a disk failure.
        if (currentProfile != "guest")
            switchToGuestProfile();
        return;
    }
    switchToGuestProfile();
}

void MainWindow::switchToGuestProfile()
{
    if (focusSession)
        focusSession->cancel(tr("已退出探歌账户，专注计划已结束"));
    if (currentProfile == "guest") {
        return;
    }
    // Detach the old account before trying any disk operation. Even an unusable
    // guest file must never leave private account data active after logout.
    auto database = QSqlDatabase::database(QSqlDatabase::defaultConnection, false);
    database.close();
    database.setDatabaseName(QStringLiteral(":memory:"));
    currentProfile = "guest";
    NeteaseClient::setAccountProfile(currentProfile);
    invalidateCloudPlayback();
    applyCloudAccount({});
    isSign = false;
    lastHistoryUrl.clear();
    ui->label_DisplayUser->setText(tr("访客 · 数据仅保存在本机"));
    ui->pushButton_SignIn->setText(tr("  登录探歌"));
    QString error;
    const bool persistent = openProfileDatabase("guest", &error);
    bool temporary = false;
    if (!persistent) {
        QString temporaryError;
        temporary = openTemporaryGuestDatabase(&temporaryError);
        if (!temporaryError.isEmpty())
            error += QStringLiteral(" / ") + temporaryError;
        ui->label_DisplayUser->setText(tr("访客 · 临时空间，不保存数据"));
    }
    if (p_PlayerList->currentIndex() >= 0)
        updateCurrentSong(p_PlayerList->currentIndex());
    ui->statusLabel->setText(
        persistent ? tr("已切换到访客空间；原有账户的收藏和偏好保留。")
        : temporary ? tr("已退出探歌。访客数据暂不可用，已切换临时空间，关闭软件后不保留；原有账户数据未删除。%1").arg(error)
                    : tr("已退出探歌，访客数据功能暂不可用；原有账户数据未删除。%1").arg(error));
    refreshProfilePages();
}

void MainWindow::updateBackendFocusStatus()
{
    if (!focusDialog)
        return;
    auto *backend = AppBackendClient::instance();
    if (!AppBackendClient::enabled()) {
        focusDialog->setModelAvailable(false);
        return;
    }
    if (!backend->authenticated())
        focusDialog->setParserStatus(tr("登录探歌后使用 AI"));
    else if (backend->agentReady())
        focusDialog->setParserStatus(tr("探歌 AI"));
    else
        focusDialog->setParserStatus(tr("AI 服务暂不可用"));
}

void MainWindow::openFocus()
{
    if (focusDialog) {
        FocusStore store(QSqlDatabase::database());
        focusDialog->setCandidates(focusCandidates("queue"), focusCandidates("favorites"));
        focusDialog->setRules(store.rules(currentProfile));
        updateBackendFocusStatus();
        updateFocusStatus();
        showContentPage(focusDialog, ui->focusButton);
        return;
    }
    FocusStore store(QSqlDatabase::database());
    auto *dialog = new FocusDialog(focusCandidates("queue"), focusCandidates("favorites"),
                                   store.rules(currentProfile), this);
    focusDialog = dialog;
    dialog->setEmbedded(true);
    dialog->setModelAvailable(FocusModelClient::configured());
    updateBackendFocusStatus();
    const auto preview = [this, dialog](const FocusRequest &request) {
        FocusStore store(QSqlDatabase::database());
        FocusPlan plan = FocusPlanner::build(request, focusCandidates(request.source),
                                             store.rules(currentProfile, request.scene));
        plan.notes.append(tr("每次最多从 500 首候选中选择；按顺序循环至计时结束，暂停不计时。"));
        plan.notes.append(tr("在线歌曲会在播放时检查当前权益；无法播放或仅试听时跳过。"));
        dialog->setPlan(plan);
    };
    connect(dialog, &FocusDialog::previewRequested, dialog, preview);
    connect(dialog, &FocusDialog::parseRequested, dialog,
            [this, dialog, preview](const QString &text, const FocusRequest &base) {
                if (AppBackendClient::enabled()) {
                    if (!focusModel)
                        focusModel = new FocusModelClient(this);
                    const quint64 generation = dialog->generation();
                    dialog->setBusy(true);
                    focusModel->parse(text, base, dialog,
                        [dialog, generation, preview](const FocusRequest &request,
                                                            const QString &error) {
                            dialog->setBusy(false);
                            if (!dialog->isCurrentGeneration(generation))
                                return;
                            if (!error.isEmpty()) {
                                dialog->setStatus(error, true);
                                return;
                            }
                            dialog->setRequest(request);
                            preview(request);
                        });
                    return;
                }
                const QString configError = AppBackendClient::configurationError();
                if (!configError.isEmpty()) {
                    dialog->setStatus(configError, true);
                    return;
                }
                QString error;
                const FocusRequest request = FocusPlanner::parse(text, base, &error);
                if (!error.isEmpty()) {
                    dialog->setStatus(error, true);
                    return;
                }
                dialog->setRequest(request);
                preview(request);
            });
    connect(dialog, &FocusDialog::startRequested, dialog, [this, dialog](const FocusPlan &plan) {
        if (!startFocusPlan(plan))
            dialog->setStatus(tr("曲库或偏好已变化，请重新预览后开始。"), true);
        else
            dialog->setStatus(tr("计划已开始，切换页面后继续计时；左侧会显示剩余时间。"));
    });
    connect(dialog, &FocusDialog::rememberRequested, dialog,
            [this, dialog](const FocusRequest &request) {
                FocusStore store(QSqlDatabase::database());
                QString error;
                bool ok = true;
                int count = 0;
                auto add = [&](const QString &kind, const QString &value = QString()) {
                    ++count;
                    if (store.addRule(currentProfile, request.scene, kind, value, &error) <= 0)
                        ok = false;
                };
                QSqlDatabase db = QSqlDatabase::database();
                if (!db.transaction()) {
                    dialog->setStatus(tr("暂时无法保存偏好。"), true);
                    return;
                }
                if (request.excludeLive)
                    add("exclude_live");
                if (request.instrumentalOnly)
                    add("instrumental_only");
                for (const auto &artist : request.excludedArtists)
                    add("exclude_artist", artist);
                if (ok)
                    ok = db.commit();
                else
                    db.rollback();
                dialog->setRules(store.rules(currentProfile));
                dialog->setStatus(!ok ? tr("偏好保存失败：%1").arg(error)
                                      : count ? tr("已记住当前场景的条件，可在下方删除。")
                                              : tr("请先选择要记住的条件。"), !ok);
            });
    connect(dialog, &FocusDialog::deleteRuleRequested, dialog, [this, dialog](qint64 id) {
        FocusStore store(QSqlDatabase::database());
        QString error;
        if (!store.removeRule(currentProfile, id, &error)) {
            dialog->setStatus(error, true);
            return;
        }
        dialog->setRules(store.rules(currentProfile));
        dialog->setStatus(tr("已忘记这条偏好。"));
    });
    connect(dialog, &FocusDialog::setTrackTags, dialog,
            [this, dialog](const QString &key, int instrumental, int live) {
                FocusStore store(QSqlDatabase::database());
                QString error;
                if (!store.setTrackLabels(currentProfile, key, instrumental, live,
                                          trackDurations.value(key), &error)) {
                    dialog->setStatus(error, true);
                    return;
                }
                dialog->setCandidates(focusCandidates("queue"), focusCandidates("favorites"));
                dialog->setStatus(tr("曲目标注已保存，请重新预览计划。"));
            });
    connect(dialog, &FocusDialog::pauseResumeRequested, this, [this]() {
        if (focusSession->isActive()) {
            on_pushButton_PlaySong_clicked();
            updateFocusStatus();
        }
    });
    connect(dialog, &FocusDialog::endSessionRequested, this, [this]() {
        focusSession->cancel();
    });
    connect(dialog, &FocusDialog::avoidCurrentTrackRequested, dialog, [this, dialog]() {
        if (!focusSession->isActive())
            return;
        FocusStore store(QSqlDatabase::database());
        QString error;
        const auto request = focusSession->plan().request;
        if (store.addRule(currentProfile, request.scene, "exclude_track",
                          focusSession->currentKey(), &error) <= 0) {
            dialog->setStatus(tr("无法保存偏好：%1").arg(error), true);
            return;
        }
        dialog->setRules(store.rules(currentProfile));
        focusSession->next(true);
        dialog->setStatus(tr("已记住：这个场景避开刚才那首歌。可在偏好列表中删除。"));
    });
    updateFocusStatus();
    connect(dialog, &FocusDialog::cancelRequested, dialog, [this, dialog] {
        if (focusModel) focusModel->cancel();
        dialog->setBusy(false);
    });
    showContentPage(dialog, ui->focusButton);
    if (AppBackendClient::enabled() && AppBackendClient::instance()->authenticated()) {
        AppBackendClient::instance()->refreshAgentStatus(dialog, [this, dialog](const QString &error) {
            updateBackendFocusStatus();
            if (!error.isEmpty()) dialog->setStatus(error, true);
        });
    }
}
