#include "coverartcache.h"
#include "songlistdelegate.h"
#include <QtTest>
#include <QBuffer>
#include <QFile>
#include <QHeaderView>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <cstring>

namespace
{
QByteArray png(const QSize &size = QSize(64, 64))
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(QColor("#6da88c"));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

class FakeReply : public QNetworkReply
{
  public:
    FakeReply(const QNetworkRequest &request, QByteArray body, int status, QObject *parent)
        : QNetworkReply(parent), body(std::move(body))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setHeader(QNetworkRequest::ContentTypeHeader, "image/png");
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    void complete()
    {
        if (done)
            return;
        available = true;
        emit downloadProgress(body.size(), body.size());
        if (done)
            return;
        emit readyRead();
        if (done)
            return;
        done = true;
        setFinished(true);
        emit finished();
    }
    void abort() override
    {
        if (done)
            return;
        done = true;
        setError(QNetworkReply::OperationCanceledError, "cancelled");
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override
    {
        return (available ? body.size() - position : 0) + QNetworkReply::bytesAvailable();
    }
    bool isSequential() const override { return true; }
    bool done = false;

  protected:
    qint64 readData(char *target, qint64 maximum) override
    {
        if (!available || position >= body.size())
            return -1;
        const qint64 count = qMin(maximum, qint64(body.size()) - position);
        std::memcpy(target, body.constData() + position, size_t(count));
        position += count;
        return count;
    }

  private:
    QByteArray body;
    qint64 position = 0;
    bool available = false;
};

class FakeNetwork : public QNetworkAccessManager
{
  public:
    bool autoComplete = true;
    int status = 200;
    QByteArray response = png();
    QList<QNetworkRequest> requests;
    QList<QPointer<FakeReply>> replies;
    int live = 0;
    int maximumLive = 0;
    void completeRunning()
    {
        const auto snapshot = replies;
        for (const auto &reply : snapshot)
            if (reply && !reply->done)
                reply->complete();
    }

  protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        requests.append(request);
        auto *reply = new FakeReply(request, response, status, this);
        replies.append(reply);
        ++live;
        maximumLive = qMax(maximumLive, live);
        connect(reply, &QNetworkReply::finished, this, [this]() { --live; });
        if (autoComplete)
            QTimer::singleShot(0, reply, [reply]() { reply->complete(); });
        return reply;
    }
};

QString imageUrl(int index)
{
    return QString("https://p1.music.126.net/test/cover-%1.jpg").arg(index);
}
} // namespace

class CoverArtTest : public QObject
{
    Q_OBJECT
  private slots:
    void acceptsOnlyConcreteNetEaseImageHosts();
    void canonicalSongIdsRejectAmbiguousOrForeignUrls();
    void deduplicatesAndCachesWithoutSendingCredentials();
    void limitsConcurrencyAndPendingQueue();
    void rejectsOversizedBodiesDimensionsAndRedirects();
    void boundsDecodedMemoryAndDiskCache();
    void loadsLocalSidecarsOffThreadAndDownsamples();
    void delegatePaintsOnlyVisibleRows();
};

void CoverArtTest::acceptsOnlyConcreteNetEaseImageHosts()
{
    const auto safe = CoverArtCache::normalizedCoverUrl("http://p12.music.126.net/cover.jpg?x=1&param=1y1");
    QCOMPARE(safe.scheme(), QString("https"));
    QCOMPARE(safe.host(), QString("p12.music.126.net"));
    QCOMPARE(QUrlQuery(safe).queryItemValue("param"), QString("512y512"));
    QCOMPARE(QUrlQuery(safe).queryItemValue("x"), QString("1"));
    const QStringList unsafe{"https://music.126.net/a.jpg", "https://p1.music.126.net.evil.test/a.jpg",
                             "https://a.p1.music.126.net/a.jpg", "file:///tmp/a.png",
                             "https://127.0.0.1/a.jpg", "http://p1.music.126.net:8080/a.jpg",
                             "https://user:secret@p1.music.126.net/a.jpg", "ftp://p1.music.126.net/a.jpg",
                             "https://p1.music.126.net", "https://music.163.com/a.jpg"};
    for (const auto &url : unsafe)
        QVERIFY2(CoverArtCache::normalizedCoverUrl(url).isEmpty(), qPrintable(url));
    FakeNetwork network;
    CoverArtCache cache(&network, {});
    for (const auto &url : unsafe)
        QVERIFY(cache.image({}, 0, url).isNull());
    QCOMPARE(network.requests.size(), 0);
}

