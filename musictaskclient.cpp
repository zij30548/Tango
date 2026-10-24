#include "musictaskclient.h"
#include "appbackendclient.h"
#include "neteaseclient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

namespace {
QString requestId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
qint64 realId(const QJsonValue &value) {
    const double id = value.toDouble();
    return value.isDouble() && id > 0 && id <= 9007199254740991. && std::floor(id) == id ? qint64(id) : 0;
}
bool definiteTaskRejection(const QJsonObject &metadata) {
    // 这些机器码均在任务提交前校验或事务回滚后返回。幂等冲突、重放、写冲突、5xx及无完整回执仍未知。
    static const QHash<int, QSet<QString>> codes{
        {400, {"INVALID_TASK_REQUEST", "INVALID_JSON", "INVALID_LOCK", "INVALID_ROUTE"}},
        {403, {"ORIGIN_NOT_ALLOWED", "AI_ACCOUNT_DISABLED"}},
        {404, {"TASK_NOT_FOUND", "NOT_FOUND"}},
        {409, {"TASK_REVISION_CONFLICT", "TASK_DEVICE_MISMATCH", "TASK_NOT_RESUMABLE", "TOOL_RETRY_LIMIT",
               "NO_TASK_PREVIEW", "NO_TASK_UNDO", "INVALID_ARTIST_SELECTION", "ADOPTION_CONFLICT", "MODEL_POLICY_MISSING"}},
        {413, {"BODY_TOO_LARGE"}}, {415, {"JSON_REQUIRED"}},
        {429, {"REQUEST_RATE_LIMITED", "AI_RATE_LIMITED", "AI_BUSY", "TASK_BUDGET",
               "AI_DAILY_LIMIT", "AI_TOTAL_LIMIT", "AI_GLOBAL_DAILY_LIMIT"}}
    };
    const auto status = metadata.value("httpStatus");
    return metadata.value("completeHttp").isBool() && metadata.value("completeHttp").toBool() &&
        status.isDouble() && status.toDouble() == status.toInt() &&
        codes.value(status.toInt()).contains(metadata.value("errorCode").toString());
}
bool validOriginalFacts(const QJsonObject &facts) {
    static const QRegularExpression identifier(QStringLiteral("^[A-Za-z0-9_-]{8,100}$"));
    const auto action = facts.value("action").toString();
    if (action == "create") return facts.size() == 1;
    return facts.size() == 3 && QStringList{"turn", "pause", "cancel", "resume", "undo"}.contains(action) &&
        identifier.match(facts.value("taskId").toString()).hasMatch() && realId(facts.value("expectedRevision")) > 0;
}
bool matchesOriginalFacts(const QJsonObject &receipt, const QJsonObject &task, const QJsonObject &facts) {
    // 记录存在只证明某次请求已提交；恢复依赖还须证明它就是原路由的动作、目标和版本。
    if (!validOriginalFacts(facts) || receipt.value("action").toString() != facts.value("action").toString()) return false;
    if (facts.value("action").toString() == "create") return !receipt.contains("expectedRevision");
    const auto expected = realId(facts.value("expectedRevision"));
    return receipt.value("taskId").toString() == facts.value("taskId").toString() &&
        realId(receipt.value("expectedRevision")) == expected && realId(task.value("revision")) > expected;
}
QString norm(QString value) {
    value = value.normalized(QString::NormalizationForm_KC).toCaseFolded();
    value.remove(QRegularExpression(QStringLiteral("[\\s\\p{P}\\p{S}]+")));
    return value;
}
// 这里只识别来源标题和标签中的同义词，不分析音频；检索词本身不能充当命中证据。
QStringList styleTerms(const QString &style) {
    static const QList<QStringList> groups{
        {"轻快", "欢快", "轻盈", "愉悦", "愉快", "活泼", "upbeat", "lively", "cheerful", "bouncy"},
        {"安静", "宁静", "静谧", "quiet", "peaceful"},
        {"舒缓", "舒缓慢调", "soothing", "calm"},
        {"R&B", "rnb", "节奏蓝调", "rhythm and blues"},
        {"爵士", "jazz"}, {"华语", "国语", "中文", "mandarin"},
        {"纯音乐", "器乐", "instrumental"}, {"摇滚", "rock"},
        {"电子", "electronic", "edm"}, {"民谣", "folk"},
        {"散步", "漫步", "遛弯", "walking", "walk"},
        {"跑步", "慢跑", "running", "jogging"},
        {"读书", "阅读", "reading"}, {"学习", "自习", "study", "studying"},
        {"通勤", "commute", "commuting"}, {"喝酒", "小酌", "微醺", "drinking"},
        {"睡觉", "睡前", "助眠", "sleep"}
    };
    for (const auto &group : groups)
        for (const auto &term : group)
            if (norm(style) == norm(term)) return group;
    return {style};
}
QJsonArray matchedSourceStyles(const QStringList &styles, const QString &name, const QJsonArray &tags) {
    QString labels = name;
    for (const auto &tag : tags) labels += QLatin1Char(' ') + tag.toString();
    labels = labels.normalized(QString::NormalizationForm_KC).toCaseFolded();
    static const QRegularExpression negativePrefix(QStringLiteral(
        "(?:无|非|不|不是|不太适合|不适合|不适用于|不推荐|没有|没|未|未必|缺少|缺乏|缺失|排除|拒绝|避免|告别|远离|不要|无需|不再|谈不上|不能保证|无法保证|是否|是不是|可能|也许|或许|大概|似乎|貌似|不见得|不一定|不确定|无法确定|不能确定|\\bno|\\bnot|\\bwithout|\\bnon|\\bexclude|\\blacking|\\black(?:s|ing)?(?:\\s+of)?|\\bfree\\s+of|\\bmaybe|\\bperhaps|\\bpossibly|\\bwhether|\\bmight(?:\\s+be)?|\\bmay(?:\\s+be)?)[\\s\\-]*(?:(?:任何|一点|一些|太|很|够|怎么|那么|这么|再|的|真的|a|an|the|any|very|so|really|quite|much)[\\s\\-]*)*$"),QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression uncertainSuffix(QStringLiteral(
        "^\\s*(?:[-—]\\s*free\\b|(?:感|程度|得)?(?:不明显|不太明显|不强|不足|不够)|不适合|不适用|不推荐|禁止|慎入|欠缺|缺乏|缺失|未确认|不确定|不保证|与否|存疑|吗|[？?]|的反面)"),QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression clauseBoundary(QStringLiteral("[，,。.!！？?；;|\\n]"));
    QJsonArray matches;
    for (const auto &style : styles) {
        bool matched = false, uncertain = false;
        for (const auto &term : styleTerms(style)) {
            QStringList escapedWords;
            for(const auto &word:term.toCaseFolded().split(QRegularExpression(QStringLiteral("\\s+")),Qt::SkipEmptyParts))
                escapedWords.append(QRegularExpression::escape(word));
            auto pattern = escapedWords.join(QStringLiteral("\\s+"));
            if (term.contains(QRegularExpression(QStringLiteral("^[\\x00-\\x7f]+$")))) {
                // 英文词需边界，避免把 urban 当 R&B 或 rocket 当 rock；中文仍按完整词匹配。
                if (term.compare("R&B", Qt::CaseInsensitive) == 0) pattern = QStringLiteral("r\\s*&\\s*b");
                pattern=QStringLiteral("(?<![a-z0-9])(?:%1)(?![a-z0-9])").arg(pattern);
            }
            if (term.trimmed().isEmpty()) continue;
            auto occurrences=QRegularExpression(pattern).globalMatch(labels);
            while(occurrences.hasNext()) {
                const auto occurrence=occurrences.next();
                const auto before=labels.left(occurrence.capturedStart()).right(60).split(clauseBoundary).last();
                const auto after=labels.mid(occurrence.capturedEnd(),40);
                if(negativePrefix.match(before).hasMatch()||uncertainSuffix.match(after).hasMatch()) uncertain=true;
                else matched=true;
            }
        }
        if (matched && !uncertain && matches.size() < 6) matches.append(style.left(40));
    }
    return matches;
}
QJsonObject sourceEvidence(const QJsonArray &matches) {
    return {{"status", matches.isEmpty() ? "unverified" : "source_matched"},
        {"matchedStyles", matches}, {"basis", matches.isEmpty() ? "none" : "playlist_name_or_tags"}};
}
QJsonObject sceneEvidence(const QJsonArray &matches) {
    return {{"status", matches.isEmpty() ? "unverified" : "source_matched"},
        {"matchedScenes", matches}, {"basis", matches.isEmpty() ? "none" : "playlist_name_or_tags"}};
}
QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
qint64 timestamp(const QJsonValue &value) {
    if (value.isDouble()) return qint64(value.toDouble());
    return QDateTime::fromString(value.toString(), Qt::ISODate).toMSecsSinceEpoch();
}
bool terminal(const QString &status) {
    return status == "adopted" || status == "cancelled" || status == "failed" ||
           status == "paused" || status == "awaiting_clarification" || status == "awaiting_apply" ||
           status == "ready" || status == "preview" || status == "awaiting_confirmation" ||
           status == "needs_clarification" || status == "budget_exhausted";
}
QString stepKey(const QJsonObject &step) {
    return step.value("id").toString() + QLatin1Char(':') + step.value("leaseId").toString();
}

// 每项研究拥有独立、可取消的上下文；场景和发行并行时不会相互取消请求。
struct Research : std::enable_shared_from_this<Research> {
    NeteaseClient *music = nullptr;
    QPointer<QObject> context;
    QJsonObject input;
    std::function<void(const QJsonObject &, const QString &)> done;
    QJsonArray tracks, sources, playlists, albums;
    QStringList queries, exclusions, requestedStyles, requestedScenes;
    QList<QJsonObject> playlistCandidates;
    QSet<qint64> seenTracks, seenSources, excludedIds;
    QString artistName, lastError, requiredArtist;
    QJsonArray artistAliases;
    qint64 artistId = 0, checked = 0;
    int queryIndex = 0, sourceIndex = 0, attempted = 0, timeoutMs = 40000;
    int artistPages = 0;
    bool finished = false, artistVerified = false, styleAdjustment = false, artistLookupDone = false, sceneResearch = false;
    bool active() const { return context && !finished; }
    void complete(const QString &failure = {}, const QString &question = {}, const QJsonArray &artists = {}) {
        if (!active()) return;
        finished = true;
        QJsonObject result{{"tracks", tracks}, {"sources", sources}, {"checkedAt", nowIso()}};
        if (artistVerified && artistId > 0 && !artistName.isEmpty())
            result.insert("resolvedArtist", QJsonObject{{"id", artistId}, {"name", artistName.left(120)}, {"aliases", artistAliases}});
        if (!question.isEmpty()) result.insert("question", question.left(600));
        if (!artists.isEmpty()) result.insert("artists", artists);
        auto callback = done;
        callback(result, failure);
    }
    void start(const QString &skill) {
        for (const auto &id : input.value("excludedIds").toArray()) if (realId(id)) excludedIds.insert(realId(id));
        for (const auto &artist : input.value("excludedArtists").toArray())
            if (artist.isString() && exclusions.size() < 20) exclusions.append(norm(artist.toString().left(100)));
        auto self = shared_from_this();
        QTimer::singleShot(timeoutMs, context, [self] { self->complete(QStringLiteral("本机曲库研究超时，任务已保留，可稍后恢复。")); });
        if (skill == "scene.research") {
            sceneResearch = true;
            const auto brief = input.value("brief").toObject();
            const bool onlyArtist = input.value("artistOnly").toBool(brief.value("artistOnly").toBool());
            requiredArtist = input.value("requiredArtist").toString(brief.value("requiredArtist").toString()).trimmed().left(100);
            const auto scene = brief.value("scene").toString().trimmed().left(120);
            for (const auto &term : QStringList{"散步", "漫步", "遛弯", "跑步", "读书", "阅读", "学习", "通勤", "喝酒", "睡前", "助眠"})
                if (scene.contains(term) && requestedScenes.size() < 3) requestedScenes.append(term);
            if (requestedScenes.isEmpty() && !scene.isEmpty() && scene.size() <= 40) requestedScenes.append(scene);
            const auto adjustmentStyles = input.value("adjustmentStyles").toArray();
            const auto styles = !adjustmentStyles.isEmpty() ? adjustmentStyles :
                (input.value("styles").isArray() ? input.value("styles").toArray() : brief.value("styles").toArray());
            for (const auto &style : styles) {
                const auto value = style.toString().trimmed().left(40);
                if (!value.isEmpty() && !requestedStyles.contains(value) && requestedStyles.size() < 6) requestedStyles.append(value);
            }
            styleAdjustment = !adjustmentStyles.isEmpty() ||
                (!input.value("adjustment").toString(brief.value("adjustment").toString()).trimmed().isEmpty() && !requestedStyles.isEmpty());
            QStringList suppliedQueries;
            for (const auto &query : input.value("queries").toArray()) {
                const auto value = query.toString().trimmed().left(80);
                if (!value.isEmpty() && !suppliedQueries.contains(value) && suppliedQueries.size() < 2) suppliedQueries.append(value);
            }
            if (!adjustmentStyles.isEmpty() && !suppliedQueries.isEmpty()) {
                // 保留后台已确认的语言和歌手条件，不能把“华语 轻快”扩成无语言限制的“轻快”。
                queries = suppliedQueries;
            } else if (styleAdjustment) {
                // 后台已提取明确修改的风格；场景原文可能包含整句追问，不再直接当检索词。
                const auto subject = onlyArtist ? requiredArtist : adjustmentStyles.isEmpty() ? brief.value("scene").toString().left(40) : QString();
                queries.append((subject + QLatin1Char(' ') + requestedStyles.mid(0, 3).join(QLatin1Char(' '))).trimmed().left(80));
            }
            for (const auto &value : suppliedQueries) {
                if (!value.isEmpty() && !queries.contains(value) && queries.size() < 2) queries.append(value);
            }
            if (queries.isEmpty() && (!requestedScenes.isEmpty() || !requestedStyles.isEmpty()))
                queries.append((requestedScenes.join(QLatin1Char(' ')) + QLatin1Char(' ') + requestedStyles.mid(0, 3).join(QLatin1Char(' '))).trimmed().left(80));
            // 先解析上游艺人身份；别名和合唱使用署名 ID 核验，不把提示词里的 ID 当成事实。
            if (onlyArtist && !requiredArtist.isEmpty()) { artistName = requiredArtist; artistSearch(); return; }
            if (queries.isEmpty()) { complete(QStringLiteral("缺少有效的场景检索词。")); return; }
            searchNext();
        } else if (skill == "releases.research") {
            const auto artist = input.value("artist");
            artistId = realId(input.value("artistId"));
            if (artist.isObject()) {
                artistId = realId(artist.toObject().value("id"));
                artistName = artist.toObject().value("name").toString().left(120);
            } else artistName = artist.toString().trimmed().left(120);
            if (artistId && !artistName.isEmpty()) releaseSearch();
            else if (!artistName.isEmpty()) artistSearch();
            else complete({}, QStringLiteral("请补充要查询新作的歌手姓名。"));
        } else complete(QStringLiteral("客户端不支持这个工具版本，请更新探歌。"));
    }
    bool excluded(const QJsonObject &song) const {
        if (excludedIds.contains(realId(song.value("id")))) return true;
        const auto artists = song.value("artist").toString().split(QRegularExpression(
            QStringLiteral("\\s*(?:/|、|,|，|&|;|；| feat\\.? | featuring )\\s*"),
            QRegularExpression::CaseInsensitiveOption), Qt::SkipEmptyParts);
        for (const auto &artist : artists) if (exclusions.contains(norm(artist))) return true;
        const auto brief = input.value("brief").toObject();
        const bool artistOnly = input.value("artistOnly").toBool(brief.value("artistOnly").toBool());
        if (artistOnly && !requiredArtist.isEmpty()) {
            bool credited = false;
            for (const auto &artist : song.value("artists").toArray())
                credited |= artistVerified && realId(artist.toObject().value("id")) == artistId;
            if (!credited) return true;
        }
        return false;
    }
    void addTracks(const QJsonArray &rows, const QString &sourceName, const QJsonArray &tags,
                   qint64 albumId = 0, qint64 published = 0, int rowLimit = 20,
                   const QJsonObject &provenance = {}) {
        for (int i = 0; i < rows.size() && i < rowLimit && tracks.size() < 40; ++i) {
            const auto song = rows.at(i).toObject();
            const auto id = realId(song.value("id"));
            const auto title = song.value("title").toString().trimmed().left(120);
            const auto artist = song.value("artist").toString().trimmed().left(120);
            const int duration = song.value("durationMs").toInt();
            if (!id || title.isEmpty() || artist.isEmpty() || duration < 0 || duration > 7200000 ||
                seenTracks.contains(id) || excluded(song)) continue;
            seenTracks.insert(id);
            QJsonObject track{{"id", id}, {"title", title}, {"artist", artist},
                              {"album", song.value("album").toString().left(120)},
                              {"coverUrl", song.value("coverUrl").toString().left(2048)},
                              {"durationMs", duration}, {"sourceName", sourceName.left(160)}, {"sourceTags", tags}};
            QJsonArray credits; QSet<qint64> seenCredits;
            for (const auto &value : song.value("artists").toArray()) {
                const auto row = value.toObject(); const auto creditId = realId(row.value("id"));
                const auto name = row.value("name").toString().trimmed().left(120);
                if (creditId && !name.isEmpty() && !seenCredits.contains(creditId) && credits.size() < 12) {
                    seenCredits.insert(creditId); credits.append(QJsonObject{{"id", creditId}, {"name", name}});
                }
            }
            track.insert("artists", credits);
            for (auto it = provenance.begin(); it != provenance.end(); ++it) track.insert(it.key(), it.value());
            if (albumId) track.insert("albumId", albumId);
            if (artistId) track.insert("artistId", artistId);
            if (published) track.insert("publishedAt", QDateTime::fromMSecsSinceEpoch(published, Qt::UTC).toString(Qt::ISODateWithMs));
            tracks.append(track);
        }
    }
    bool enoughDuration() const {
        qint64 knownDuration = 0;
        for (const auto &track : tracks) knownDuration += track.toObject().value("durationMs").toInt();
        const auto brief=input.value("brief").toObject();
        const int targetMinutes=brief.value("targetMinutes").toInt();
        const int targetCount=qBound(0,brief.value("targetTrackCount").toInt(),30);
        // 显式首数和时长都要满足；默认12首不能提前截断30首任务，检索预算仍保持不变。
        if (targetCount>0 && tracks.size()<targetCount) return false;
        if (targetMinutes>0) return knownDuration>=qint64(targetMinutes)*54000;
        return targetCount>0 || tracks.size()>=12;
    }
    void searchArtistSongs(const QString &) {
        if (!artistVerified || !artistId) { complete(QStringLiteral("歌手身份尚未核验，无法读取作品。")); return; }
        artistLookupDone = true;
        const int offset = 30 * artistPages++;
        QUrlQuery query; query.addQueryItem("artistId", QString::number(artistId));
        query.addQueryItem("limit", "30"); query.addQueryItem("offset", QString::number(offset));
        auto self = shared_from_this();
        music->get("/artist/tracks", query, context, [self, offset](const QJsonObject &data, const QString &error) {
            if (!self->active()) return;
            if (!error.isEmpty()) self->lastError = error;
            const auto total=data.value("total");
            const bool totalValid=total.isNull() || (total.isDouble() && total.toDouble() >= 0 &&
                total.toDouble() <= 9007199254740991. && std::floor(total.toDouble()) == total.toDouble());
            const bool valid = error.isEmpty() && realId(data.value("artistId")) == self->artistId &&
                data.value("offset").isDouble() && data.value("offset").toDouble() == offset &&
                data.value("limit").isDouble() && data.value("limit").toDouble() == 30 && totalValid &&
                data.value("songs").isArray() && data.value("more").isBool() && data.value("checkedAt").isDouble() &&
                std::abs(timestamp(data.value("checkedAt")) - QDateTime::currentMSecsSinceEpoch()) <= 300000;
            const QString sourceName = QStringLiteral("歌手作品：%1").arg(self->artistName);
            if (valid) {
                const QJsonObject provenance{{"sourceId", self->artistId}, {"sourceKind", "artist_catalog"}, {"sourceQuery", self->requiredArtist.left(80)},
                    {"styleEvidence", sourceEvidence({})}, {"sceneEvidence", sceneEvidence({})}};
                const int before = self->tracks.size();
                self->addTracks(data.value("songs").toArray(), sourceName, {}, 0, 0, 30, provenance);
                bool recorded = false;
                for (const auto &value : self->sources) recorded |= value.toObject().value("sourceKind").toString() == "artist_catalog";
                if (self->tracks.size() > before && !recorded && self->sources.size() < 3) {
                    auto source = provenance;
                    source.insert("id", self->artistId); source.insert("name", sourceName); source.insert("tags", QJsonArray()); source.insert("checkedAt", nowIso());
                    self->sources.append(source);
                }
            } else if (error.isEmpty()) self->lastError = QStringLiteral("歌手作品回执身份、分页或时间不一致，未采用。");
            // 最多两页，仍共享原工具超时和候选上限；不足时如实返回而不扩大成无限检索。
            if (valid && data.value("more").toBool() && self->artistPages < 2 && !self->enoughDuration() && self->tracks.size() < 40) {
                self->searchArtistSongs(self->requiredArtist); return;
            }
            if (self->queryIndex >= self->queries.size() || self->enoughDuration() || self->tracks.size() >= 40 || self->queries.isEmpty()) {
                self->complete(self->tracks.isEmpty() ? self->lastError : QString()); return;
            }
            self->searchNext();
        });
    }
    void searchNext() {
        if (!active()) return;
        if (queryIndex >= queries.size()) {
            if (styleAdjustment || !requestedScenes.isEmpty()) {
                QList<QJsonObject> ordered;
                for (const auto &row : playlists) ordered.append(row.toObject());
                std::stable_sort(ordered.begin(), ordered.end(), [this](const QJsonObject &a, const QJsonObject &b) {
                    const int styleWeight=styleAdjustment?7:1, sceneWeight=styleAdjustment?1:7;
                    const auto left=styleWeight*matchedSourceStyles(requestedStyles, a.value("name").toString(), a.value("tags").toArray()).size() +
                        sceneWeight*matchedSourceStyles(requestedScenes, a.value("name").toString(), a.value("tags").toArray()).size();
                    const auto right=styleWeight*matchedSourceStyles(requestedStyles, b.value("name").toString(), b.value("tags").toArray()).size() +
                        sceneWeight*matchedSourceStyles(requestedScenes, b.value("name").toString(), b.value("tags").toArray()).size();
                    if(left!=right)return left>right;
                    // 标题里的歌手名只影响检索顺序，歌曲仍必须通过真实署名 ID 核验。
                    const auto wanted=norm(requiredArtist);
                    return !wanted.isEmpty()&&norm(a.value("name").toString()).contains(wanted)&&
                        !norm(b.value("name").toString()).contains(wanted);
                });
                playlists = {}; for (const auto &row : ordered) playlists.append(row);
            }
            playlistNext(); return;
        }
        const auto sourceQuery = queries.at(queryIndex++);
        QUrlQuery query; query.addQueryItem("keywords", sourceQuery);
        query.addQueryItem("limit", "5"); query.addQueryItem("offset", "0");
        auto self = shared_from_this();
        music->get("/radio/search", query, context, [self, sourceQuery](const QJsonObject &data, const QString &error) {
            if (!self->active()) return;
            if (!error.isEmpty()) self->lastError = error;
            const auto rows = data.value("playlists").toArray();
            bool first = true;
            for (int i = 0; i < rows.size() && i < 5 && self->playlists.size() < 10; ++i) {
                auto row = rows.at(i).toObject(); const auto id = realId(row.value("id"));
                if (!id || self->seenSources.contains(id) || row.value("name").toString().trimmed().isEmpty()) continue;
                self->seenSources.insert(id);
                row.insert("sourceQuery", sourceQuery);
                if (first && self->queryIndex > 1 && !self->playlists.isEmpty()) self->playlists.insert(1, row);
                else self->playlists.append(row);
                first = false;
            }
            self->searchNext();
        });
    }
    void playlistNext() {
        if (!active()) return;
        if (attempted >= 5 || sourceIndex >= playlists.size() ||
            (!styleAdjustment && requestedScenes.isEmpty() && (sources.size() >= 3 || tracks.size() >= 40))) {
            if (styleAdjustment || !requestedScenes.isEmpty()) { finishStylePlaylists(); return; }
            if (!requiredArtist.isEmpty() && !artistLookupDone && !enoughDuration() && sources.size() < 3) { searchArtistSongs(requiredArtist); return; }
            complete(tracks.isEmpty() && !lastError.isEmpty() ? lastError : QString()); return;
        }
        const auto playlist = playlists.at(sourceIndex++).toObject(); ++attempted;
        const auto id = realId(playlist.value("id"));
        QUrlQuery query; query.addQueryItem("id", QString::number(id)); query.addQueryItem("limit", "20");
        const auto brief = input.value("brief").toObject();
        if (input.value("artistOnly").toBool(brief.value("artistOnly").toBool()) && artistVerified)
            query.addQueryItem("artistId", QString::number(artistId));
        auto self = shared_from_this();
        music->get("/radio/playlist", query, context, [self, id, playlist](const QJsonObject &data, const QString &error) {
            if (!self->active()) return;
            const auto source = data.value("playlist").toObject();
            if (!error.isEmpty()) self->lastError = error;
            if (error.isEmpty() && realId(source.value("id")) == id &&
                !source.value("name").toString().trimmed().isEmpty()) {
                QJsonArray tags;
                for (const auto &tag : source.value("tags").toArray())
                    if (tag.isString() && tags.size() < 6) tags.append(tag.toString().left(40));
                const int before = self->tracks.size();
                const auto name = source.value("name").toString().left(160);
                const auto matched = matchedSourceStyles(self->requestedStyles, name, tags);
                const auto matchedScenes = matchedSourceStyles(self->requestedScenes, name, tags);
                QJsonObject provenance{{"sourceId", id}, {"sourceKind", "playlist"}, {"sourceQuery", playlist.value("sourceQuery")},
                    {"styleEvidence", sourceEvidence(matched)}, {"sceneEvidence", sceneEvidence(matchedScenes)}};
                auto metadata = provenance;
                metadata.insert("id", id); metadata.insert("name", name); metadata.insert("tags", tags);
                metadata.insert("checkedAt", nowIso()); metadata.insert("url", QStringLiteral("https://music.163.com/playlist?id=%1").arg(id));
                if (self->styleAdjustment || !self->requestedScenes.isEmpty()) {
                    const int score=(self->styleAdjustment?7:1)*matched.size() + (self->styleAdjustment?1:7)*matchedScenes.size();
                    self->playlistCandidates.append(QJsonObject{{"metadata", metadata}, {"songs", data.value("songs").toArray()}, {"score", score}});
                    self->playlistNext(); return;
                }
                self->addTracks(data.value("songs").toArray(), name, tags, 0, 0, 20, provenance);
                // 搜索命中仍只是候选；保留真实来源标题和标签，由后台独立检查场景依据。
                if (self->tracks.size() > before) self->sources.append(metadata);
            }
            self->playlistNext();
        });
    }
    void finishStylePlaylists() {
        // 同一五次详情预算内先取来源再排序，防止泛化来源先占满40个候选，挤掉实际场景依据。
        std::stable_sort(playlistCandidates.begin(), playlistCandidates.end(), [this](const QJsonObject &a, const QJsonObject &b) {
            if(a.value("score").toInt()!=b.value("score").toInt())return a.value("score").toInt()>b.value("score").toInt();
            const auto wanted=norm(requiredArtist);
            return !wanted.isEmpty()&&norm(a.value("metadata").toObject().value("name").toString()).contains(wanted)&&
                !norm(b.value("metadata").toObject().value("name").toString()).contains(wanted);
        });
        const auto brief = input.value("brief").toObject();
        const bool artistOnly = input.value("artistOnly").toBool(brief.value("artistOnly").toBool());
        const bool canFillArtist = artistOnly && !requiredArtist.isEmpty();
        for (const auto &candidate : playlistCandidates) {
            // 来源不足时给按真实艺人 ID 读取的作品保留一格；补齐歌曲仍标记场景未核验。
            if (sources.size() >= 3 || tracks.size() >= 40 || (canFillArtist && sources.size() >= 2 && !enoughDuration())) break;
            const auto metadata = candidate.value("metadata").toObject();
            const QJsonObject provenance{{"sourceId", metadata.value("sourceId")}, {"sourceKind", metadata.value("sourceKind")}, {"sourceQuery", metadata.value("sourceQuery")},
                {"styleEvidence", metadata.value("styleEvidence")}, {"sceneEvidence", metadata.value("sceneEvidence")}};
            const int before = tracks.size();
            addTracks(candidate.value("songs").toArray(), metadata.value("name").toString(), metadata.value("tags").toArray(), 0, 0, 20, provenance);
            if (tracks.size() > before) sources.append(metadata);
        }
        if (canFillArtist && !artistLookupDone && !enoughDuration() && tracks.size() < 40 && sources.size() < 3) {
            searchArtistSongs(requiredArtist); return;
        }
        complete(tracks.isEmpty() && !lastError.isEmpty() ? lastError : QString());
    }
    void artistSearch() {
        QUrlQuery query; query.addQueryItem("keywords", artistName); query.addQueryItem("limit", "5");
        auto self = shared_from_this();
        music->get("/assistant/artists", query, context, [self](const QJsonObject &data, const QString &error) {
            if (!self->active()) return;
            if (!error.isEmpty()) { self->complete(error); return; }
            QJsonArray artists; QSet<qint64> seen;
            for (const auto &row : data.value("artists").toArray()) {
                const auto value = row.toObject(); const auto id = realId(value.value("id"));
                const auto name = value.value("name").toString().trimmed().left(120);
                if (!id || name.isEmpty() || seen.contains(id) || artists.size() >= 5) continue;
                seen.insert(id);
                QStringList identity;
                for (const auto &alias : value.value("aliases").toArray()) {
                    const auto text = alias.toString().trimmed().left(80);
                    if (!text.isEmpty() && !identity.contains(text)) identity.append(text);
                    if (identity.size() == 3) break;
                }
                for (const auto &key : QStringList{"description", "identity"}) {
                    const auto text = value.value(key).toString().trimmed().left(120);
                    if (!text.isEmpty() && !identity.contains(text)) identity.append(text);
                }
                QJsonArray aliases;
                for (const auto &alias : value.value("aliases").toArray()) {
                    const auto text = alias.toString().trimmed().left(100);
                    if (!text.isEmpty() && aliases.size() < 10 && !aliases.contains(text)) aliases.append(text);
                }
                artists.append(QJsonObject{{"id", id}, {"name", name}, {"aliases", aliases}, {"description", identity.join(QStringLiteral(" / ")).left(160)}});
            }
            QJsonArray exactMatches;
            for (const auto &candidate : artists) {
                const auto row = candidate.toObject();
                bool exact = norm(row.value("name").toString()) == norm(self->artistName);
                for (const auto &value : data.value("artists").toArray())
                    if (realId(value.toObject().value("id")) == realId(row.value("id")))
                        for (const auto &alias : value.toObject().value("aliases").toArray()) exact |= norm(alias.toString()) == norm(self->artistName);
                if (exact) exactMatches.append(row);
            }
            // 多条搜索结果中只有一个姓名或上游别名完全匹配时可继续；
            // 同名或多个别名冲突仍须用户确认，不能直接采用排名第一项。
            if (exactMatches.size() == 1) {
                const auto row = exactMatches.first().toObject();
                self->artistId = realId(row.value("id"));
                self->artistName = row.value("name").toString();
                self->artistAliases = row.value("aliases").toArray();
                if (self->sceneResearch) {
                    self->artistVerified = true;
                    // 场景或风格约束先读取来源，不能因为普通作品够长就跳过散步等要求。
                    if (!self->queries.isEmpty() && (self->styleAdjustment || !self->requestedScenes.isEmpty() || !self->requestedStyles.isEmpty())) self->searchNext();
                    else self->searchArtistSongs(self->requiredArtist);
                } else self->releaseSearch();
                return;
            }
            self->complete({}, artists.isEmpty() ? QStringLiteral("没有找到这位歌手，请补充完整姓名。")
                : QStringLiteral("找到多个可能的歌手，请先确认歌手身份。"), artists);
        });
    }
    void releaseSearch() {
        QUrlQuery query; query.addQueryItem("artistId", QString::number(artistId)); query.addQueryItem("limit", "6");
        auto self = shared_from_this();
        music->get("/assistant/releases", query, context, [self](const QJsonObject &data, const QString &error) {
            if (!self->active()) return;
            if (!error.isEmpty()) { self->complete(error); return; }
            self->checked = timestamp(data.value("checkedAt"));
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (realId(data.value("artist").toObject().value("id")) != self->artistId || std::abs(self->checked - now) > 300000) {
                self->complete(QStringLiteral("新作来源身份或查询时间校验失败。")); return;
            }
            const auto verifiedName = data.value("artist").toObject().value("name").toString().trimmed().left(120);
            if (!verifiedName.isEmpty()) self->artistName = verifiedName;
            self->artistVerified = true;
            const qint64 since = timestamp(self->input.value("since"));
            const qint64 until = timestamp(self->input.value("until"));
            QList<QJsonObject> sorted; QSet<qint64> seen;
            for (const auto &row : data.value("albums").toArray()) {
                const auto album = row.toObject(); const auto id = realId(album.value("id"));
                const auto published = timestamp(album.value("publishTime"));
                if (!id || seen.contains(id) || published <= 0 || published > self->checked ||
                    (since > 0 && published < since) || (until > 0 && published > until) ||
                    album.value("name").toString().trimmed().isEmpty()) continue;
                seen.insert(id); sorted.append(album);
            }
            std::stable_sort(sorted.begin(), sorted.end(), [](const QJsonObject &a, const QJsonObject &b) {
                return timestamp(a.value("publishTime")) > timestamp(b.value("publishTime"));
            });
            for (int i = 0; i < sorted.size() && i < 6; ++i) self->albums.append(sorted.at(i));
            self->albumNext();
        });
    }
    void albumNext() {
        if (!active()) return;
        if (sources.size() >= 3 || attempted >= 3 || sourceIndex >= albums.size() || tracks.size() >= 40) {
            complete(tracks.isEmpty() && !lastError.isEmpty() ? lastError : QString()); return;
        }
        const auto album = albums.at(sourceIndex++).toObject(); ++attempted;
        const auto id = realId(album.value("id"));
        QUrlQuery query; query.addQueryItem("id", QString::number(id)); query.addQueryItem("limit", "20");
        auto self = shared_from_this();
        music->get("/assistant/album", query, context, [self, id, album](const QJsonObject &data, const QString &error) {
            if (!self->active()) return;
            const auto source = data.value("album").toObject();
            if (!error.isEmpty()) self->lastError = error;
            const auto published = timestamp(album.value("publishTime"));
            if (error.isEmpty() && realId(source.value("id")) == id && realId(source.value("artistId")) == self->artistId &&
                timestamp(source.value("publishTime")) == published) {
                const auto name = source.value("name").toString().left(160); const int before = self->tracks.size();
                const QJsonObject provenance{{"sourceId", id}, {"sourceKind", "album"},
                    {"sourceQuery", self->artistName.left(80)}, {"styleEvidence", sourceEvidence({})}};
                self->addTracks(data.value("songs").toArray(), name, {}, id, published, 20, provenance);
                if (self->tracks.size() > before) {
                    auto metadata = provenance;
                    metadata.insert("id", id); metadata.insert("name", name); metadata.insert("checkedAt", nowIso());
                    metadata.insert("url", QStringLiteral("https://music.163.com/album?id=%1").arg(id));
                    self->sources.append(metadata);
                }
            }
            self->albumNext();
        });
    }
};
}

MusicTaskClient::MusicTaskClient(QObject *parent, AppBackendClient *backendClient)
    : QObject(parent), backend(backendClient ? backendClient : AppBackendClient::instance()),
      music(new NeteaseClient(this)), timer(new QTimer(this)),
      eventReconnectTimer(new QTimer(this)), progressRefreshTimer(new QTimer(this)) {
    QDir().mkpath(NeteaseClient::dataDirectory());
    const auto settingsPath = QDir(NeteaseClient::dataDirectory()).filePath(QStringLiteral("music-task-device.ini"));
    QSettings settings(settingsPath, QSettings::IniFormat);
    installationId = settings.value(QStringLiteral("musicTasks/deviceId")).toString();
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_-]{16,80}$"));
    if (!valid.match(installationId).hasMatch()) {
        installationId = requestId(); settings.setValue(QStringLiteral("musicTasks/deviceId"), installationId);
        settings.sync(); QFile::setPermissions(settingsPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    timer->setSingleShot(true);
    eventReconnectTimer->setSingleShot(true);
    progressRefreshTimer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, &MusicTaskClient::poll);
    connect(eventReconnectTimer, &QTimer::timeout, this, &MusicTaskClient::ensureEvents);
    connect(progressRefreshTimer, &QTimer::timeout, this, &MusicTaskClient::poll);
    connect(backend, &AppBackendClient::authChanged, this, [this] { detach(); });
}
MusicTaskClient::~MusicTaskClient() { cancelRequests(); }
QJsonObject MusicTaskClient::snapshot() const {
    auto visible = state;
    if (observationOnly) visible.insert("clientObservationOnly", true);
    if (!connectionState.isEmpty()) visible.insert("clientConnection", connectionState);
    // An event is an observation, never permission to run a tool or adopt music.
    // Keep the complete snapshot authoritative until its safe GET catches up.
    if (!observedProgress.isEmpty() && observedProgress.value("taskId") == state.value("id") &&
        observedProgress.value("revision").toInt() >= state.value("revision").toInt() &&
        observedProgress.value("stateVersion").toDouble() >= state.value("stateVersion").toDouble())
        visible.insert("progress", observedProgress);
    return visible;
}
QString MusicTaskClient::deviceId() const { return installationId; }
bool MusicTaskClient::busy() const { return requestBusy; }
void MusicTaskClient::setBusy(bool value) {
    if (requestBusy == value) return;
    requestBusy = value; emit busyChanged(value);
}
void MusicTaskClient::cancelRequests() {
    ++generation;
    timer->stop();
    stopEvents(); progressRefreshTimer->stop(); observedProgress = {}; eventFailures = 0; progressCatchupAttempts = 0;
    resultOutbox.clear(); resultAttempts.clear();
    // Invalidating generation precedes abort: finished may be synchronous.
    delete operation.data(); operation = nullptr;
    const auto contexts = tools; tools.clear();
    for (const auto &context : contexts) delete context.data();
}
void MusicTaskClient::detach() {
    cancelRequests(); completedSteps.clear(); state = {}; consecutiveErrors = 0;
    connectionState = {}; discoverLatest = false;
    eventsUnsupported = false;
    selectedTaskId.clear(); observationOnly = false;
    setBusy(false); emit snapshotChanged(state);
}
void MusicTaskClient::failed(const QString &message, quint64 epoch) {
    if (epoch != generation) return;
    delete operation.data(); operation = nullptr; setBusy(false);
    // Resume discovery after transient network loss, without resending a turn,
    // planner request, or adoption. Polling GET is safe; server owns retries.
    const bool canObserve = (!state.isEmpty() || discoverLatest || !selectedTaskId.isEmpty()) && backend->authenticated();
    const bool retry = canObserve && ++consecutiveErrors <= 5;
    const QString guidance = retry
        ? tr("与后台的连接中断，正在安全查询已保存进度；不会重复提交任务。")
        : tr("已停止自动查询，后台任务状态尚未确认。请检查连接后点击“找回任务”或“继续任务”；不会自动重发请求或播放歌曲。");
    connectionState = {{"status", retry ? "retrying" : "interrupted"},
        {"message", message.left(500) + QLatin1Char('\n') + guidance}};
    emit snapshotChanged(snapshot());
    emit error(message);
    if (retry)
        timer->start(qMin(30000, 2000 * consecutiveErrors));
}
void MusicTaskClient::rejected(const QString &id, const QString &message, quint64 epoch) {
    if (epoch != generation) return;
    delete operation.data(); operation = nullptr;
    discoverLatest = false; connectionState = {}; consecutiveErrors = 0;
    // 明确拒绝终结本次派发；保留当前任务，只等用户核对，不安排查询或新ID重发。
    observationOnly = true; setBusy(false);
    if (epoch != generation) return;
    emit snapshotChanged(snapshot());
    if (epoch != generation) return;
    const auto explanation = tr("后台已明确拒绝本次提交：%1。未自动重发；请核对当前任务后重新提出要求。").arg(message.left(400));
    if (!id.isEmpty()) emit requestRejected(id, explanation);
    emit error(explanation);
}
void MusicTaskClient::restoreLatest() {
    if (requestBusy) return;
    if (!backend->authenticated()) { emit error(tr("请先登录探歌账户，再恢复音乐任务。")); return; }
    cancelRequests(); completedSteps.clear(); discoverLatest = true; consecutiveErrors = 0; connectionState = {};
    selectedTaskId.clear(); observationOnly = false;
    const auto epoch = generation; operation = new QObject(this); setBusy(true);
    backend->latestMusicTask(installationId, operation, [this, epoch](const QJsonObject &data, const QString &failure) {
        if (epoch != generation) return;
        if (!failure.isEmpty()) { failed(failure, epoch); return; }
        acceptSnapshot(data, epoch);
    });
}
void MusicTaskClient::restore(const QString &taskId) {
    if (requestBusy) return;
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_-]{8,100}$"));
    if (!backend->authenticated() || !valid.match(taskId).hasMatch()) {
        emit error(tr("请先登录并选择有效的音乐任务。")); return;
    }
    cancelRequests(); completedSteps.clear(); discoverLatest = false;
    selectedTaskId = taskId; observationOnly = true;
    if (state.value("id").toString() != taskId) state = {};
    consecutiveErrors = 0; connectionState = {};
    const auto epoch = generation; operation = new QObject(this); setBusy(true);
    backend->musicTask(taskId, operation, [this, epoch](const QJsonObject &data, const QString &failure) {
        if (epoch != generation) return;
        if (!failure.isEmpty()) { failed(failure, epoch); return; }
        acceptSnapshot(data, epoch);
    });
}
void MusicTaskClient::observeRequest(const QString &requestId, const QJsonObject &expectedFacts, bool selectTask) {
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_-]{8,100}$"));
    if (!backend->authenticated() || !valid.match(requestId).hasMatch() || !validOriginalFacts(expectedFacts)) {
        emit error(tr("请先登录并提供有效的原操作标识。")); return;
    }
    if (!selectTask) {
        // 旧请求的事实查询使用独立上下文，不能中断新任务、租约工具或通知订阅。
        const auto epoch = generation;
        const auto account = backend->serviceIdentity() + QLatin1Char(':') + backend->userId();
        auto *context = new QObject(this);
        backend->musicTaskRequest(installationId, requestId, context,
            [this, epoch, account, requestId, expectedFacts, context](const QJsonObject &data, const QString &failure) {
            context->deleteLater();
            if (epoch != generation || !backend->authenticated() ||
                account != backend->serviceIdentity() + QLatin1Char(':') + backend->userId()) return;
            if (!failure.isEmpty()) {
                emit error(tr("原操作结果仍未确认；本次只查询，没有重复提交。") + failure.left(300)); return;
            }
            const auto receipt = data.value("receipt").toObject();
            const auto task = data.value("task").toObject();
            const auto taskId = receipt.value("taskId").toString();
            static const QRegularExpression validTask(QStringLiteral("^[A-Za-z0-9_-]{8,100}$"));
            if (receipt.value("requestId").toString() != requestId ||
                receipt.value("deviceId").toString() != installationId) {
                emit error(tr("原操作回执与请求不一致，未继续后续操作。")); return;
            }
            if (receipt.value("status").toString() == "unknown" && data.value("task").isNull() &&
                !receipt.contains("taskId")) {
                emit error(tr("尚未查到原操作记录；这不代表操作未执行，没有自动重发。")); return;
            }
            if (receipt.value("status").toString() != "recorded" || !validTask.match(taskId).hasMatch() ||
                task.value("id").toString() != taskId || task.value("deviceId").toString() != installationId ||
                !task.value("revision").isDouble() || task.value("revision").toInt() < 1 ||
                task.value("pendingSteps").toArray().size() > 2 || !matchesOriginalFacts(receipt, task, expectedFacts)) {
                emit error(tr("原操作回执格式无效，未继续后续操作。")); return;
            }
            emit requestSettled(requestId, true, {});
        });
        return;
    }
    if (requestBusy) { emit error(tr("正在查询或提交任务，请稍后再观察原操作。")); return; }
    cancelRequests(); observationOnly = true; discoverLatest = false;
    const auto epoch = generation;
    operation = new QObject(this); setBusy(true);
    backend->musicTaskRequest(installationId, requestId, operation,
        [this, epoch, requestId, expectedFacts](const QJsonObject &data, const QString &failure) {
        if (epoch != generation) return;
        const auto finishUnknown = [this](const QString &message) {
            delete operation.data(); operation = nullptr; setBusy(false);
            emit error(message);
        };
        if (!failure.isEmpty()) {
            finishUnknown(tr("原操作结果仍未确认；本次只查询，没有重复提交。") + failure.left(300)); return;
        }
        const auto receipt = data.value("receipt").toObject();
        if (receipt.value("requestId").toString() != requestId ||
            receipt.value("deviceId").toString() != installationId) {
            finishUnknown(tr("原操作回执与请求不一致，未继续后续操作。")); return;
        }
        if (receipt.value("status").toString() == "unknown" && data.value("task").isNull() &&
            !receipt.contains("taskId")) {
            finishUnknown(tr("尚未查到原操作记录；这不代表操作未执行，没有自动重发。")); return;
        }
        static const QRegularExpression validTask(QStringLiteral("^[A-Za-z0-9_-]{8,100}$"));
        const auto taskId = receipt.value("taskId").toString();
        if (receipt.value("status").toString() != "recorded" || !validTask.match(taskId).hasMatch() ||
            data.value("task").toObject().value("id").toString() != taskId ||
            !matchesOriginalFacts(receipt, data.value("task").toObject(), expectedFacts)) {
            finishUnknown(tr("原操作回执格式无效，未继续后续操作。")); return;
        }
        selectedTaskId = taskId;
        // 读取事实仅恢复界面，不能启用租约工具、采用队列或再次调用模型。
        if (acceptSnapshot(data, epoch) && epoch == generation)
            emit requestSettled(requestId, true, {});
    });
}
void MusicTaskClient::observeAdoptionRequest(const QString &requestId, const QString &taskId, int expectedRevision) {
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_-]{8,100}$"));
    if (!backend->authenticated() || !valid.match(requestId).hasMatch() || !valid.match(taskId).hasMatch() || expectedRevision<1 ||
        requestBusy || state.value("id").toString()!=taskId || state.value("revision").toInt()!=expectedRevision) {
        emit error(tr("当前任务已变化或正在提交，未恢复旧采用请求。")); return;
    }
    const auto epoch=generation;
    auto *context=new QObject(this);
    backend->musicTaskRequest(installationId,requestId,context,
        [this,epoch,requestId,taskId,expectedRevision,context](const QJsonObject &data,const QString &failure) {
        context->deleteLater();
        if(epoch!=generation || !backend->authenticated() || state.value("id").toString()!=taskId ||
            state.value("revision").toInt()!=expectedRevision)return;
        if(!failure.isEmpty()) {emit error(tr("原采用请求仍未确认，本次只查询，没有重复提交。")+failure.left(300));return;}
        const auto receipt=data.value("receipt").toObject(); const auto task=data.value("task").toObject();
        if(receipt.value("requestId").toString()!=requestId || receipt.value("deviceId").toString()!=installationId) {
            emit error(tr("原采用回执与请求不一致，未继续采用或播放。")); return;
        }
        if(receipt.value("status").toString()=="unknown" && data.value("task").isNull()) {
            emit error(tr("尚未查到原采用请求，这不代表未执行；没有重复提交或播放。")); return;
        }
        if(!receipt.contains("action") || !receipt.contains("expectedRevision")) {
            emit error(tr("原回执缺少原动作事实，不能核验采用；没有重复提交或自动播放。")); return;
        }
        if(receipt.value("status").toString()!="recorded" || receipt.value("taskId").toString()!=taskId ||
            receipt.value("action").toString()!="adopt" || realId(receipt.value("expectedRevision"))!=expectedRevision ||
            task.value("id").toString()!=taskId || task.value("deviceId").toString()!=installationId || realId(task.value("revision"))!=expectedRevision) {
            emit error(tr("原采用动作或任务版本未通过核验，没有重复提交或播放。")); return;
        }
        // 即使原采用已落库，只恢复事实；本机播放仍需要用户重新明确采用。
        observationOnly=true;
        if(acceptSnapshot(data,epoch) && epoch==generation)emit requestSettled(requestId,true,{});
    });
}
void MusicTaskClient::submit(const QString &instruction) {
    if (!state.value("id").toString().isEmpty()) {
        if (instruction.trimmed().isEmpty()) { emit error(tr("请补充这次修改的要求。")); return; }
        action(QStringLiteral("turn"), instruction);
    } else submit(instruction, QStringLiteral(""), {});
}
void MusicTaskClient::submit(const QString &instruction, const QString &preference,
                             const QVector<qint64> &excludedIds, const QString &messageId) {
    if (!backend->authenticated()) { emit error(tr("请先登录探歌账户，再创建音乐任务。")); return; }
    if (instruction.trimmed().isEmpty() || instruction.size() > 1000 || preference.size() > 500) {
        emit error(tr("请用 1 至 1000 个字符描述音乐需求。")); return;
    }
    if (requestBusy) { emit error(tr("正在提交上一个任务操作，请稍候再创建。")); return; }
    static const QRegularExpression validMessageId(QStringLiteral("^[A-Za-z0-9_-]{8,100}$"));
    if (!messageId.isEmpty() && !validMessageId.match(messageId).hasMatch()) {
        emit error(tr("会话消息标识无效，未提交音乐任务。")); return;
    }
    if (discoverLatest && !connectionState.isEmpty()) {
        emit error(tr("上次提交结果尚未确认，请先点击“找回任务”；若要另建一份，请明确点击“新建安排”。")); return;
    }
    if (!state.value("id").toString().isEmpty()) {
        action(QStringLiteral("turn"), instruction, {{"preference", preference}}, messageId);
        return;
    }
    cancelRequests(); completedSteps.clear(); discoverLatest = false; connectionState = {}; consecutiveErrors = 0;
    selectedTaskId.clear(); observationOnly = false;
    const auto epoch = generation;
    operation = new QObject(this); setBusy(true);
    QJsonArray excluded;
    for (int i = qMax(0, excludedIds.size() - 60); i < excludedIds.size(); ++i)
        if (excludedIds.at(i) > 0 && excludedIds.at(i) <= 9007199254740991LL) excluded.append(excludedIds.at(i));
    const QJsonObject body{{"deviceId", installationId}, {"requestId", messageId.isEmpty() ? requestId() : messageId},
        {"catalogContext", NeteaseClient::catalogCacheContext()},
        {"instruction", instruction.trimmed()}, {"preference", preference}, {"excludedIds", excluded}};
    backend->createMusicTask(body, operation, [this, epoch, instruction, messageId](const QJsonObject &data, const QString &failure) {
        if (epoch != generation) return;
        if (!failure.isEmpty()) {
            if (definiteTaskRejection(data)) { rejected(messageId, failure, epoch); return; }
            discoverLatest = true; failed(failure, epoch);
            if (!messageId.isEmpty()) emit requestSettled(messageId, false, failure);
            return;
        }
        const bool accepted = acceptSnapshot(data, epoch);
        if (accepted && epoch == generation && !state.value("id").toString().isEmpty() &&
            state.value("id") == data.value("task").toObject().value("id"))
            emit instructionAccepted(instruction.trimmed());
        if (!messageId.isEmpty()) emit requestSettled(messageId, accepted, accepted ? QString() : tr("音乐任务回执格式无效。"));
    });
}
void MusicTaskClient::action(const QString &name, const QString &instruction, const QJsonObject &details, const QString &messageId) {
    dispatchAction(name, instruction, details, messageId, false);
}
void MusicTaskClient::adopt(const QString &requestId) {
    if (requestId.isEmpty()) { emit error(tr("采用操作缺少原始请求标识，未提交。")); return; }
    dispatchAction(QStringLiteral("adopt"), {}, {}, requestId, true);
}
void MusicTaskClient::dispatchAction(const QString &name, const QString &instruction, const QJsonObject &details,
                                     const QString &messageId, bool explicitAdoption) {
    if (!backend->authenticated() || state.value("id").toString().isEmpty()) {
        emit error(tr("请先登录并打开一个音乐任务。")); return;
    }
    if (requestBusy) { emit error(tr("正在提交上一个任务操作，请稍候再修改。")); return; }
    if (instruction.size() > 1000) { emit error(tr("修改要求不能超过 1000 个字符。")); return; }
    static const QRegularExpression validMessageId(QStringLiteral("^[A-Za-z0-9_-]{8,100}$"));
    if (!messageId.isEmpty() && (!(QStringList{"turn", "pause", "cancel", "resume", "undo"}.contains(name) ||
            (explicitAdoption && name == QStringLiteral("adopt"))) || !validMessageId.match(messageId).hasMatch())) {
        emit error(tr("会话消息标识无效，未提交音乐修改。")); return;
    }
    static const QSet<QString> allowed{"turn", "resume", "pause", "cancel", "adopt", "applied", "undo", "lock", "select_artist"};
    if (!allowed.contains(name)) { emit error(tr("不支持这个任务操作。")); return; }
    if (name == QStringLiteral("select_artist") && !realId(details.value("artistId"))) {
        emit error(tr("请选择曲库返回的有效歌手。")); return;
    }
    if (name == QStringLiteral("resume") && observationOnly &&
        state.value("status").toString() == QStringLiteral("waiting_tools") &&
        !state.value("pendingSteps").toArray().isEmpty()) {
        // The persisted task already owns these leases. Resuming local
        // observation requires no server action and must not restart a model.
        observationOnly = false;
        emit snapshotChanged(snapshot());
        runPending();
        if (!messageId.isEmpty()) emit requestSettled(messageId, true, {});
        return;
    }
    const auto id = state.value("id").toString(); const auto revision = state.value("revision");
    cancelRequests(); discoverLatest = false; connectionState = {}; consecutiveErrors = 0;
    const auto epoch = generation; operation = new QObject(this); setBusy(true);
    QJsonObject body{{"deviceId", installationId}, {"requestId", messageId.isEmpty() ? requestId() : messageId},
        {"catalogContext", NeteaseClient::catalogCacheContext()},
        {"expectedRevision", revision}, {"action", name}};
    if (name == QStringLiteral("turn") && !instruction.isEmpty()) body.insert("instruction", instruction.trimmed());
    // The caller may supply only fields belonging to supported explicit actions.
    if (name == "turn" && details.contains("preference")) {
        const auto value = details.value("preference");
        if (!value.isString() || value.toString().size() > 500) {
            delete operation.data(); operation = nullptr; setBusy(false);
            emit error(tr("音乐偏好内容无效，请重新编辑。")); return;
        }
        body.insert("preference", value.toString());
    }
    if (name == "select_artist") body.insert("artistId", details.value("artistId"));
    if (name == "lock") body.insert("lockedIds", details.value("lockedIds"));
    if (name == "applied") body.insert("adoptionId", details.value("adoptionId"));
    selectedTaskId = id; observationOnly = false;
    backend->musicTaskAction(id, body, operation, [this, epoch, name, instruction, messageId](const QJsonObject &data, const QString &failure) {
        if (epoch != generation) return;
        if (!failure.isEmpty()) {
            if (definiteTaskRejection(data)) { rejected(messageId, failure, epoch); return; }
            failed(failure, epoch);
            if (!messageId.isEmpty()) emit requestSettled(messageId, false, failure);
            return;
        }
        const bool accepted = acceptSnapshot(data, epoch);
        if (accepted && name == QStringLiteral("turn") && epoch == generation &&
            state.value("id") == data.value("task").toObject().value("id"))
            emit instructionAccepted(instruction.trimmed());
        if (!messageId.isEmpty()) emit requestSettled(messageId, accepted, accepted ? QString() : tr("任务操作回执格式无效。"));
    });
}
void MusicTaskClient::poll() {
    if (requestBusy || (state.isEmpty() && !discoverLatest && selectedTaskId.isEmpty()) || !backend->authenticated()) return;
    if (operation) { timer->start(250); return; } // Coalesce SSE bursts with the fallback GET.
    const auto epoch = generation; operation = new QObject(this);
    // Background observation does not disable user controls.
    const auto callback = [this, epoch](const QJsonObject &data, const QString &failure) {
        if (epoch != generation) return;
        if (!failure.isEmpty()) { failed(failure, epoch); return; }
        acceptSnapshot(data, epoch);
    };
    if (discoverLatest) backend->latestMusicTask(installationId, operation, callback);
    else backend->musicTask(selectedTaskId.isEmpty() ? state.value("id").toString() : selectedTaskId, operation, callback);
}
bool MusicTaskClient::acceptSnapshot(const QJsonObject &response, quint64 epoch) {
    if (epoch != generation) return false;
    delete operation.data(); operation = nullptr; setBusy(false);
    if (response.value("task").isNull() || response.value("task").isUndefined()) {
        if (!selectedTaskId.isEmpty()) {
            const auto message = tr("指定任务的回复未包含有效快照，已停止本机执行。请重新找回此会话。");
            connectionState = {{"status", "interrupted"}, {"message", message}};
            timer->stop(); stopEvents(); progressRefreshTimer->stop();
            emit snapshotChanged(snapshot()); emit error(message); return false;
        }
        const bool uncertainCreate = discoverLatest && !connectionState.isEmpty() && state.isEmpty();
        state = {}; connectionState = {}; discoverLatest = false; consecutiveErrors = 0;
        stopEvents(); observedProgress = {};
        emit snapshotChanged(state);
        if (uncertainCreate) emit error(tr("未找到已保存的音乐任务。请确认后台连接正常后重新发送；系统没有自动重复提交。"));
        return false;
    }
    const auto task = response.value("task").toObject();
    const auto id = task.value("id").toString();
    if (id.isEmpty() || (!selectedTaskId.isEmpty() && id != selectedTaskId) ||
        task.value("deviceId").toString() != installationId ||
        !task.value("revision").isDouble() || task.value("revision").toInt() < 1 ||
        task.value("pendingSteps").toArray().size() > 2) {
        const auto message = tr("任务快照无效，已停止本机执行。请检查后台版本后点击“找回任务”。");
        connectionState = {{"status", "interrupted"}, {"message", message}};
        timer->stop(); stopEvents(); progressRefreshTimer->stop();
        const auto contexts = tools; tools.clear();
        for (const auto &context : contexts) delete context.data();
        emit snapshotChanged(snapshot()); emit error(message); return false;
    }
    if (!state.isEmpty() && state.value("id").toString() == id) {
        const bool stale = task.value("revision").toInt() < state.value("revision").toInt() ||
            (task.value("revision").toInt() == state.value("revision").toInt() &&
             task.value("stateVersion").toDouble() < state.value("stateVersion").toDouble());
        if (stale) {
            scheduleProgressCatchup();
            if (!terminal(state.value("status").toString())) timer->start(eventsHealthy ? 5000 : 1000);
            return false;
        }
    }
    const bool changed = task != state || !connectionState.isEmpty();
    state = task; consecutiveErrors = 0; connectionState = {}; discoverLatest = false;
    selectedTaskId = id;
    if (observedProgress.value("taskId").toString() != id ||
        observedProgress.value("revision").toInt() < state.value("revision").toInt() ||
        observedProgress.value("stateVersion").toDouble() < state.value("stateVersion").toDouble())
        observedProgress = {};
    // Prune contexts no longer leased by this snapshot. Late callbacks cannot
    // submit results after a turn, pause, cancellation, or changed lease.
    QSet<QString> leases;
    for (const auto &step : task.value("pendingSteps").toArray()) leases.insert(stepKey(step.toObject()));
    for (auto it = tools.begin(); it != tools.end();) {
        if (!leases.contains(it.key())) { delete it.value().data(); it = tools.erase(it); }
        else ++it;
    }
    for (auto it = resultOutbox.begin(); it != resultOutbox.end();)
        if (!leases.contains(it.key())) { resultAttempts.remove(it.key()); it = resultOutbox.erase(it); }
        else ++it;
    if (changed) emit snapshotChanged(snapshot());
    if (epoch != generation) return false;
    if (!observationOnly) runPending();
    if (!terminal(state.value("status").toString())) {
        // A healthy stream sends immediate changes; retain a slower safety GET
        // for missed events. Without SSE, retain the original one-second poll.
        timer->start(eventsHealthy ? 5000 : 1000);
        ensureEvents();
    } else {
        timer->stop(); stopEvents(); progressRefreshTimer->stop();
    }
    scheduleProgressCatchup();
    return true;
}

