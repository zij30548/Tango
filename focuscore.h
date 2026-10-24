#ifndef FOCUSCORE_H
#define FOCUSCORE_H

#include <QSqlDatabase>
#include <QMetaType>
#include <QStringList>
#include <QUrl>
#include <QVector>

struct FocusRequest
{
    int minutes = 25;
    QString scene = QStringLiteral("work"); // work / study / rest
    QString source = QStringLiteral("queue"); // queue / favorites
    bool excludeLive = false;
    bool instrumentalOnly = false;
    QStringList excludedArtists;
    QString originalText;
};

struct FocusTrack
{
    QString key;
    QString title;
    QString artist;
    QUrl url;
    qint64 songId = 0;
    qint64 durationMs = 0;
    bool favorite = false;
    int instrumental = -1; // -1 unknown, 0 no, 1 manually confirmed
    int live = -1; // -1 unknown, 0 no, 1 manually confirmed
};

struct FocusRule
{
    qint64 id = 0;
    QString scene;
    QString kind; // exclude_live / instrumental_only / exclude_artist / exclude_track
    QString value;
};

struct FocusPlan
{
    FocusRequest request;
    QVector<FocusTrack> tracks;
    QStringList notes;
    QString error;
};

class FocusPlanner
{
public:
    // Non-empty error means the instruction was not understood; do not run it.
    static FocusRequest parse(const QString &text, const FocusRequest &base = FocusRequest(),
                              QString *error = nullptr);
    static bool parse(const QString &text, const FocusRequest &base, FocusRequest *request,
                      QString *error);
    static FocusPlan build(const FocusRequest &request, const QVector<FocusTrack> &candidates,
                           const QVector<FocusRule> &rules = {});
};

class FocusStore
{
public:
    explicit FocusStore(const QSqlDatabase &database);
    bool initialize(QString *error = nullptr);
    QVector<FocusRule> rules(const QString &profile, const QString &scene = QString(),
                             QString *error = nullptr) const;
    qint64 addRule(const QString &profile, const QString &scene, const QString &kind,
                   const QString &value = QString(), QString *error = nullptr);
    bool removeRule(const QString &profile, qint64 id, QString *error = nullptr);
    bool setTrackLabels(const QString &profile, const QString &trackKey, int instrumental,
                        int live, qint64 durationMs = 0, QString *error = nullptr);
    QVector<FocusTrack> applyLabels(const QString &profile, const QVector<FocusTrack> &tracks,
                                    QString *error = nullptr) const;

private:
    QSqlDatabase database;
};

Q_DECLARE_METATYPE(FocusRequest)
Q_DECLARE_METATYPE(FocusTrack)
Q_DECLARE_METATYPE(FocusPlan)

#endif