void CoverArtTest::canonicalSongIdsRejectAmbiguousOrForeignUrls()
{
    QCOMPARE(CoverArtCache::neteaseSongId(QUrl("https://music.163.com/song?id=66842")), qint64(66842));
    QCOMPARE(CoverArtCache::neteaseSongId(QUrl("http://music.163.com/song?id=123456789012")), qint64(123456789012));
    const QStringList unsafe{"https://music.163.com/song?id=-1", "https://music.163.com/song?id=1&id=2",
                             "https://evil.test/song?id=1", "https://music.163.com/playlist?id=1",
                             "https://music.163.com/song?id=0", "https://music.163.com/song?id=1e3",
                             "https://user@music.163.com/song?id=12", "https://music.163.com:443/song?id=12"};
    for (const auto &url : unsafe)
        QCOMPARE(CoverArtCache::neteaseSongId(QUrl(url)), qint64(0));
}

void CoverArtTest::deduplicatesAndCachesWithoutSendingCredentials()
{
    FakeNetwork network;
    network.autoComplete = false;
    CoverArtCache cache(&network, {});
    QSignalSpy ready(&cache, &CoverArtCache::imageReady);
    for (int repeat = 0; repeat < 500; ++repeat)
        QVERIFY(cache.image({}, 0, imageUrl(1)).isNull());
    QCOMPARE(network.requests.size(), 1);
    QCOMPARE(cache.activeRequestCount(), 1);
    const auto request = network.requests.first();
    QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
             int(QNetworkRequest::ManualRedirectPolicy));
    QCOMPARE(request.attribute(QNetworkRequest::CacheLoadControlAttribute).toInt(),
             int(QNetworkRequest::PreferCache));
    QCOMPARE(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(),
             int(QNetworkRequest::Manual));
    QCOMPARE(request.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(),
             int(QNetworkRequest::Manual));
    QVERIFY(!request.hasRawHeader("Authorization"));
    QVERIFY(!request.hasRawHeader("Cookie"));
    QVERIFY(!request.hasRawHeader("X-Local-Token"));
    network.completeRunning();
    QTRY_COMPARE(ready.count(), 1);
    const QImage result = cache.image({}, 0, imageUrl(1));
    QCOMPARE(result.size(), QSize(64, 64));
    QCOMPARE(network.requests.size(), 1);
    QCOMPARE(cache.activeRequestCount(), 0);
}

void CoverArtTest::limitsConcurrencyAndPendingQueue()
{
    FakeNetwork network;
    network.autoComplete = false;
    CoverArtCache cache(&network, {});
    for (int index = 0; index < 500; ++index)
        cache.image({}, 0, imageUrl(index));
    QCOMPARE(cache.activeRequestCount(), 4);
    QCOMPARE(cache.queuedRequestCount(), 64);
    QCOMPARE(network.requests.size(), 4);
    QElapsedTimer deadline;
    deadline.start();
    while ((cache.activeRequestCount() || cache.queuedRequestCount()) && deadline.elapsed() < 10000)
    {
        network.completeRunning();
        QTest::qWait(10);
    }
    QCOMPARE(cache.activeRequestCount(), 0);
    QCOMPARE(cache.queuedRequestCount(), 0);
    QCOMPARE(network.requests.size(), 68);
    QVERIFY(network.maximumLive <= 4);
}

void CoverArtTest::rejectsOversizedBodiesDimensionsAndRedirects()
{
    FakeNetwork network;
    CoverArtCache cache(&network, {});
    QSignalSpy ready(&cache, &CoverArtCache::imageReady);
    network.response = QByteArray(2 * 1024 * 1024 + 1, 'x');
    cache.image({}, 0, imageUrl(1));
    QTRY_COMPARE(ready.count(), 1);
    QVERIFY(cache.image({}, 0, imageUrl(1)).isNull());
    QCOMPARE(network.requests.size(), 1); // Failure cooldown prevents repaint/retry storms.
    network.response = png(QSize(8193, 1));
    cache.image({}, 0, imageUrl(2));
    QTRY_COMPARE(ready.count(), 2);
    QVERIFY(cache.image({}, 0, imageUrl(2)).isNull());
    network.status = 302;
    network.response = png();
    cache.image({}, 0, imageUrl(3));
    QTRY_COMPARE(ready.count(), 3);
    QVERIFY(cache.image({}, 0, imageUrl(3)).isNull());
    QCOMPARE(network.requests.size(), 3); // No redirect target is followed.
    network.status = 200;
    network.response = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\"/>";
    cache.image({}, 0, imageUrl(4));
    QTRY_COMPARE(ready.count(), 4);
    QVERIFY(cache.image({}, 0, imageUrl(4)).isNull());
}

