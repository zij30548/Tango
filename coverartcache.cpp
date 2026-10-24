#include "coverartcache.h"
#include "neteaseclient.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrentRun>

namespace
{
constexpr qint64 DownloadLimit = 2 * 1024 * 1024;
constexpr qint64 LocalFileLimit = 8 * 1024 * 1024;
constexpr qint64 OriginalPixelLimit = 16 * 1024 * 1024;
constexpr int MaximumConcurrent = 4;
constexpr int MaximumQueued = 64;

QImage readImage(QImageReader &reader)
{
    reader.setDecideFormatFromContent(true);
    const QByteArray format = reader.format().toLower();
    if (format != "jpeg" && format != "jpg" && format != "png" && format != "webp")
        return {};
    const QSize original = reader.size();
    if (original.isEmpty() || original.width() > 8192 || original.height() > 8192 ||
        qint64(original.width()) * original.height() > OriginalPixelLimit)
        return {};
    reader.setAutoTransform(true);
    if (original.width() > 512 || original.height() > 512)
        reader.setScaledSize(original.scaled(512, 512, Qt::KeepAspectRatio));
    QImage result = reader.read();
    if (result.isNull())
        return {};
    if (result.width() > 512 || result.height() > 512)
        result = result.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // Normalize allocation cost and avoid retaining an unusually padded decoder buffer.
    return result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage decodeBytes(const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > DownloadLimit)
        return {};
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    return readImage(reader);
}

QImage localCover(const QUrl &mediaUrl)
{
    const QFileInfo media(mediaUrl.toLocalFile());
    const QDir directory = media.dir();
    const QString base = media.completeBaseName();
    const QStringList names{base + ".jpg", base + ".png", base + ".jpeg", "cover.jpg",
                            "cover.png", "folder.jpg"};
    for (const QString &name : names)
    {
        const QFileInfo file(directory.filePath(name));
        if (!file.isFile() || file.size() <= 0 || file.size() > LocalFileLimit)
            continue;
        QImageReader reader(file.absoluteFilePath());
        const QImage image = readImage(reader);
        if (!image.isNull())
            return image;
    }
    return {};
}
} // namespace

CoverArtCache::CoverArtCache(QObject *parent)
    : QObject(parent), network(new QNetworkAccessManager(this)), metadata(new NeteaseClient(this))
{
    network->setProxy(QNetworkProxy::NoProxy);
    initialize(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                   .filePath("artwork"));
}

CoverArtCache::CoverArtCache(QNetworkAccessManager *imageNetwork, const QString &cacheDirectory,
                           QObject *parent)
    : QObject(parent), network(imageNetwork ? imageNetwork : new QNetworkAccessManager(this)),
      metadata(new NeteaseClient(this))
{
    initialize(cacheDirectory);
}

void CoverArtCache::initialize(const QString &cacheDirectory)
{
    if (cacheDirectory.isEmpty())
        return;
    if (!QDir().mkpath(cacheDirectory))
        return;
    auto *disk = new QNetworkDiskCache(network);
    disk->setCacheDirectory(cacheDirectory);
    disk->setMaximumCacheSize(64 * 1024 * 1024);
    network->setCache(disk);
}

CoverArtCache *CoverArtCache::instance()
{
    static QPointer<CoverArtCache> cache;
    if (!cache)
        cache = new CoverArtCache(QCoreApplication::instance());
    return cache;
}

QUrl CoverArtCache::normalizedCoverUrl(const QString &source)
{
    if (source.isEmpty() || source.size() > 4096)
        return {};
    QUrl url(source, QUrl::StrictMode);
    static const QRegularExpression host(QStringLiteral("^p[0-9]+\\.music\\.126\\.net$"));
    if (!url.isValid() || !url.userInfo().isEmpty() || url.port(-1) != -1 ||
        !host.match(url.host().toLower()).hasMatch() ||
        (url.scheme() != "https" && url.scheme() != "http") || url.path().isEmpty())
        return {};
    url.setScheme("https");
    url.setHost(url.host().toLower());
    url.setFragment({});
    // NetEase's image endpoint supports a bounded rendition; preserve other query fields.
    QUrlQuery query(url);
    query.removeAllQueryItems("param");
    query.addQueryItem("param", "512y512");
    url.setQuery(query);
    return url;
}

qint64 CoverArtCache::neteaseSongId(const QUrl &url)
{
    if (!url.isValid() || !url.userInfo().isEmpty() || url.port(-1) != -1 ||
        (url.scheme() != "https" && url.scheme() != "http") ||
        url.host().compare("music.163.com", Qt::CaseInsensitive) != 0 || url.path() != "/song")
        return 0;
    const auto ids = QUrlQuery(url).allQueryItemValues("id");
    if (ids.size() != 1 || !QRegularExpression("^[1-9][0-9]{0,17}$").match(ids.first()).hasMatch())
        return 0;
    bool ok = false;
    const qint64 id = ids.first().toLongLong(&ok);
    return ok ? id : 0;
}

bool CoverArtCache::isCoolingDown(const QString &key)
{
    const auto until = failedUntil.object(key);
    if (!until)
        return false;
    if (*until > QDateTime::currentMSecsSinceEpoch())
        return true;
    failedUntil.remove(key);
    return false;
}

