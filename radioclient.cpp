#include "radioclient.h"
#include "appbackendclient.h"
#include "neteaseclient.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QTimer>
#include <QDateTime>
#include <QTimeZone>
#include <cmath>
#include <algorithm>

namespace {
qint64 idOf(const QJsonValue &value) {
    const double id = value.toDouble();
    return value.isDouble() && id > 0 && id <= 9007199254740991. && std::floor(id) == id
               ? qint64(id) : 0;
}
QJsonArray strings(const QStringList &items) {
    QJsonArray result; for (const auto &item : items) result.append(item); return result;
}
bool text(const QJsonValue &value, int maximum, bool empty = true) {
    return value.isString() && value.toString().size() <= maximum &&
           (empty || !value.toString().trimmed().isEmpty());
}
QString normalizedArtist(QString name) {
    name = name.normalized(QString::NormalizationForm_KC).toCaseFolded();
    name.remove(QRegularExpression(QStringLiteral("[\\s\\p{P}\\p{S}]+")));
    return name;
}
OnlineSong songFrom(const QJsonObject &value) {
    OnlineSong song;
    song.id=idOf(value.value("id"));song.title=value.value("title").toString().left(120);
    song.artist=value.value("artist").toString().left(120);song.album=value.value("album").toString().left(120);
    song.coverUrl=value.value("coverUrl").toString().left(2048);
    song.durationMs=value.value("durationMs").toInt();song.fee=value.value("fee").toInt();
    return song;
}
}

struct RadioClient::Work {
    quint64 revision;
    QPointer<QObject> context;
    Callback callback;
    QJsonObject base;
    QStringList queries, excludedArtists;
    QVector<qint64> excludedIds;
    QVector<QPair<qint64, QString>> playlists;
    QSet<qint64> seenPlaylists, seenSongs;
    QVector<RadioRecommendation> candidates;
    int searchIndex = 0, playlistIndex = 0, loaded = 0, attempted = 0;
    QString lastError;
    AssistantArtist artist;
    QJsonArray albums;
    int albumIndex = 0;
    qint64 checkedAt = 0;
    QStringList releaseSummary;
};

RadioClient::RadioClient(QObject *parent) : QObject(parent), music(new NeteaseClient(this)) {
    connect(AppBackendClient::instance(),&AppBackendClient::authChanged,this,[this]{reset();});
}
RadioClient::~RadioClient() { cancel(); }
void RadioClient::cancel() { ++revision; delete operation.data(); operation = nullptr; }
void RadioClient::reset() { cancel(); usedPlaylists.clear(); }
bool RadioClient::current(const std::shared_ptr<Work> &work) const {
    return work->context && operation == work->context && work->revision == revision;
}
void RadioClient::complete(const std::shared_ptr<Work> &work, const RadioResult &result,
                           const QString &error) {
    if (!current(work)) return;
    auto callback = work->callback;
    delete operation.data(); operation = nullptr;
    callback(result, error);
}

void RadioClient::generate(const QString &instruction, const QStringList &history,
                           const QString &preference, const QVector<qint64> &excludedIds,
                           QObject *context, Callback callback) {
    generateImpl(instruction,history,preference,excludedIds,context,callback,false);
}

void RadioClient::assist(const QString &instruction, const QStringList &history,
                         const QString &preference, const QVector<qint64> &excludedIds,
                         QObject *context, Callback callback) {
    generateImpl(instruction,history,preference,excludedIds,context,callback,true);
}

std::shared_ptr<RadioClient::Work> RadioClient::start(QObject *context, Callback callback) {
    cancel();
    if (!context || !callback) return {};
    operation = new QObject(context);
    auto work = std::make_shared<Work>();
    work->revision = revision; work->context = operation; work->callback = callback;
    QTimer::singleShot(60000,operation,[this,work] {
        if(current(work))complete(work,{},tr("这次查找等待较久，已停止；可以稍后重试。"));
    });
    return work;
}

