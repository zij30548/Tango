#include "focuscore.h"

#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {
void clearError(QString *error)
{
    if (error) error->clear();
}

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

bool validScene(const QString &scene)
{
    return scene == QStringLiteral("work") || scene == QStringLiteral("study")
           || scene == QStringLiteral("rest");
}

bool validRequest(const FocusRequest &request, QString *error)
{
    if (request.minutes < 1 || request.minutes > 180)
        return fail(error, QStringLiteral("专注时长请设为 1 至 180 分钟。"));
    if (!validScene(request.scene))
        return fail(error, QStringLiteral("当前支持工作、学习和休息三个场景。"));
    if (request.source != QStringLiteral("queue") && request.source != QStringLiteral("favorites"))
        return fail(error, QStringLiteral("当前仅支持从当前队列或收藏中选歌。"));
    if (request.excludedArtists.size() > 100)
        return fail(error, QStringLiteral("排除歌手数量过多，请减少到 100 位以内。"));
    return true;
}

QString normalize(const QString &value)
{
    return value.simplified().toCaseFolded();
}

bool artistMatches(const QString &credit, const QString &excluded)
{
    const QString wanted = normalize(excluded);
    if (wanted.isEmpty()) return false;
    if (normalize(credit) == wanted) return true;
    const QStringList artists = credit.split(QRegularExpression(QStringLiteral("\\s*[/、,&;，；]\\s*")),
                                              Qt::SkipEmptyParts);
    for (const QString &artist : artists)
        if (normalize(artist) == wanted) return true;
    return false;
}

bool looksLive(const QString &title)
{
    static const QRegularExpression version(QStringLiteral("\\blive\\b|现场|演唱会"),
                                            QRegularExpression::CaseInsensitiveOption);
    return title.contains(version);
}

bool validProfile(const QString &profile, QString *error)
{
    if (profile.trimmed().isEmpty() || profile.size() > 256)
        return fail(error, QStringLiteral("账户标识无效，无法读取或保存偏好。"));
    return true;
}

bool queryError(const QSqlQuery &query, QString *error)
{
    return fail(error, QStringLiteral("偏好数据库操作失败：%1").arg(query.lastError().text()));
}
} // namespace

FocusRequest FocusPlanner::parse(const QString &text, const FocusRequest &base, QString *error)
{
    FocusRequest request = base;
    QString localError;
    parse(text, base, &request, &localError);
    if (error) *error = localError;
    return request;
}