QImage CoverArtCache::image(const QUrl &mediaUrl, qint64 songId, const QString &coverUrl)
{
    const qint64 canonicalId = neteaseSongId(mediaUrl);
    if (canonicalId)
        songId = canonicalId;
    const QString songKey = songId > 0 ? QStringLiteral("song:%1").arg(songId) : QString();
    const QUrl cover = normalizedCoverUrl(coverUrl);
    QString assetKey;
    if (!cover.isEmpty())
    {
        assetKey = "image:" + QString::fromLatin1(cover.toEncoded());
        if (!songKey.isEmpty())
            resolved.insert(songKey, new QString(assetKey));
        if (const auto found = images.object(assetKey))
            return *found;
        enqueue({JobType::Download, assetKey, cover});
        return {};
    }
    if (mediaUrl.isLocalFile())
    {
        assetKey = "local:" + mediaUrl.toString(QUrl::FullyEncoded);
        if (const auto found = images.object(assetKey))
            return *found;
        enqueue({JobType::Local, assetKey, mediaUrl});
        return {};
    }
    if (songKey.isEmpty())
        return {};
    if (const auto knownAsset = resolved.object(songKey))
    {
        assetKey = *knownAsset;
        if (const auto found = images.object(assetKey))
            return *found;
        enqueue({JobType::Download, assetKey, QUrl(assetKey.mid(6))});
        return {};
    }
    enqueue({JobType::Metadata, songKey, {}, songId});
    return {};
}

void CoverArtCache::enqueue(const Job &job)
{
    if (pending.contains(job.key) || isCoolingDown(job.key) || queue.size() >= MaximumQueued)
        return;
    pending.insert(job.key);
    queue.enqueue(job);
    pump();
}

void CoverArtCache::pump()
{
    while (active < MaximumConcurrent && !queue.isEmpty())
    {
        const Job job = queue.dequeue();
        ++active;
        start(job);
    }
}

void CoverArtCache::start(const Job &job)
{
    if (job.type == JobType::Metadata)
    {
        QUrlQuery query;
        query.addQueryItem("id", QString::number(job.songId));
        metadata->get("/song/detail", query, this,
                      [this, job](const QJsonObject &result, const QString &error)
                      {
                          const auto song = result.value("song").toObject();
                          const qint64 returnedId = song.value("id").toVariant().toLongLong();
                          const QUrl cover = normalizedCoverUrl(song.value("coverUrl").toString());
                          if (!error.isEmpty() || returnedId != job.songId || cover.isEmpty())
                          {
                              finish(job, {}, true);
                              return;
                          }
                          const QString key = "image:" + QString::fromLatin1(cover.toEncoded());
                          resolved.insert(job.key, new QString(key));
                          enqueue({JobType::Download, key, cover});
                          finish(job);
                      });
        return;
    }
    if (job.type == JobType::Local)
    {
        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this,
                [this, watcher, job]()
                {
                    const auto result = watcher->result();
                    watcher->deleteLater();
                    finish(job, result, result.isNull());
                });
        watcher->setFuture(QtConcurrent::run([url = job.url]() { return localCover(url); }));
        return;
    }
    // Defense in depth: every download, including resolved metadata, passes the same allowlist.
    const QUrl safe = normalizedCoverUrl(job.url.toString());
    if (safe.isEmpty())
    {
        finish(job, {}, true);
        return;
    }
    QNetworkRequest request(safe);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setRawHeader("Accept", "image/jpeg,image/png,image/webp");
    auto *reply = network->get(request);
    reply->setReadBufferSize(DownloadLimit + 1);
    auto bytes = QSharedPointer<QByteArray>::create();
    connect(reply, &QNetworkReply::readyRead, this,
            [reply, bytes]()
            {
                bytes->append(reply->read(DownloadLimit + 1 - bytes->size()));
                if (bytes->size() > DownloadLimit)
                {
                    reply->setProperty("artworkTooLarge", true);
                    reply->abort();
                }
            });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [reply](qint64 received, qint64 total)
            {
                if (received > DownloadLimit || total > DownloadLimit)
                {
                    reply->setProperty("artworkTooLarge", true);
                    reply->abort();
                }
            });
    QTimer::singleShot(10000, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, bytes, job]()
            {
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300 &&
                                !reply->property("artworkTooLarge").toBool();
                if (ok)
                    bytes->append(reply->read(DownloadLimit + 1 - bytes->size()));
                reply->deleteLater();
                if (!ok || bytes->size() > DownloadLimit)
                {
                    finish(job, {}, true);
                    return;
                }
                // Decoding stays off the UI thread, just like sidecar IO.
                auto *watcher = new QFutureWatcher<QImage>(this);
                connect(watcher, &QFutureWatcher<QImage>::finished, this,
                        [this, watcher, job]()
                        {
                            const QImage result = watcher->result();
                            watcher->deleteLater();
                            finish(job, result, result.isNull());
                        });
                watcher->setFuture(QtConcurrent::run([bytes]() { return decodeBytes(*bytes); }));
            });
}

void CoverArtCache::remember(const QString &key, const QImage &image)
{
    if (image.isNull())
        return;
    const int cost = int((image.sizeInBytes() + 1023) / 1024);
    images.insert(key, new QImage(image), cost);
}

void CoverArtCache::finish(const Job &job, const QImage &image, bool failed)
{
    remember(job.key, image);
    if (failed)
        failedUntil.insert(job.key, new qint64(QDateTime::currentMSecsSinceEpoch() + 120000));
    pending.remove(job.key);
    --active;
    pump();
    emit imageReady();
}

int CoverArtCache::activeRequestCount() const { return active; }
int CoverArtCache::queuedRequestCount() const { return queue.size(); }
qint64 CoverArtCache::memoryCostBytes() const { return qint64(images.totalCost()) * 1024; }