void RadioClient::generateImpl(const QString &instruction, const QStringList &history,
                               const QString &preference, const QVector<qint64> &excludedIds,
                               QObject *context, Callback callback, bool assistant) {
    auto work=start(context,callback);if(!work)return;
    if (instruction.trimmed().isEmpty() || instruction.size() > 1000 || preference.size() > 500) {
        complete(work,{},tr("请用 1 至 1000 个字符描述这次想听的音乐。")); return;
    }
    QStringList recent = history.mid(qMax(0, history.size()-4));
    for (auto &item : recent) item = item.left(1000);
    work->base = {{"instruction",instruction.trimmed()},{"history",strings(recent)},
                  {"preference",preference}};
    // Retain the current request and explicit preference even for long UTF-8
    // conversations, while fitting the planning endpoint's 16 KiB body limit.
    while(QJsonDocument(work->base).toJson(QJsonDocument::Compact).size()>15*1024 && !recent.isEmpty()) {
        recent.removeFirst();work->base.insert("history",strings(recent));
    }
    work->excludedIds = excludedIds.mid(qMax(0, excludedIds.size()-60));
    emit stageChanged(tr("正在理解你此刻想听的音乐…"));
    auto planned=[this,work,assistant](const QJsonObject &data,const QString &error) {
            if (!current(work)) return;
            if (!error.isEmpty()) { complete(work,{},error); return; }
            if(assistant) {
                const QString kind=data.value("kind").toString();
                if(!text(data.value("note"),600)) {complete(work,{},tr("助手返回的说明无效，请重试。"));return;}
                if(kind=="releases") {
                    if(!text(data.value("artist"),100,false)) {complete(work,{},tr("请补充想查的歌手。"));return;}
                    findArtists(work,data.value("artist").toString());return;
                }
                if(kind=="remember" || kind=="forget" || kind=="clarify") {
                    RadioResult result;result.kind=kind;result.note=data.value("note").toString();
                    if(kind=="remember") {
                        if(!text(data.value("preference"),500,false)) {complete(work,{},tr("口味摘要无效，请重试或手动编辑。"));return;}
                        result.preference=data.value("preference").toString();
                    }
                    complete(work,result,{});return;
                }
                if(kind!="radio") {complete(work,{},tr("暂时无法理解这个音乐请求，请补充歌手或场景。"));return;}
            }
            const auto queries = data.value("queries").toArray();
            const auto artists = data.value("excludedArtists").toArray();
            if (!text(data.value("title"),80,false) || queries.isEmpty() || queries.size()>2 ||
                !data.value("excludedArtists").isArray() || artists.size()>20) {
                complete(work,{},tr("AI 没有返回有效的找歌方向，请重试。")); return;
            }
            for (const auto &query:queries) {
                if (!text(query,80,false)) { complete(work,{},tr("AI 返回的检索词无效。")); return; }
                work->queries.append(query.toString().trimmed());
            }
            for (const auto &artist:artists) {
                if (!text(artist,100,false)) { complete(work,{},tr("AI 返回的歌手条件无效。")); return; }
                work->excludedArtists.append(artist.toString().trimmed());
            }
            searchNext(work);
        };
    if(assistant)AppBackendClient::instance()->planAssistant(work->base,operation,planned);
    else AppBackendClient::instance()->planRadio(work->base,operation,planned);
}

void RadioClient::findArtists(const std::shared_ptr<Work> &work, const QString &query) {
    if(!current(work))return;
    emit stageChanged(tr("正在曲库中核对歌手“%1”…").arg(query));
    QUrlQuery parameters;parameters.addQueryItem("keywords",query);parameters.addQueryItem("limit","5");
    music->get("/assistant/artists",parameters,operation,[this,work,query](const QJsonObject &data,const QString &error) {
        if(!current(work))return;
        if(!error.isEmpty()){complete(work,{},error);return;}
        QVector<AssistantArtist> artists,matches;QSet<qint64> seen;
        const auto values=data.value("artists").toArray();
        for(int i=0;i<values.size() && i<5;i++) {
            const auto row=values.at(i).toObject();AssistantArtist artist;
            artist.id=idOf(row.value("id"));artist.name=row.value("name").toString().left(120);
            artist.coverUrl=row.value("coverUrl").toString().left(2048);
            if(!artist.id || artist.name.trimmed().isEmpty() || seen.contains(artist.id))continue;
            for(const auto &alias:row.value("aliases").toArray())
                if(text(alias,120,false) && artist.aliases.size()<10)artist.aliases.append(alias.toString());
            bool exact=normalizedArtist(query)==normalizedArtist(artist.name);
            for(const auto &alias:artist.aliases)exact|=normalizedArtist(query)==normalizedArtist(alias);
            seen.insert(artist.id);artists.append(artist);if(exact)matches.append(artist);
        }
        // A familiar stage name can also belong to unrelated exact-name users.
        // Never let an exact-name duplicate outrank another search identity.
        if(artists.size()==1 && matches.size()==1){fetchReleases(work,matches.first());return;}
        RadioResult result;result.kind=QStringLiteral("artists");result.artists=artists;
        result.note=artists.isEmpty()?tr("曲库中暂时没有找到这位歌手，请补充完整姓名。")
            :tr("为避免找错歌手，请先选择你想查询的歌手，再查看新作。");
        complete(work,result,{});
    });
}