bool FocusPlanner::parse(const QString &text, const FocusRequest &base, FocusRequest *output,
                         QString *error)
{
    clearError(error);
    if (!output) return fail(error, QStringLiteral("缺少计划输出。"));
    FocusRequest request = base;
    request.originalText = text.trimmed();
    if (text.size() > 1000) return fail(error, QStringLiteral("请将计划要求缩短到 1000 字以内。"));
    QString remaining = text.toLower().trimmed();
    if (remaining.isEmpty()) {
        if (!validRequest(request, error)) return false;
        *output = request;
        return true;
    }

    // This is deliberately a small, explicit grammar, not a claim to understand
    // arbitrary mood, genre, artist or audio attributes.
    static const QRegularExpression minutes(QStringLiteral("(?<![\\d.])(\\d+)\\s*(?:分钟|min(?:utes?)?)(?![a-z])"));
    auto durations = minutes.globalMatch(remaining);
    int parsedMinutes = 0;
    bool foundDuration = false;
    while (durations.hasNext()) {
        const auto match = durations.next();
        bool ok = false;
        const int value = match.captured(1).toInt(&ok);
        if (!ok || (foundDuration && parsedMinutes != value))
            return fail(error, QStringLiteral("请只指定一个明确的分钟数。"));
        parsedMinutes = value;
        foundDuration = true;
    }
    if (foundDuration) request.minutes = parsedMinutes;
    remaining.remove(minutes);

    static const QRegularExpression artistDirective(
        QStringLiteral("(?:排除歌手|避开歌手)\\s*[：:]?\\s*([^，,。；;\\n]+)"));
    auto artists = artistDirective.globalMatch(remaining);
    while (artists.hasNext()) {
        const auto match = artists.next();
        if (match.captured(1).contains(QRegularExpression(QStringLiteral("不要|排除|仅|分钟|纯音乐|工作|学习|休息"))))
            return fail(error, QStringLiteral("歌手列表与其他条件请用逗号分开。"));
        const auto names = match.captured(1).split(
            QRegularExpression(QStringLiteral("[、/]")), Qt::SkipEmptyParts);
        for (const QString &name : names) {
            const QString trimmed = name.trimmed();
            if (!trimmed.isEmpty() && !request.excludedArtists.contains(trimmed, Qt::CaseInsensitive))
                request.excludedArtists.append(trimmed);
        }
    }
    remaining.remove(artistDirective);

    struct Choice { const char *value; QStringList words; };
    const QVector<Choice> scenes{
        {"work", {QStringLiteral("写代码"), QStringLiteral("编程"), QStringLiteral("工作"), QStringLiteral("办公")}},
        {"study", {QStringLiteral("学习"), QStringLiteral("读书"), QStringLiteral("阅读")}},
        {"rest", {QStringLiteral("休息")}}
    };
    int sceneCount = 0;
    for (const auto &choice : scenes) {
        bool matched = false;
        for (const QString &word : choice.words) {
            if (remaining.contains(word)) matched = true;
            remaining.remove(word);
        }
        if (matched) { ++sceneCount; request.scene = QString::fromLatin1(choice.value); }
    }
    if (sceneCount > 1) return fail(error, QStringLiteral("每个计划请只选择一个场景。"));

    const bool favorites = remaining.contains(QStringLiteral("收藏"));
    const bool queue = remaining.contains(QStringLiteral("队列")) || remaining.contains(QStringLiteral("播放列表"));
    if (favorites && queue) return fail(error, QStringLiteral("请选择收藏或当前队列中的一个来源。"));
    if (favorites) request.source = QStringLiteral("favorites");
    if (queue) request.source = QStringLiteral("queue");
    for (const QString &word : {QStringLiteral("我的收藏"), QStringLiteral("收藏"),
                               QStringLiteral("当前队列"), QStringLiteral("队列"),
                               QStringLiteral("播放列表")}) remaining.remove(word);

    const QStringList noLive{QStringLiteral("不要现场版"), QStringLiteral("排除现场版"),
                             QStringLiteral("避开现场版"), QStringLiteral("不要现场"),
                             QStringLiteral("排除现场"), QStringLiteral("不要演唱会版")};
    for (const QString &phrase : noLive) {
        if (remaining.contains(phrase)) request.excludeLive = true;
        remaining.remove(phrase);
    }
    const QStringList instrumental{QStringLiteral("只要纯音乐"), QStringLiteral("仅纯音乐"),
                                   QStringLiteral("不要人声"), QStringLiteral("无人声"),
                                   QStringLiteral("纯音乐")};
    for (const QString &phrase : instrumental) {
        if (remaining.contains(phrase)) request.instrumentalOnly = true;
        remaining.remove(phrase);
    }
    for (const QString &phrase : {QStringLiteral("到时提醒我"), QStringLiteral("结束提醒我"),
                                 QStringLiteral("结束时提醒我"), QStringLiteral("提醒我"),
                                 QStringLiteral("安排音乐"), QStringLiteral("安排听歌"),
                                 QStringLiteral("选音乐"), QStringLiteral("选歌"),
                                 QStringLiteral("放音乐"), QStringLiteral("听歌"),
                                 QStringLiteral("专注计划"), QStringLiteral("专注"),
                                 QStringLiteral("我要"), QStringLiteral("我想"),
                                 QStringLiteral("帮我"), QStringLiteral("请"),
                                 QStringLiteral("从"), QStringLiteral("里"), QStringLiteral("中")})
        remaining.remove(phrase);
    remaining.remove(QRegularExpression(QStringLiteral("[\\s，,。.!！；;：:、]+")));
    if (!remaining.isEmpty())
        return fail(error, QStringLiteral("暂不能可靠理解“%1”。目前支持时长、工作/学习/休息、队列/收藏、排除现场、仅纯音乐，以及“排除歌手：姓名”。请使用界面选项，或启用模型解析后预览。")
                               .arg(remaining.left(80)));
    if (!validRequest(request, error)) return false;
    *output = request;
    return true;
}