void MusicTaskClient::stopEvents() {
    eventReconnectTimer->stop();
    eventsHealthy = false;
    auto old = eventContext; eventContext = nullptr;
    delete old.data();
}

void MusicTaskClient::ensureEvents() {
    if (eventContext || eventsUnsupported || eventReconnectTimer->isActive() ||
        !backend->authenticated() || state.value("id").toString().isEmpty() ||
        terminal(state.value("status").toString()) ||
        (terminal(observedProgress.value("status").toString()) &&
         observedProgress.value("stateVersion").toDouble() > state.value("stateVersion").toDouble())) return;
    const auto epoch = generation;
    const auto id = state.value("id").toString();
    auto *context = new QObject(this); eventContext = context;
    const qint64 lastVersion = qint64(qMax(state.value("stateVersion").toDouble(),
        observedProgress.value("stateVersion").toDouble()));
    backend->musicTaskEvents(id, lastVersion, context,
        [this, epoch, context](const QJsonObject &progress, const QString &) {
            if (epoch != generation || eventContext != context) return;
            eventFailures = 0; eventsHealthy = true;
            if (!terminal(state.value("status").toString())) timer->start(5000);
            receiveProgress(progress, epoch);
        },
        [this, epoch, id, context](int status, const QString &) {
            if (epoch != generation || eventContext != context) return;
            eventContext = nullptr; eventsHealthy = false; context->deleteLater();
            if (!terminal(state.value("status").toString()) &&
                (!timer->isActive() || timer->remainingTime() > 1000)) timer->start(1000);
            // Older backends remain fully usable through the existing safe GET.
            if (status == 404 || status == 405 || status == 501) eventsUnsupported = true;
            if (eventsUnsupported || !backend->authenticated() || state.value("id").toString() != id ||
                terminal(state.value("status").toString())) return;
            if (terminal(observedProgress.value("status").toString())) {
                if (!progressRefreshTimer->isActive()) progressRefreshTimer->start(100);
                return;
            }
            const int attempt = qMin(eventFailures++, 5);
            eventReconnectTimer->start(qMin(30000, 1000 * (1 << attempt)));
        });
}