void RadioClient::releasesForArtist(const AssistantArtist &artist, QObject *context, Callback callback) {
    auto work=start(context,callback);if(!work)return;
    if(!AppBackendClient::instance()->authenticated()) {complete(work,{},tr("请先登录探歌，再查询歌手新作。"));return;}
    if(artist.id<=0 || artist.id>9007199254740991LL || artist.name.trimmed().isEmpty() || artist.name.size()>120) {
        complete(work,{},tr("歌手信息无效，请重新搜索。"));return;
    }
    fetchReleases(work,artist);
}

void RadioClient::fetchReleases(const std::shared_ptr<Work> &work, const AssistantArtist &artist) {
    if(!current(work))return;
    work->artist=artist;
    emit stageChanged(tr("正在核对 %1 的发行日期…").arg(artist.name));
    QUrlQuery parameters;parameters.addQueryItem("artistId",QString::number(artist.id));parameters.addQueryItem("limit","6");
    music->get("/assistant/releases",parameters,operation,[this,work](const QJsonObject &data,const QString &error) {
        if(!current(work))return;
        if(!error.isEmpty()){complete(work,{},error);return;}
        work->checkedAt=idOf(data.value("checkedAt"));
        const auto now=QDateTime::currentMSecsSinceEpoch();
        if(idOf(data.value("artist").toObject().value("id"))!=work->artist.id ||
           work->checkedAt<now-300000 || work->checkedAt>now+300000) {
            complete(work,{},tr("歌手或查询时间校验失败，请稍后重试。"));return;
        }
        QList<QJsonObject> sorted;QSet<qint64> seen;
        const auto values=data.value("albums").toArray();
        for(int i=0;i<values.size() && i<6;i++) {
            const auto album=values.at(i).toObject();const auto id=idOf(album.value("id"));
            const auto published=idOf(album.value("publishTime"));
            if(!id || seen.contains(id) || !published || published>work->checkedAt || !text(album.value("name"),160,false))continue;
            seen.insert(id);sorted.append(album);
        }
        std::stable_sort(sorted.begin(),sorted.end(),[](const QJsonObject &a,const QJsonObject &b) {
            return a.value("publishTime").toDouble()>b.value("publishTime").toDouble();
        });
        for(const auto &album:sorted)work->albums.append(album);
        if(work->albums.isEmpty()) {
            RadioResult result;result.kind=QStringLiteral("clarify");
            result.note=tr("截至 %1，本次网易云查询没有返回 %2 可核验发行日期的作品，暂时无法确认新歌；这不代表全网没有发行。")
                .arg(QDateTime::fromMSecsSinceEpoch(work->checkedAt,Qt::UTC).toTimeZone(QTimeZone("Asia/Shanghai")).toString("yyyy-MM-dd"),work->artist.name);
            complete(work,result,{});return;
        }
        fetchAlbum(work);
    });
}