void CoverArtTest::boundsDecodedMemoryAndDiskCache()
{
    FakeNetwork network;
    network.response = png(QSize(512, 512));
    QTemporaryDir directory;
    CoverArtCache cache(&network, directory.path());
    const auto *disk = qobject_cast<QNetworkDiskCache *>(network.cache());
    QVERIFY(disk);
    QCOMPARE(disk->maximumCacheSize(), qint64(64 * 1024 * 1024));
    QSignalSpy ready(&cache, &CoverArtCache::imageReady);
    for (int index = 0; index < 24; ++index)
    {
        cache.image({}, 0, imageUrl(index));
        QTRY_COMPARE(ready.count(), index + 1);
        QVERIFY(cache.memoryCostBytes() <= 12 * 1024 * 1024);
    }
    QCOMPARE(cache.memoryCostBytes(), qint64(12 * 1024 * 1024));
    const int previous = network.requests.size();
    QVERIFY(cache.image({}, 0, imageUrl(0)).isNull()); // Oldest decoded cover was evicted.
    QCOMPARE(network.requests.size(), previous + 1);
}

void CoverArtTest::loadsLocalSidecarsOffThreadAndDownsamples()
{
    QTemporaryDir directory;
    QFile audio(directory.filePath("quiet.wav"));
    QVERIFY(audio.open(QIODevice::WriteOnly));
    audio.close();
    QImage source(QSize(1600, 800), QImage::Format_RGB32);
    source.fill(QColor("#6da88c"));
    QVERIFY(source.save(directory.filePath("quiet.jpg")));
    FakeNetwork network;
    CoverArtCache cache(&network, {});
    QSignalSpy ready(&cache, &CoverArtCache::imageReady);
    const QUrl media = QUrl::fromLocalFile(audio.fileName());
    QVERIFY(cache.image(media).isNull()); // IO and decoding have not blocked the caller.
    QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(cache.image(media).size(), QSize(512, 256));
    QCOMPARE(network.requests.size(), 0);
    QFile other(directory.filePath("other.wav"));
    QVERIFY(other.open(QIODevice::WriteOnly));
    other.close();
    QVERIFY(source.save(directory.filePath("cover.png")));
    const QUrl otherMedia = QUrl::fromLocalFile(other.fileName());
    cache.image(otherMedia);
    QTRY_COMPARE(ready.count(), 2);
    QCOMPARE(cache.image(otherMedia).size(), QSize(512, 256));
}

void CoverArtTest::delegatePaintsOnlyVisibleRows()
{
    FakeNetwork network;
    network.autoComplete = false;
    CoverArtCache cache(&network, {});
    QStandardItemModel model(500, 3);
    for (int row = 0; row < model.rowCount(); ++row)
    {
        const auto index = model.index(row, 0);
        model.setData(index, QString("测试曲目 %1").arg(row));
        model.setData(index, "歌手 · 专辑", SongListDelegate::ArtistRole);
        model.setData(index, imageUrl(row), SongListDelegate::CoverUrlRole);
    }
    QTableView view;
    view.resize(600, 320);
    view.setModel(&model);
    view.setStyleSheet(QStringLiteral(
        "QTableView { border: 2px solid #738c7c; selection-background-color: #abff00; }"
        "QTableView::item:selected { background: #ff0000; }"));
    SongListDelegate::install(&view, 0, &cache);
    view.show();
    QTest::qWait(20);
    QCOMPARE(view.rowHeight(0), 72);
    QCOMPARE(view.horizontalHeader()->defaultAlignment(), Qt::AlignLeft | Qt::AlignVCenter);
    QVERIFY(!view.showGrid());
    const QPixmap rendered = view.grab();
    QVERIFY(!rendered.isNull());
    QVERIFY(view.rowAt(view.viewport()->height() - 1) < 10);
    QVERIFY(network.requests.size() > 0);
    QVERIFY(cache.activeRequestCount() + cache.queuedRequestCount() < 10);
    const QImage beforeSelection = view.viewport()->grab().toImage();
    view.selectRow(1);
    const QImage afterSelection = view.viewport()->grab().toImage();
    const int selectedY = view.visualRect(model.index(1, 0)).center().y();
    // The inherited full-width highlight is gone; the inset delegate highlight remains.
    QCOMPARE(afterSelection.pixelColor(2, selectedY), beforeSelection.pixelColor(2, selectedY));
    QVERIFY(afterSelection.pixelColor(8, selectedY) != beforeSelection.pixelColor(8, selectedY));
    view.scrollTo(model.index(499, 0));
    QVERIFY(!view.grab().isNull());
    // Only the two visible ranges request images; 500 model rows are never prefetched.
    QVERIFY(cache.activeRequestCount() + cache.queuedRequestCount() < 20);
}

QTEST_MAIN(CoverArtTest)
#include "coverart_test.moc"
