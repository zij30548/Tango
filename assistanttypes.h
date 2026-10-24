#ifndef ASSISTANTTYPES_H
#define ASSISTANTTYPES_H

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

// Search metadata only; selecting an artist never grants playback rights.
struct AssistantArtist
{
    qint64 id = 0;
    QString name;
    QStringList aliases;
    QString coverUrl;
};

Q_DECLARE_METATYPE(AssistantArtist)
Q_DECLARE_METATYPE(QVector<AssistantArtist>)

#endif