void RadioClient::fetchAlbum(const std::shared_ptr<Work> &work) {
    if(!current(work))return;
    if(work->loaded>=2 || work->albumIndex>=3 || work->albumIndex>=work->albums.size() || work->candidates.size()>=12) {
        RadioResult result;result.kind=QStringLiteral("releases");
        result.title=tr("%1 · 曲库近期发行").arg(work->artist.name);result.tracks=work->candidates;
        const auto latest=work->albums.first().toObject();
        const auto date=[](qint64 ms) {return QDateTime::fromMSecsSinceEpoch(ms,Qt::UTC).toTimeZone(QTimeZone("Asia/Shanghai")).toString("yyyy-MM-dd");};
        QStringList summaries;
        for(int i=0;i<work->albums.size() && i<3;i++) {
            const auto album=work->albums.at(i).toObject();
            const QString type=album.value("type").toString();
            const QString label=type.compare(QStringLiteral("Single"),Qt::CaseInsensitive)==0?tr("单曲")
                :type.contains(QStringLiteral("EP"),Qt::CaseInsensitive)?tr("EP")
                :type.compare(QStringLiteral("Album"),Qt::CaseInsensitive)==0 || type==QStringLiteral("专辑")?tr("专辑"):tr("发行");
            QString name=album.value("name").toString();if(name.size()>32)name=name.left(31)+QChar(0x2026);
            summaries.append(tr("《%1》%2 · %3").arg(name,label,date(idOf(album.value("publishTime")))));
        }
        const bool recent=idOf(latest.value("publishTime"))>=work->checkedAt-90LL*24*60*60*1000;
        result.note=tr("查询日期：%1（北京时间）。仅为网易云本次可查条目，按发行日期排列，不代表全网完整目录。%2%3。")
            .arg(date(work->checkedAt),recent?tr("近 90 天可查到发行；最近条目："):tr("本次条目中近 90 天未见新发行；最近可查作品："),summaries.join(QStringLiteral("；")));
        if(result.tracks.isEmpty())result.note+=tr(" 发行记录已找到，但暂未读取到可展示曲目，请稍后再试。");
        else result.note+=tr(" 下方展示已读取作品中的最多 12 首；完整播放以账号权限为准。");
        complete(work,result,{});return;
    }
    const auto album=work->albums.at(work->albumIndex++).toObject();
    const auto id=idOf(album.value("id"));
    emit stageChanged(tr("正在读取《%1》的真实曲目…").arg(album.value("name").toString()));
    QUrlQuery parameters;parameters.addQueryItem("id",QString::number(id));parameters.addQueryItem("limit","20");
    music->get("/assistant/album",parameters,operation,[this,work,album,id](const QJsonObject &data,const QString &error) {
        if(!current(work))return;
        const auto source=data.value("album").toObject();
        if(!error.isEmpty() || idOf(source.value("id"))!=id || idOf(source.value("artistId"))!=work->artist.id ||
           idOf(source.value("publishTime"))!=idOf(album.value("publishTime"))) {
            work->lastError=error.isEmpty()?tr("专辑身份或发行日期校验失败。"):error;fetchAlbum(work);return;
        }
        const int before=work->candidates.size();
        const QString name=source.value("name").toString().left(120);
        const QString date=QDateTime::fromMSecsSinceEpoch(idOf(source.value("publishTime")),Qt::UTC)
            .toTimeZone(QTimeZone("Asia/Shanghai")).toString("yyyy-MM-dd");
        const auto songs=data.value("songs").toArray();
        for(int i=0;i<songs.size() && i<20 && work->candidates.size()<12;i++) {
            auto song=songFrom(songs.at(i).toObject());
            if(!song.id || song.title.trimmed().isEmpty() || work->seenSongs.contains(song.id))continue;
            work->seenSongs.insert(song.id);
            work->candidates.append({song,tr("《%1》· %2 发行").arg(name,date),name,{}});
        }
        if(work->candidates.size()>before)++work->loaded;
        fetchAlbum(work);
    });
}

void RadioClient::searchNext(const std::shared_ptr<Work> &work) {
    if (!current(work)) return;
    if (work->searchIndex >= work->queries.size()) { loadNext(work); return; }
    const QString query = work->queries.at(work->searchIndex++);
    emit stageChanged(tr("正在曲库中寻找“%1”…").arg(query));
    QUrlQuery parameters;parameters.addQueryItem("keywords",query);
    parameters.addQueryItem("limit","5");parameters.addQueryItem("offset","0");
    music->get("/radio/search",parameters,operation,[this,work](const QJsonObject &data,const QString &error) {
        if (!current(work)) return;
        if (!error.isEmpty()) work->lastError=error;
        const auto playlists=data.value("playlists").toArray();
        bool first=true;
        for (int i=0;i<playlists.size() && i<5;i++) {
            const auto value=playlists.at(i);
            const auto playlist=value.toObject();const qint64 id=idOf(playlist.value("id"));
            const QString name=playlist.value("name").toString().left(160);
            if (!id || name.isEmpty() || work->seenPlaylists.contains(id) || usedPlaylists.contains(id)) continue;
            work->seenPlaylists.insert(id);
            if (first && work->searchIndex>1 && !work->playlists.isEmpty())
                work->playlists.insert(1,{id,name});
            else work->playlists.append({id,name});
            first=false;
        }
        searchNext(work);
    });
}