FocusPlan FocusPlanner::build(const FocusRequest &request, const QVector<FocusTrack> &candidates,
                              const QVector<FocusRule> &rules)
{
    FocusPlan plan;
    plan.request = request;
    if (!validRequest(request, &plan.error)) return plan;
    bool excludeLive = request.excludeLive;
    bool instrumentalOnly = request.instrumentalOnly;
    QStringList excludedArtists = request.excludedArtists;
    QSet<QString> excludedTracks;
    int ruleCount = 0;
    for (const auto &rule : rules) {
        if (rule.scene != request.scene) continue;
        if (rule.kind == QStringLiteral("exclude_live")) excludeLive = true;
        else if (rule.kind == QStringLiteral("instrumental_only")) instrumentalOnly = true;
        else if (rule.kind == QStringLiteral("exclude_artist")) excludedArtists.append(rule.value);
        else if (rule.kind == QStringLiteral("exclude_track")) excludedTracks.insert(rule.value);
        else continue;
        ++ruleCount;
    }
    if (ruleCount) plan.notes.append(QStringLiteral("已应用此场景的 %1 条明确偏好。可在记忆列表中删除。 ").arg(ruleCount).trimmed());
    QSet<QString> seen;
    int unknownInstrumental = 0;
    int unverifiedLive = 0;
    int unknownDuration = 0;
    qint64 duration = 0;
    const qint64 target = qint64(request.minutes) * 60000;
    for (const auto &track : candidates) {
        if (track.key.isEmpty() || track.url.isEmpty() || !track.url.isValid()) continue;
        if (seen.contains(track.key)) continue;
        if (request.source == QStringLiteral("favorites") && !track.favorite) continue;
        if (excludedTracks.contains(track.key)) continue;
        bool artistExcluded = false;
        for (const QString &name : excludedArtists) {
            if (artistMatches(track.artist, name)) { artistExcluded = true; break; }
        }
        if (artistExcluded) continue;
        if (excludeLive && (track.live == 1 || (track.live == -1 && looksLive(track.title)))) continue;
        if (instrumentalOnly && track.instrumental != 1) {
            if (track.instrumental == -1) ++unknownInstrumental;
            continue;
        }
        seen.insert(track.key);
        if (excludeLive && track.live == -1) ++unverifiedLive;
        plan.tracks.append(track);
        if (track.durationMs > 0 && track.durationMs <= 7LL * 24 * 60 * 60 * 1000) duration += track.durationMs;
        else ++unknownDuration;
        if (duration >= target) break;
    }
    if (instrumentalOnly) plan.notes.append(QStringLiteral("仅使用你人工标注为纯音乐的曲目；未标注不会按歌名猜测。"));
    if (unknownInstrumental) plan.notes.append(QStringLiteral("%1 首曲目尚未标注是否纯音乐，因此未加入。").arg(unknownInstrumental));
    if (excludeLive) plan.notes.append(QStringLiteral("已排除已标注现场版及标题含 Live、现场、演唱会的未知版本。"));
    if (unverifiedLive) plan.notes.append(QStringLiteral("队列中仍有 %1 首版本未人工确认，无法保证全部为录音室版。").arg(unverifiedLive));
    if (unknownDuration) plan.notes.append(QStringLiteral("%1 首曲目时长未知，不计入已知总时长；计时器仍按所选分钟数结束。").arg(unknownDuration));
    if (duration < target) plan.notes.append(QStringLiteral("已知曲目总时长 %1 分 %2 秒，未达到 %3 分钟；将按顺序循环至播放计时结束。").arg(duration / 60000).arg(duration / 1000 % 60).arg(request.minutes));
    else plan.notes.append(QStringLiteral("已知曲目总时长约 %1 分钟，计时结束时由播放器结束本次计划。").arg((duration + 59999) / 60000));
    if (plan.tracks.isEmpty()) plan.error = QStringLiteral("没有符合条件的真实曲目。请添加音乐、调整条件或补充人工标签。");
    return plan;
}

FocusStore::FocusStore(const QSqlDatabase &value) : database(value) {}

bool FocusStore::initialize(QString *error)
{
    clearError(error);
    if (!database.isValid() || !database.isOpen())
        return fail(error, QStringLiteral("偏好数据库尚未打开。"));
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS focus_rules (id INTEGER PRIMARY KEY AUTOINCREMENT, profile TEXT NOT NULL, scene TEXT NOT NULL, kind TEXT NOT NULL, value TEXT NOT NULL DEFAULT '', UNIQUE(profile,scene,kind,value))")))
        return queryError(query, error);
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS focus_track_labels (profile TEXT NOT NULL, track_key TEXT NOT NULL, instrumental INTEGER NOT NULL DEFAULT -1, live INTEGER NOT NULL DEFAULT -1, duration_ms INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(profile,track_key))")))
        return queryError(query, error);
    return true;
}

