#ifndef RADIOTYPES_H
#define RADIOTYPES_H

#include "neteasetypes.h"
#include <QStringList>

struct RadioRecommendation
{
    OnlineSong song;
    QString reason;
    QString sourceName;
    QStringList sourceTags;
};
Q_DECLARE_METATYPE(RadioRecommendation)
Q_DECLARE_METATYPE(QVector<RadioRecommendation>)

#endif