void RadioClient::loadNext(const std::shared_ptr<Work> &work) {
    if (!current(work)) return;
    if (work->loaded>=2 || work->attempted>=4 || work->playlistIndex>=work->playlists.size()) {
        if (work->candidates.isEmpty()) complete(work,{},work->lastError.isEmpty()
            ? tr("这次没有找到新的合适歌曲，试试补充歌手、语言或场景，再换个方向。") : work->lastError);
        else select(work);
        return;
    }
    const auto playlist=work->playlists.at(work->playlistIndex++);++work->attempted;
    emit stageChanged(tr("正在从“%1”挑选真实歌曲…").arg(playlist.second));
    QUrlQuery parameters;parameters.addQueryItem("id",QString::number(playlist.first));parameters.addQueryItem("limit","20");
    music->get("/radio/playlist",parameters,operation,[this,work,playlist](const QJsonObject &data,const QString &error) {
        if (!current(work)) return;
        if (!error.isEmpty()) { work->lastError=error;loadNext(work);return; }
        const auto source=data.value("playlist").toObject();
        if (idOf(source.value("id"))!=playlist.first) { work->lastError=tr("歌单来源校验失败，请重试。");loadNext(work);return; }
        QStringList tags;for (const auto &tag:source.value("tags").toArray())
            if (tag.isString() && tags.size()<6) tags.append(tag.toString().left(40));
        const int before=work->candidates.size();
        const auto values=data.value("songs").toArray();
        for (int i=0;i<values.size() && i<20 && work->candidates.size()<40;i++) {
            const auto value=values.at(i).toObject();OnlineSong song;
            song.id=idOf(value.value("id"));song.title=value.value("title").toString().left(120);
            song.artist=value.value("artist").toString().left(120);song.album=value.value("album").toString().left(120);
            song.coverUrl=value.value("coverUrl").toString().left(2048);song.durationMs=value.value("durationMs").toInt();song.fee=value.value("fee").toInt();
            if (!song.id || song.title.isEmpty() || work->seenSongs.contains(song.id) || work->excludedIds.contains(song.id)) continue;
            const auto artists=song.artist.split(QRegularExpression("\\s*/\\s*"),Qt::SkipEmptyParts);
            bool excluded=false;for(const auto &artist:artists) for(const auto &avoid:work->excludedArtists)
                if (artist.compare(avoid,Qt::CaseInsensitive)==0) excluded=true;
            if (excluded) continue;
            work->seenSongs.insert(song.id);
            work->candidates.append({song,{},source.value("name").toString(playlist.second).left(160),tags});
        }
        if (work->candidates.size()>before) {
            ++work->loaded;usedPlaylists.append(playlist.first);
            while(usedPlaylists.size()>60)usedPlaylists.removeFirst();
        }
        loadNext(work);
    });
}

void RadioClient::select(const std::shared_ptr<Work> &work) {
    if (!current(work)) return;
    emit stageChanged(tr("正在为你编排这一段音乐…"));
    auto body=work->base;QJsonArray candidates,excluded;
    for (const auto &track:work->candidates) candidates.append(QJsonObject{
        {"id",track.song.id},{"title",track.song.title},{"artist",track.song.artist},
        {"album",track.song.album},{"sourceName",track.sourceName},{"sourceTags",strings(track.sourceTags)}});
    for (const auto id:work->excludedIds)excluded.append(id);
    body.insert("candidates",candidates);body.insert("excludedIds",excluded);
    while (QJsonDocument(body).toJson(QJsonDocument::Compact).size()>60*1024 && candidates.size()>1) {
        candidates.removeLast();work->candidates.removeLast();body.insert("candidates",candidates);
    }
    AppBackendClient::instance()->selectRadio(body,operation,[this,work](const QJsonObject &data,const QString &error) {
        if (!current(work)) return;
        if (!error.isEmpty()) { complete(work,{},error);return; }
        const auto selected=data.value("tracks").toArray();
        if (!text(data.value("title"),80,false) || !text(data.value("note"),600) || selected.isEmpty() || selected.size()>12) {
            complete(work,{},tr("AI 没有返回有效电台，请换个描述重试。"));return;
        }
        RadioResult result;result.title=data.value("title").toString();result.note=data.value("note").toString();QSet<qint64> seen;
        for (const auto &value:selected) {
            const auto row=value.toObject();const qint64 id=idOf(row.value("id"));
            const auto found=std::find_if(work->candidates.cbegin(),work->candidates.cend(),[id](const RadioRecommendation &r){return r.song.id==id;});
            if (!id || seen.contains(id) || work->excludedIds.contains(id) || found==work->candidates.cend() || !text(row.value("reason"),160,false)) {
                complete(work,{},tr("推荐结果包含无效或重复歌曲，已停止，请重试。"));return;
            }
            seen.insert(id);auto recommendation=*found;recommendation.reason=row.value("reason").toString();result.tracks.append(recommendation);
        }
        complete(work,result,{});
    });
}
