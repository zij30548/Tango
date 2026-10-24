#ifndef COVERARTCACHE_H
#define COVERARTCACHE_H

#include <QCache>
#include <QImage>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QUrl>

class NeteaseClient;
class QNetworkAccessManager;

// One shared, bounded cache. Looking up artwork never waits for file or network IO.
class CoverArtCache : public QObject
{
    Q_OBJECT
  public:
    explicit CoverArtCache(QObject *parent = nullptr);
    // Network injection makes the same production validation testable without real accounts/CDN IO.
    CoverArtCache(QNetworkAccessManager *imageNetwork, const QString &cacheDirectory,
                  QObject *parent = nullptr);
    static CoverArtCache *instance();
    QImage image(const QUrl &mediaUrl, qint64 songId = 0,
                 const QString &coverUrl = QString());
    static QUrl normalizedCoverUrl(const QString &url);
    static qint64 neteaseSongId(const QUrl &url);
    int activeRequestCount() const;
    int queuedRequestCount() const;
    qint64 memoryCostBytes() const;

  signals:
    void imageReady();

  private:
    enum class JobType { Metadata, Download, Local };
    struct Job
    {
        JobType type;
        QString key;
        QUrl url;
        qint64 songId = 0;
    };
    void initialize(const QString &cacheDirectory);
    bool isCoolingDown(const QString &key);
    void enqueue(const Job &job);
    void pump();
    void start(const Job &job);
    void finish(const Job &job, const QImage &image = QImage(), bool failed = false);
    void remember(const QString &key, const QImage &image);

    QNetworkAccessManager *network;
    NeteaseClient *metadata;
    QCache<QString, QImage> images{12 * 1024}; // Cost in KiB, including decoded pixels.
    QCache<QString, QString> resolved{1024};
    QCache<QString, qint64> failedUntil{512};
    QSet<QString> pending;
    QQueue<Job> queue;
    int active = 0;
};
#endif
