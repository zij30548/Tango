#ifndef RADIOCLIENT_H
#define RADIOCLIENT_H
#include "radiotypes.h"
#include "assistanttypes.h"
#include <QObject>
#include <QPointer>
#include <QSet>
#include <functional>
#include <memory>
class NeteaseClient;
struct RadioResult {
    QString title, note;
    QVector<RadioRecommendation> tracks;
    QString kind = QStringLiteral("radio"), preference;
    QVector<AssistantArtist> artists;
};
class RadioClient : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(const RadioResult &, const QString &)>;
    explicit RadioClient(QObject *parent = nullptr);
    ~RadioClient() override;
    void generate(const QString &instruction, const QStringList &history,
                  const QString &preference, const QVector<qint64> &excludedIds,
                  QObject *context, Callback callback);
    void assist(const QString &instruction, const QStringList &history,
                const QString &preference, const QVector<qint64> &excludedIds,
                QObject *context, Callback callback);
    void releasesForArtist(const AssistantArtist &artist, QObject *context, Callback callback);
    void cancel();
    void reset();
signals:
    void stageChanged(QString stage);
private:
    struct Work;
    std::shared_ptr<Work> start(QObject *context, Callback callback);
    void generateImpl(const QString &, const QStringList &, const QString &,
                      const QVector<qint64> &, QObject *, Callback, bool assistant);
    void findArtists(const std::shared_ptr<Work> &, const QString &query);
    void fetchReleases(const std::shared_ptr<Work> &, const AssistantArtist &);
    void fetchAlbum(const std::shared_ptr<Work> &);
    void searchNext(const std::shared_ptr<Work> &work);
    void loadNext(const std::shared_ptr<Work> &work);
    void select(const std::shared_ptr<Work> &work);
    void complete(const std::shared_ptr<Work> &work, const RadioResult &, const QString &);
    bool current(const std::shared_ptr<Work> &) const;
    NeteaseClient *music;
    QPointer<QObject> operation;
    quint64 revision = 0;
    QList<qint64> usedPlaylists;
};
#endif
