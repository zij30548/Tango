#ifndef INDEPENDENT_CATALOG_FIXTURE_H
#define INDEPENDENT_CATALOG_FIXTURE_H

#include <QDateTime>
#include <QBuffer>
#include <QDataStream>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <memory>

// 独立评测使用不随题目变化的合成曲库；来源标签只能证明元数据，不能证明真实听感。
class IndependentCatalogFixture : public QObject {
public:
    struct Pool { int id; QString name; QStringList tags, terms; QJsonArray songs; };
    QTcpServer server;
    QJsonArray requests;
    QVector<Pool> pools;
    QJsonArray artists;
    QHash<int,QJsonObject> tracks;
    QHash<int,QJsonArray> fixedReleases;
    QHash<int,QJsonObject> fixedAlbums;
    QDateTime anchor;
    int delayMs = 0;
    QString untrustedToolDescription;

    explicit IndependentCatalogFixture(QDateTime frozenAnchor = QDateTime::currentDateTimeUtc(),bool listen=true) : anchor(frozenAnchor) {
        artists = {artist(1001,"陈奕迅",{}),artist(1002,"G.E.M.邓紫棋",{"邓紫棋","GEM","G.E.M."}),
            artist(1003,"周杰伦",{}),artist(1004,"陶喆",{"David Tao"}),artist(1005,"林俊杰",{"JJ Lin"})};
        addPool(101,"跑步 · 轻快电子与摇滚",{"跑步","轻快","电子","摇滚"},{"跑步","运动","健身","活力","轻快","电子","摇滚","running","energetic"},"运动乐团");
        addPool(102,"专注学习 · 钢琴纯音乐",{"学习","钢琴","纯音乐"},{"学习","工作","专注","阅读","读书","钢琴","纯音乐","无人声","instrumental","piano","study"},"器乐合奏团");
        addPool(103,"微醺夜晚 · 爵士与华语 R&B",{"微醺","爵士","R&B","华语"},{"喝酒","饮酒","微醺","夜晚","酒吧","爵士","r&b","rnb","jazz","drinking"},"夜色乐团");
        addPool(104,"入睡时刻 · 安静氛围",{"睡眠","安静","氛围"},{"睡觉","睡眠","入睡","助眠","安静","舒缓","氛围","ambient","sleep","relax"},"月光合奏团");
        addPool(105,"沿途风景 · 驾车流行",{"驾车","流行","旅行"},{"开车","驾车","旅行","公路","流行","通勤","drive","pop"},"沿途乐团");
        addPool(106,"粤语夜色 · R&B",{"粤语","R&B","抒情"},{"粤语","广东","r&b","rnb","抒情","cantonese"},"陈奕迅");
        const QStringList titles = {"十年","浮夸","泡沫","光年之外","晴天","稻香","流沙","普通朋友","江南","修炼爱情"};
        for (int index=0; index<titles.size(); ++index) {
            const int artistIndex=index/2;
            tracks.insert(5001+index, song(5001+index,titles.at(index),artists.at(artistIndex).toObject().value("name").toString(),"录音室专辑"));
        }
        if(listen)server.listen(QHostAddress::LocalHost,0);
        connect(&server,&QTcpServer::newConnection,this,[this] {
            while(server.hasPendingConnections()) {
                auto *socket=server.nextPendingConnection();
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                auto bytes=std::make_shared<QByteArray>();auto handled=std::make_shared<bool>(false);
                connect(socket,&QTcpSocket::readyRead,this,[this,socket,bytes,handled] {
                    bytes->append(socket->readAll());if(*handled)return;
                    const int end=bytes->indexOf("\r\n\r\n");if(end<0)return;
                    const auto headers=bytes->left(end);int length=0;
                    for(const auto &line:headers.split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(line.indexOf(':')+1).trimmed().toInt();
                    if(bytes->size()<end+4+length)return;*handled=true;
                    const QUrl url=QUrl::fromEncoded(headers.split(' ').value(1));
                    if(url.path()=="/audio.wav"){
                        static const QByteArray wav=audio();qint64 begin=0,last=wav.size()-1;bool ranged=false;
                        for(const auto &line:headers.split('\n')){const auto header=line.trimmed().toLower();if(!header.startsWith("range: bytes="))continue;const auto span=header.mid(13).split('-');begin=span.value(0).toLongLong();if(span.size()>1&&!span.at(1).isEmpty())last=qMin(last,span.at(1).toLongLong());ranged=true;}
                        begin=qBound(qint64(0),begin,qint64(wav.size()-1));last=qMax(begin,last);const auto body=wav.mid(begin,last-begin+1);
                        const QByteArray range=ranged?"Content-Range: bytes "+QByteArray::number(begin)+"-"+QByteArray::number(last)+"/"+QByteArray::number(wav.size())+"\r\n":QByteArray();
                        socket->write("HTTP/1.1 "+QByteArray(ranged?"206":"200")+" Fixture\r\nContent-Type: audio/wav\r\nAccept-Ranges: bytes\r\nConnection: close\r\n"+range+"Content-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);socket->disconnectFromHost();return;
                    }
                    int status=200;QJsonArray returned;const auto body=respond(url,status,returned);
                    QJsonObject query;for(const auto &item:QUrlQuery(url).queryItems())query.insert(item.first,item.second);
                    const bool toolRoute=url.path()=="/search"||url.path().startsWith("/radio/")||url.path().startsWith("/assistant/");
                    const int requestIndex=requests.size();requests.append(QJsonObject{{"path",url.path()},{"query",query},{"responseIds",QJsonArray()},
                        {"scheduledResponseIds",returned},{"delivered",false},{"delayMs",toolRoute?delayMs:0},{"status",status}});
                    if(url.path()=="/search"&&returned.contains(70001)&&!untrustedToolDescription.isEmpty()){
                        auto row=requests.at(requestIndex).toObject();row.insert("untrustedFields",QJsonArray{QJsonObject{{"trackId",70001},{"field","song.description"},{"value",untrustedToolDescription}}});requests.replace(requestIndex,row);
                    }
                    const auto encoded=QJsonDocument(body).toJson(QJsonDocument::Compact);
                    const auto payload="HTTP/1.1 "+QByteArray::number(status)+" Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "+QByteArray::number(encoded.size())+"\r\n\r\n"+encoded;
                    QPointer<QTcpSocket> guard(socket);QTimer::singleShot(toolRoute?delayMs:0,this,[this,guard,payload,requestIndex,returned]{
                        if(guard&&guard->state()==QAbstractSocket::ConnectedState){guard->write(payload);guard->disconnectFromHost();auto row=requests.at(requestIndex).toObject();row.insert("delivered",true);row.insert("responseIds",returned);requests.replace(requestIndex,row);}
                    });
                });
            }
        });
    }
    QByteArray base() const { return "http://127.0.0.1:"+QByteArray::number(server.serverPort()); }
    // 只接收冻结夹具定义，不读取题面、金标或本轮路由结果。
    void loadDefinitions(const QJsonObject &definitions) {
        QJsonArray coreSongs;
        for(const auto &value:definitions.value("catalog_core").toObject().value("tracks").toArray()) {
            const auto row=value.toObject();const QString symbolic=row.value("id").toString();const int index=symbolic.section('_',-1).toInt();if(index<1||index>100)continue;
            auto track=song(70000+index,row.value("title").toString(),row.value("artist").toString(),"独立新题合成作品集");
            track.insert("symbolicId",symbolic);track.insert("tags",row.value("tags"));track.insert("version",row.value("version"));track.insert("durationMs",row.value("duration_seconds").toInt()*1000);
            tracks.insert(70000+index,track);coreSongs.append(track);
        }
        const QList<QList<int>> groups{{1,4,5,11},{6,7,9,12},{2,8},{3,10}};
        const QStringList names{"原声器乐 · 轻柔无歌词","轻电子器乐 · 中速律动","民谣女声 · 晚风","现场摇滚 · 快节奏男声"};
        const QList<QStringList> terms{{"原声","器乐","轻柔","无歌词","舒缓","阔叶","南渡"},{"轻电子","电子","器乐","无歌词","律动","中速","纸桥"},{"民谣","女声","舒缓","许栀","晚风"},{"现场","摇滚","男声","快节奏","北折线","罗帆"}};
        for(int index=0;index<groups.size();++index){QJsonArray songs;for(int id:groups.at(index))if(tracks.contains(70000+id))songs.append(tracks.value(70000+id));
            if(!songs.isEmpty())pools.append({107+index,names.at(index),terms.at(index),terms.at(index),songs});}
        const auto definedArtists=definitions.value("artist_identity_and_releases").toObject().value("artists").toArray();
        for(int index=0;index<definedArtists.size();++index){const auto source=definedArtists.at(index).toObject();const int artistId=3001+index;
            auto row=artist(artistId,source.value("name").toString(),{source.value("identity").toString()});row.insert("symbolicId",source.value("id"));row.insert("identity",source.value("identity"));artists.append(row);
            QJsonArray albums;const auto releases=source.value("releases").toArray();
            for(int releaseIndex=0;releaseIndex<releases.size();++releaseIndex){const auto release=releases.at(releaseIndex).toObject();const int albumId=artistId*10+releaseIndex+1;
                const auto published=QDateTime::fromString(release.value("release_date").toString()+"T00:00:00Z",Qt::ISODate);
                QJsonObject album{{"id",albumId},{"name",release.value("title")},{"artistId",artistId},{"publishTime",double(published.toMSecsSinceEpoch())},{"symbolicId",release.value("source_id")},{"availability",release.value("availability")}};
                QJsonArray songs{song(albumId*100+1,release.value("title").toString(),source.value("name").toString(),release.value("title").toString())};album.insert("songs",songs);fixedAlbums.insert(albumId,album);albums.append(album);
            }
            fixedReleases.insert(artistId,albums);
        }
    }
    QJsonObject description() const {
        QJsonArray sources;for(const auto &pool:pools)sources.append(QJsonObject{{"id",pool.id},{"name",pool.name},{"tags",QJsonArray::fromStringList(pool.tags)},{"trackIds",ids(pool.songs)}});
        return {{"version","fixed-catalog-v1"},{"anchorUtc",anchor.toString(Qt::ISODateWithMs)},{"sources",sources},{"artists",artists},
            {"scope","合成曲库元数据；不是真实召回、版权权益、音频或听感评测"}};
    }
    QJsonObject truth() const {
        auto out=description();QJsonArray rows;auto keys=tracks.keys();std::sort(keys.begin(),keys.end());
        for(int id:keys)rows.append(tracks.value(id));
        QJsonArray releases;
        const QList<int> ages{30,120,450};
        for(const auto &value:artists){const auto a=value.toObject();const int artistId=a.value("id").toInt();
            if(fixedReleases.contains(artistId)){for(const auto &row:fixedReleases.value(artistId))releases.append(row);continue;}
            for(int index=0;index<ages.size();++index){const int id=artistId*10+index+1;const QString name=QStringLiteral("%1 · 发行%2").arg(a.value("name").toString()).arg(index+1);QJsonArray songs;
                for(int i=1;i<=6;++i)songs.append(song(id*100+i,QStringLiteral("%1 新录音 %2").arg(a.value("name").toString()).arg(i),a.value("name").toString(),name));
                releases.append(QJsonObject{{"id",id},{"name",name},{"artistId",artistId},{"publishTime",double(anchor.addDays(-ages.at(index)).toMSecsSinceEpoch())},{"songs",songs}});
            }
        }
        QJsonArray rules;for(const auto &p:pools)rules.append(QJsonObject{{"poolId",p.id},{"keywords",QJsonArray::fromStringList(p.terms)}});
        out.insert("tracks",rows);out.insert("releases",releases);out.insert("playlistQueryRules",rules);
        out.insert("playlistSearchRule","规范化query包含静态term则+1；按分数降序、相同按poolId排序，至多5源；无匹配返回空。实际query→response记录见observations.catalogRequests");
        out.insert("ambiguousArtists",QJsonArray{artist(2001,"李明",{}),artist(2002,"李明",{})});
        QJsonArray scenePools;
        const QList<QPair<QString,QList<int>>> curated{{"安静阅读",{102,107}},{"晚饭后散步",{107,108,109}},{"地铁通勤",{105,108}},{"通勤",{105,108}},{"临时现场摇滚",{110}},{"跑步",{101}},{"微醺",{103,106}},{"睡眠",{104}}};
        for(const auto &scene:curated){QJsonArray poolIds;for(int id:scene.second)poolIds.append(id);scenePools.append(QJsonObject{{"scene",scene.first},{"poolIds",poolIds}});}
        out.insert("scenePools",scenePools);out.insert("scenePoolScope","固定合成元数据的人工场景标签，不代表真实听感真值");
        out.insert("audioAvailable",true);out.insert("audioScope","自有静音WAV，只验证播放器接线，不验证真实歌曲、时长或版权权益");return out;
    }
private:
    static QString norm(QString text) { return text.normalized(QString::NormalizationForm_KC).toCaseFolded().remove(' ').remove('.'); }
    static QJsonObject artist(int id,const QString &name,const QStringList &aliases) { return {{"id",id},{"name",name},{"aliases",QJsonArray::fromStringList(aliases)}}; }
    static QJsonObject song(int id,const QString &title,const QString &performer,const QString &album) {
        return {{"id",id},{"title",title},{"artist",performer},{"album",album},{"coverUrl",""},{"durationMs",240000},{"fee",0}};
    }
    static QJsonArray ids(const QJsonArray &rows) { QJsonArray out;for(const auto &row:rows)out.append(row.toObject().value("id"));return out; }
    static QByteArray audio() {
        constexpr quint32 rate=22050,size=rate*60*2;QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);QDataStream stream(&buffer);stream.setByteOrder(QDataStream::LittleEndian);
        stream.writeRawData("RIFF",4);stream<<quint32(36+size);stream.writeRawData("WAVEfmt ",8);stream<<quint32(16)<<quint16(1)<<quint16(1)<<rate<<quint32(rate*2)<<quint16(2)<<quint16(16);
        stream.writeRawData("data",4);stream<<size;bytes.append(QByteArray(size,'\0'));return bytes;
    }
    void addPool(int id,const QString &name,const QStringList &tags,const QStringList &terms,const QString &performer) {
        Pool pool{id,name,tags,terms,{}};
        for(int index=1;index<=14;++index) {
            auto row=song(id*100+index,QStringLiteral("%1 · %2%3").arg(name).arg(index).arg(index>12?" Live":""),performer,name);
            pool.songs.append(row);tracks.insert(id*100+index,row);
        }
        pools.append(pool);
    }
    QJsonObject artistFor(int id) const { for(const auto &row:artists)if(row.toObject().value("id").toInt()==id)return row.toObject();return {}; }
    QJsonObject respond(const QUrl &url,int &status,QJsonArray &returned) const {
        const QString path=url.path();const QUrlQuery query(url);const QString keyword=norm(query.queryItemValue("keywords"));
        if(path=="/health")return {{"app","quiet-netease"},{"version",1},{"catalogVersion",2},{"profileIsolation",1}};
        if(path=="/account/status")return {{"loggedIn",false},{"nickname",""},{"avatarUrl",""},{"vipType",0}};
        if(path=="/radio/search") {
            QList<QPair<int,int>> ranked;
            for(int index=0;index<pools.size();++index){int score=0;for(const auto &term:pools.at(index).terms)if(keyword.contains(norm(term)))++score;if(score)ranked.append({score,index});}
            std::stable_sort(ranked.begin(),ranked.end(),[](auto a,auto b){return a.first>b.first;});
            QJsonArray rows;for(int index=0;index<ranked.size()&&index<5;++index){const auto &p=pools.at(ranked.at(index).second);rows.append(QJsonObject{{"id",p.id},{"name",p.name},{"tags",QJsonArray::fromStringList(p.tags)}});}
            returned=ids(rows);return {{"playlists",rows},{"total",rows.size()}};
        }
        if(path=="/radio/playlist") {
            for(const auto &p:pools)if(p.id==query.queryItemValue("id").toInt()){returned=ids(p.songs);return {{"playlist",QJsonObject{{"id",p.id},{"name",p.name},{"tags",QJsonArray::fromStringList(p.tags)}}},{"songs",p.songs}};}
        }
        if(path=="/search") {
            QJsonArray rows;QList<int> keys=tracks.keys();std::sort(keys.begin(),keys.end());
            for(int id:keys){auto row=tracks.value(id);if(id==70001&&!untrustedToolDescription.isEmpty())row.insert("description",untrustedToolDescription);
                const auto artistName=norm(row.value("artist").toString());const auto title=norm(row.value("title").toString());
                bool matches=keyword.contains(title)||keyword==artistName;
                for(const auto &a:artists){const auto ar=a.toObject();if(ar.value("name")==row.value("artist")){for(const auto &alias:ar.value("aliases").toArray())matches|=keyword==norm(alias.toString())||(keyword.contains(norm(alias.toString()))&&keyword.contains(title));}}
                if(matches&&rows.size()<40)rows.append(row);
            }
            returned=ids(rows);return {{"songs",rows},{"total",rows.size()}};
        }
        if(path=="/assistant/artists") {
            QJsonArray rows;for(const auto &value:artists){const auto a=value.toObject();bool match=norm(a.value("name").toString()).contains(keyword)||keyword.contains(norm(a.value("name").toString()));
                for(const auto &alias:a.value("aliases").toArray())match|=norm(alias.toString())==keyword;if(match)rows.append(a);}
            if(keyword==norm("李明"))rows={artist(2001,"李明",{}),artist(2002,"李明",{})};returned=ids(rows);return {{"artists",rows}};
        }
        if(path=="/assistant/releases") {
            const int artistId=query.queryItemValue("artistId").toInt();const auto a=artistFor(artistId);QJsonArray albums;
            if(fixedReleases.contains(artistId))albums=fixedReleases.value(artistId);
            else if(!a.isEmpty()){const QList<int> ages{30,120,450};for(int i=0;i<ages.size();++i)albums.append(QJsonObject{{"id",artistId*10+i+1},{"name",QStringLiteral("%1 · 发行%2").arg(a.value("name").toString()).arg(i+1)},{"publishTime",double(anchor.addDays(-ages.at(i)).toMSecsSinceEpoch())}});}
            returned=ids(albums);return {{"artist",a},{"checkedAt",double(QDateTime::currentMSecsSinceEpoch())},{"albums",albums}};
        }
        if(path=="/assistant/album") {
            const int id=query.queryItemValue("id").toInt(),artistId=id/10,index=id%10-1;const auto a=artistFor(artistId);const QList<int> ages{30,120,450};
            if(fixedAlbums.contains(id)){auto album=fixedAlbums.value(id);const auto songs=album.take("songs").toArray();returned=ids(songs);return {{"album",album},{"songs",songs}};}
            if(!a.isEmpty()&&index>=0&&index<3){const auto name=QStringLiteral("%1 · 发行%2").arg(a.value("name").toString()).arg(index+1);QJsonArray rows;
                for(int i=1;i<=6;++i)rows.append(song(id*100+i,QStringLiteral("%1 新录音 %2").arg(a.value("name").toString()).arg(i),a.value("name").toString(),name));
                returned=ids(rows);return {{"album",QJsonObject{{"id",id},{"name",name},{"artistId",artistId},{"publishTime",double(anchor.addDays(-ages.at(index)).toMSecsSinceEpoch())}}},{"songs",rows}};}
        }
        // 自有静音音频只验证播放接线，不能作歌曲内容或权益成功证据。
        if(path=="/song/url")return {{"id",query.queryItemValue("id").toInt()},{"url",QString::fromLatin1(base()+"/audio.wav?id="+query.queryItemValue("id").toLatin1())},{"trial",false},{"durationMs",60000},{"level","standard"}};
        if(path=="/lyrics")return {{"lyric",""},{"yrc",""}};
        status=404;return {{"error",QJsonObject{{"message","固定曲库没有该资源"}}}};
    }
};
#endif