QVector<FocusRule> FocusStore::rules(const QString &profile, const QString &scene, QString *error) const
{
    clearError(error);
    QVector<FocusRule> result;
    if (!validProfile(profile, error)) return result;
    if (!scene.isEmpty() && !validScene(scene)) {
        fail(error, QStringLiteral("场景标识无效。"));
        return result;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT id,scene,kind,value FROM focus_rules WHERE profile=?")
                  + (scene.isEmpty() ? QString() : QStringLiteral(" AND scene=?"))
                  + QStringLiteral(" ORDER BY id"));
    query.addBindValue(profile);
    if (!scene.isEmpty()) query.addBindValue(scene);
    if (!query.exec()) { queryError(query, error); return result; }
    while (query.next())
        result.append(FocusRule{query.value(0).toLongLong(), query.value(1).toString(),
                                query.value(2).toString(), query.value(3).toString()});
    return result;
}

qint64 FocusStore::addRule(const QString &profile, const QString &scene, const QString &kind,
                          const QString &value, QString *error)
{
    clearError(error);
    if (!validProfile(profile, error)) return 0;
    const bool flag = kind == QStringLiteral("exclude_live") || kind == QStringLiteral("instrumental_only");
    const bool named = kind == QStringLiteral("exclude_artist") || kind == QStringLiteral("exclude_track");
    if (!validScene(scene) || (!flag && !named) || (named && value.trimmed().isEmpty()) || value.size() > 4096) {
        fail(error, QStringLiteral("偏好类型、场景或内容无效。"));
        return 0;
    }
    const QString storedValue = flag ? QStringLiteral("") : value.trimmed();
    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO focus_rules(profile,scene,kind,value) VALUES(?,?,?,?)"));
    query.addBindValue(profile); query.addBindValue(scene); query.addBindValue(kind); query.addBindValue(storedValue);
    if (!query.exec()) { queryError(query, error); return 0; }
    query.prepare(QStringLiteral("SELECT id FROM focus_rules WHERE profile=? AND scene=? AND kind=? AND value=?"));
    query.addBindValue(profile); query.addBindValue(scene); query.addBindValue(kind); query.addBindValue(storedValue);
    if (!query.exec()) { queryError(query, error); return 0; }
    return query.next() ? query.value(0).toLongLong() : 0;
}

bool FocusStore::removeRule(const QString &profile, qint64 id, QString *error)
{
    clearError(error);
    if (!validProfile(profile, error)) return false;
    QSqlQuery query(database);
    query.prepare(QStringLiteral("DELETE FROM focus_rules WHERE profile=? AND id=?"));
    query.addBindValue(profile); query.addBindValue(id);
    if (!query.exec()) return queryError(query, error);
    return query.numRowsAffected() > 0;
}

bool FocusStore::setTrackLabels(const QString &profile, const QString &trackKey, int instrumental,
                               int live, qint64 durationMs, QString *error)
{
    clearError(error);
    if (!validProfile(profile, error)) return false;
    if (trackKey.isEmpty() || trackKey.size() > 4096 || instrumental < -1 || instrumental > 1
        || live < -1 || live > 1 || durationMs < 0 || durationMs > 7LL * 24 * 60 * 60 * 1000)
        return fail(error, QStringLiteral("曲目标注无效。"));
    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT INTO focus_track_labels(profile,track_key,instrumental,live,duration_ms) VALUES(?,?,?,?,?) ON CONFLICT(profile,track_key) DO UPDATE SET instrumental=excluded.instrumental,live=excluded.live,duration_ms=CASE WHEN excluded.duration_ms>0 THEN excluded.duration_ms ELSE focus_track_labels.duration_ms END"));
    query.addBindValue(profile); query.addBindValue(trackKey); query.addBindValue(instrumental);
    query.addBindValue(live); query.addBindValue(durationMs);
    if (!query.exec()) return queryError(query, error);
    return true;
}

QVector<FocusTrack> FocusStore::applyLabels(const QString &profile, const QVector<FocusTrack> &tracks,
                                          QString *error) const
{
    clearError(error);
    QVector<FocusTrack> result = tracks;
    if (!validProfile(profile, error)) return result;
    // One query independent of library size; avoid an individual SQL round-trip
    // per candidate and never load or duplicate the audio itself.
    QHash<QString, QVector<int>> positions;
    for (int index = 0; index < result.size(); ++index) positions[result.at(index).key].append(index);
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT track_key,instrumental,live,duration_ms FROM focus_track_labels WHERE profile=?"));
    query.addBindValue(profile);
    if (!query.exec()) { queryError(query, error); return result; }
    while (query.next()) {
        const auto found = positions.constFind(query.value(0).toString());
        if (found == positions.constEnd()) continue;
        const qint64 duration = query.value(3).toLongLong();
        for (const int index : *found) {
            auto &track = result[index];
            track.instrumental = query.value(1).toInt();
            track.live = query.value(2).toInt();
            if (duration > 0) track.durationMs = duration;
        }
    }
    return result;
}
