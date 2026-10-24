#ifndef NETEASETYPES_H
#define NETEASETYPES_H

#include <QMetaType>
#include <QString>
#include <QVector>

// Metadata only. A catalog fee flag is not a playback entitlement.
struct OnlineSong
{
    qint64 id = 0;
    QString title;
    QString artist;
    int durationMs = 0;
    int fee = 0;
    QString album;
    QString coverUrl;
    // 仅保存上游明确给出的规范版本；缺失时为空，不能据此推断录音室版本。
    QString version;
};

Q_DECLARE_METATYPE(OnlineSong)
Q_DECLARE_METATYPE(QVector<OnlineSong>)

#endif // NETEASETYPES_H
