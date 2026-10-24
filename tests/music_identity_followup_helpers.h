#ifndef TANGO_MUSIC_IDENTITY_FOLLOWUP_HELPERS_H
#define TANGO_MUSIC_IDENTITY_FOLLOWUP_HELPERS_H

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <cmath>

namespace IdentityFollowupProbe {
inline QString canonicalName(QString value) {
    // 只接受同一个全名的排版差异；不依据排名、子串或别名替换歌手身份。
    return value.normalized(QString::NormalizationForm_KC).toCaseFolded()
        .remove(QRegularExpression(QStringLiteral("[\\s.]+")));
}
inline int verifiedArtistOrdinal(const QJsonArray &candidates) {
    if(candidates.isEmpty()||candidates.size()>5)return 0;
    int ordinal=0;
    for(int index=0;index<candidates.size();++index){const auto row=candidates.at(index).toObject();const auto id=row.value("id");
        if(id.isDouble()&&id.toDouble()==7763&&std::floor(id.toDouble())==id.toDouble()&&
            canonicalName(row.value("name").toString())==canonicalName(QStringLiteral("G.E.M.邓紫棋"))){
            if(ordinal)return 0;ordinal=index+1;
        }
    }
    return ordinal;
}
inline bool stepPreconditions(const QJsonObject &step,const QJsonObject &before) {
    const auto task=before.value("task").toObject();const auto tracks=task.value("preview").toObject().value("tracks").toArray();
    if(step.value("requirePreview").toBool()&&task.value("status")!="preview")return false;
    if(tracks.size()<step.value("minimumTracks").toInt())return false;
    if(task.value("lockedIds").toArray().size()<step.value("requireLockedTracks").toInt())return false;
    return true;
}
}
#endif