void MusicTaskClient::receiveProgress(const QJsonObject &progress, quint64 epoch) {
    if (epoch != generation || progress.value("taskId").toString() != state.value("id").toString()) return;
    const double version = progress.value("stateVersion").toDouble(-1);
    const int revision = progress.value("revision").toInt();
    if (revision < state.value("revision").toInt() || version < state.value("stateVersion").toDouble() ||
        (!observedProgress.isEmpty() &&
            (revision < observedProgress.value("revision").toInt() ||
             version <= observedProgress.value("stateVersion").toDouble()))) return;
    observedProgress = progress;
    progressCatchupAttempts = 0;
    emit snapshotChanged(snapshot());
    scheduleProgressCatchup();
}

void MusicTaskClient::scheduleProgressCatchup() {
    if (observedProgress.value("taskId").toString() != state.value("id").toString() ||
        observedProgress.value("stateVersion").toDouble() <= state.value("stateVersion").toDouble()) {
        progressCatchupAttempts = 0; return;
    }
    // A response can have been read just before a newer event arrived. Fetch
    // again after it completes, with bounded backoff if a replica stays stale.
    if (!progressRefreshTimer->isActive())
        progressRefreshTimer->start(qMin(5000, 100 * (1 << qMin(progressCatchupAttempts++, 6))));
}
void MusicTaskClient::runPending() {
    if (observationOnly) return;
    if (state.value("status").toString() != QStringLiteral("waiting_tools")) return;
    const auto epoch = generation;
    for (const auto &value : state.value("pendingSteps").toArray()) {
        const auto step = value.toObject(); const auto key = stepKey(step);
        const auto skill = step.value("skill").toString();
        if (completedSteps.contains(key) || tools.contains(key)) continue;
        if ((skill != "scene.research" && skill != "releases.research") || step.value("version").toInt() != 1 ||
            step.value("id").toString().isEmpty() || step.value("leaseId").toString().isEmpty() ||
            !step.value("input").isObject()) {
            emit error(tr("任务要求的工具或版本不受支持，已停止本机执行。"));
            if (!step.value("id").toString().isEmpty() && !step.value("leaseId").toString().isEmpty()) {
                auto *context = new QObject(this); tools.insert(key, context);
                QJsonObject body{{"deviceId", installationId}, {"leaseId", step.value("leaseId")}, {"revision", step.value("revision")},
                    {"catalogContext", NeteaseClient::catalogCacheContext()},
                    {"error", QJsonObject{{"code", "unsupported_tool_version"}, {"message", "Unsupported catalog tool or version"}}}};
                resultOutbox.insert(key, body); sendStepResult(key, step, body, context, epoch);
            } else completedSteps.insert(key);
            continue;
        }
        auto *context = new QObject(this); tools.insert(key, context);
        if (resultOutbox.contains(key)) {
            sendStepResult(key, step, resultOutbox.value(key), context, epoch);
            continue;
        }
        auto work = std::make_shared<Research>(); work->music = music; work->context = context;
        work->input = step.value("input").toObject();
        const auto expiresAt = timestamp(step.value("expiresAt"));
        if (expiresAt > 0) work->timeoutMs = int(qBound<qint64>(1, expiresAt - QDateTime::currentMSecsSinceEpoch() - 1500, 40000));
        work->done = [this, epoch, key, step, context](const QJsonObject &result, const QString &failure) {
            if (epoch != generation || !tools.contains(key) || tools.value(key) != context) return;
            QJsonObject body{{"deviceId", installationId}, {"leaseId", step.value("leaseId")}, {"revision", step.value("revision")},
                {"catalogContext", NeteaseClient::catalogCacheContext()}};
            if (failure.isEmpty()) body.insert("result", result);
            else body.insert("error", QJsonObject{{"code", "local_tool_failed"}, {"message", failure.left(500)}});
            resultOutbox.insert(key, body);
            sendStepResult(key, step, body, context, epoch);
        };
        work->start(skill);
    }
}

void MusicTaskClient::sendStepResult(const QString &key, const QJsonObject &step,
                                    const QJsonObject &body, QObject *context, quint64 epoch) {
    const auto id = state.value("id").toString();
    resultAttempts.insert(key, resultAttempts.value(key) + 1);
    backend->completeMusicTaskStep(id, step.value("id").toString(), body, context,
        [this, epoch, key](const QJsonObject &data, const QString &errorText) {
            if (epoch != generation) return;
            auto old = tools.take(key); delete old.data();
            if (!errorText.isEmpty()) {
                // Observe first. Only an unchanged live lease may resend the
                // exact same result (server step-ID + result-hash idempotency),
                // never repeat metadata research or a paid model request.
                if (resultAttempts.value(key) >= 3) {
                    completedSteps.insert(key); resultOutbox.remove(key);
                    emit error(tr("查询结果未能确认送达，进度已保留；请恢复任务后继续。"));
                } else emit error(errorText);
                timer->start(1500); return;
            }
            completedSteps.insert(key); resultOutbox.remove(key); resultAttempts.remove(key);
            acceptSnapshot(data, epoch);
        });
}
