#include <QtTest>
#include <QAction>
#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QProgressBar>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTableView>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <memory>

#include "mainwindow.h"
#include "historysong.h"
#include "focussession.h"
#include "lyricswidget.h"
#include "neteasedialogs.h"
#include "neteasetypes.h"
#include "songlistdelegate.h"

namespace {

class ScopedEnvironment
{
public:
    ~ScopedEnvironment()
    {
        for (auto it = saved.crbegin(); it != saved.crend(); ++it) {
            if (it->existed)
                qputenv(it->name.constData(), it->value);
            else
                qunsetenv(it->name.constData());
        }
    }
    void set(const QByteArray &name, const QByteArray &value)
    {
        saved.append({name, qgetenv(name.constData()),
                      qEnvironmentVariableIsSet(name.constData())});
        qputenv(name.constData(), value);
    }
private:
    struct Value { QByteArray name, value; bool existed; };
    QVector<Value> saved;
};

QByteArray wave(int seconds)
{
    constexpr quint32 sampleRate = 22050;
    const quint32 dataSize = sampleRate * seconds * sizeof(qint16);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + dataSize);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << sampleRate
           << quint32(sampleRate * sizeof(qint16)) << quint16(sizeof(qint16)) << quint16(16);
    stream.writeRawData("data", 4);
    stream << dataSize;
    bytes.append(QByteArray(dataSize, '\0'));
    return bytes;
}

// This server never contacts NetEase. It serves both the normalized cloud API
// and quiet WAV data, including the range requests made by AVFoundation.
class FakeCloudService : public QObject
{
public:
    enum Access { Full, Trial, Denied };
    FakeCloudService()
    {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                socket->setParent(this);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    QByteArray bytes = socket->property("request").toByteArray();
                    bytes += socket->readAll();
                    socket->setProperty("request", bytes);
                    const int headerEnd = bytes.indexOf("\r\n\r\n");
                    if (headerEnd < 0 || socket->property("handled").toBool())
                        return;
                    const QByteArray header = bytes.left(headerEnd);
                    int length = 0;
                    for (const QByteArray &line : header.split('\n')) {
                        if (line.trimmed().toLower().startsWith("content-length:"))
                            length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    }
                    if (bytes.size() < headerEnd + 4 + length)
                        return;
                    socket->setProperty("handled", true);
                    handle(socket, header, bytes.mid(headerEnd + 4, length));
                });
            }
        });
    }

    bool listen() { return server.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return server.serverPort(); }
    QUrl streamUrl(qint64 id, int revision) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/audio.wav?id=%2&revision=%3")
                        .arg(port()).arg(id).arg(revision));
    }

    void releaseDelayed()
    {
        const auto waiting = delayed;
        delayed.clear();
        for (const auto &entry : waiting) {
            if (entry.socket && entry.socket->state() == QAbstractSocket::ConnectedState)
                sendJson(entry.socket, entry.payload);
        }
    }

    QHash<qint64, Access> access;
    QHash<qint64, int> resolutionCount;
    QHash<qint64, int> audioRequestCount;
    QSet<qint64> delaySongs;
    QByteArray token = QByteArray(64, 'a');
    QByteArray audio = wave(12);
    int unauthorizedRequests = 0;
    int logoutRequests = 0;
    int lyricsRequests = 0;
    bool delayLyrics = false;
    QString yrc;
    bool accountLoggedIn = true;
    int accountRequests = 0;
    int qrCheckRequests = 0;

private:
    void send(QTcpSocket *socket, const QByteArray &body,
              const QByteArray &contentType = "application/json", int code = 200,
              const QByteArray &extraHeaders = {}, qint64 declaredLength = -1)
    {
        const QByteArray status = code == 206 ? "206 Partial Content" :
                                  code == 401 ? "401 Unauthorized" : "200 OK";
        socket->write("HTTP/1.1 " + status + "\r\nContent-Type: " + contentType
                      + "\r\nContent-Length: "
                      + QByteArray::number(declaredLength < 0 ? body.size() : declaredLength)
                      + "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body);
        socket->disconnectFromHost();
    }
    void sendJson(QTcpSocket *socket, const QJsonObject &object, int code = 200)
    {
        send(socket, QJsonDocument(object).toJson(QJsonDocument::Compact),
             "application/json", code);
    }
    void handle(QTcpSocket *socket, const QByteArray &headers, const QByteArray &body)
    {
        const QList<QByteArray> request = headers.left(headers.indexOf("\r\n")).split(' ');
        if (request.size() < 2)
            return;
        const QUrl url(QStringLiteral("http://127.0.0.1") + QString::fromUtf8(request.at(1)));
        const QUrlQuery query(url);
        if (url.path() == "/audio.wav") {
            ++audioRequestCount[query.queryItemValue("id").toLongLong()];
            qint64 start = 0;
            qint64 end = audio.size() - 1;
            bool range = false;
            for (const QByteArray &line : headers.split('\n')) {
                const QByteArray clean = line.trimmed().toLower();
                if (!clean.startsWith("range: bytes="))
                    continue;
                const auto limits = clean.mid(13).split('-');
                if (!limits.isEmpty()) {
                    start = limits.at(0).toLongLong();
                    if (limits.size() > 1 && !limits.at(1).isEmpty())
                        end = qMin(end, limits.at(1).toLongLong());
                    range = true;
                }
            }
            const QByteArray rangeHeader = range
                ? "Content-Range: bytes " + QByteArray::number(start) + "-"
                    + QByteArray::number(end) + "/" + QByteArray::number(audio.size()) + "\r\n"
                : QByteArray();
            send(socket, request.first() == "HEAD" ? QByteArray() : audio.mid(start, end - start + 1),
                 "audio/wav", range ? 206 : 200, "Accept-Ranges: bytes\r\n" + rangeHeader,
                 end - start + 1);
            return;
        }
        if (url.path() == "/health") {
            sendJson(socket, {{"app", "quiet-netease"}, {"version", 1}, {"catalogVersion", 2}, {"profileIsolation", 1},
                              {"pid", double(QCoreApplication::applicationPid())}});
            return;
        }
        QByteArray receivedToken;
        for (const QByteArray &line : headers.split('\n')) {
            if (line.trimmed().toLower().startsWith("x-musicplayer-token:"))
                receivedToken = line.mid(line.indexOf(':') + 1).trimmed();
        }
        if (receivedToken != token) {
            ++unauthorizedRequests;
            sendJson(socket, {{"error", QJsonObject{{"code", "unauthorized"},
                {"message", "The fake test service rejected an invalid token"}}}}, 401);
            return;
        }
        if (url.path() == "/account/status") {
            ++accountRequests;
            sendJson(socket, {{"loggedIn", accountLoggedIn}, {"nickname", "Isolated test VIP"},
                              {"avatarUrl", ""}, {"vipType", 11}});
        } else if (url.path() == "/login/qr") {
            QImage image(16, 16, QImage::Format_RGB32);
            image.fill(Qt::white);
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            sendJson(socket, {{"key", "isolated-test-qr"},
                {"qrImage", QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64())}});
        } else if (url.path() == "/login/qr/check") {
            ++qrCheckRequests;
            sendJson(socket, {{"code", 803}, {"message", "Isolated test authorization confirmed"}});
        } else if (url.path() == "/logout") {
            Q_UNUSED(body);
            ++logoutRequests;
            sendJson(socket, {{"loggedIn", false}, {"nickname", ""},
                              {"avatarUrl", ""}, {"vipType", 0}});
        } else if (url.path() == "/lyrics") {
            ++lyricsRequests;
            const QJsonObject payload{{"lyric", "[00:00.000]Song beginning\n[00:04.000]Trial excerpt begins\n"}, {"yrc", yrc}};
            if (delayLyrics)
                delayed.append({socket, payload});
            else
                sendJson(socket, payload);
        } else if (url.path() == "/song/url") {
            const qint64 id = query.queryItemValue("id").toLongLong();
            const int revision = ++resolutionCount[id];
            const Access permission = access.value(id, Full);
            QJsonObject payload{{"id", double(id)},
                {"url", permission == Denied ? QString() : streamUrl(id, revision).toString()},
                {"trial", permission == Trial}, {"trialStartMs", permission == Trial ? 4000 : 0},
                {"trialEndMs", permission == Trial ? 30000 : 0},
                {"durationMs", 180000}, {"level", "standard"},
                {"message", permission == Denied ? "该歌曲暂时没有播放权限" : ""}};
            if (delaySongs.contains(id))
                delayed.append({socket, payload});
            else
                sendJson(socket, payload);
        } else {
            sendJson(socket, {{"error", QJsonObject{{"code", "invalid_request"},
                {"message", "Unexpected route in isolated integration test"}}}});
        }
    }

    struct Delayed { QPointer<QTcpSocket> socket; QJsonObject payload; };
    QTcpServer server;
    QVector<Delayed> delayed;
};

OnlineSong song(qint64 id, const QString &title = QStringLiteral("隔离测试歌曲"))
{
    OnlineSong value;
    value.id = id;
    value.title = title;
    value.artist = QStringLiteral("隔离测试歌手");
    value.durationMs = 180000;
    value.fee = 1;
    return value;
}

RadioRecommendation radioSong(qint64 id, const QString &title = QStringLiteral("电台隔离测试歌曲"))
{
    RadioRecommendation value;
    value.song = song(id, title);
    value.reason = QStringLiteral("来自测试歌单");
    value.sourceName = QStringLiteral("隔离测试歌单");
    value.sourceTags = QStringList{QStringLiteral("学习")};
    return value;
}

QString activateHistoryRow(MainWindow &window, int row)
{
    QString selectedSource;
    window.show();
    QMetaObject::invokeMethod(&window, "on_pushButton_HistorySong_clicked", Qt::DirectConnection);
    QTest::qWait(50);
    auto *page = window.findChild<HistorySong *>("historyPage");
    if (!page || page->isWindow() || !page->isVisible()) return {};
    const auto connection = QObject::connect(page, &HistorySong::songSelected, page,
        [&](const QString &source) { selectedSource = source; });
    auto *table = page->findChild<QTableWidget *>("table_HistorySong");
    if (table && table->item(row, 0)) {
        const QPoint position = table->visualItemRect(table->item(row, 0)).center();
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, position);
        QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, position);
    }
    QObject::disconnect(connection);
    return selectedSource;
}

} // namespace

class NetEaseIntegrationTest : public QObject
{
    Q_OBJECT
private slots:
    void init();
    void cleanup();
    void authorizedPlaybackPersistsCanonicalIdentity();
    void historyDoubleClickRefreshes64BitOnlineIdentity();
    void historyDoubleClickResolvesDuplicateLocalNames();
    void historyUnavailableSourcesLeaveCurrentPlaybackIntact();
    void delayedOnlineResultCannotReplaceNewLocalSelection();
    void replayAndAutomaticNextRefreshStreamUrls();
    void trialAndDeniedResultsRemainDistinct();
    void onlineWordTimingReachesBothLyricViews();
    void logoutInvalidatesPendingAndCurrentPlayback();
    void backendErrorAllowsFreshPlaybackRetry();
    void manualLyricsPersistAndWinOverDelayedCloudLyrics();
    void unsuccessfulAccountVerificationAllowsQrRetry();
    void focusSkipsUnavailableTracksAndTerminatesBoundedFailures();
    void focusPauseDuringResolutionSurvivesDeniedTrackSkip();
    void radioReplacesQueuePreservesHistoryFavoritesAndMetadata();
    void radioSkipsUnavailableTracksAndLoopsPlayableSongs();
    void radioAllUnavailableTerminatesBoundedFailures();
    void radioPauseDuringResolutionSurvivesDeniedTrackSkip();
    void radioLateRepliesCannotReplaceExternalSelection_data();
    void radioLateRepliesCannotReplaceExternalSelection();
private:
    std::unique_ptr<QTemporaryDir> data;
    std::unique_ptr<ScopedEnvironment> environment;
    std::unique_ptr<FakeCloudService> service;
};

void NetEaseIntegrationTest::init()
{
    data = std::make_unique<QTemporaryDir>();
    QVERIFY(data->isValid());
    service = std::make_unique<FakeCloudService>();
    QVERIFY(service->listen());
    environment = std::make_unique<ScopedEnvironment>();
    environment->set("MUSICPLAYER_DATA_DIR", data->path().toUtf8());
    environment->set("MUSICPLAYER_NETEASE_PORT", QByteArray::number(service->port()));
    QVERIFY(QDir().mkpath(data->filePath("netease")));
    QFile token(data->filePath("netease/token"));
    QVERIFY(token.open(QIODevice::WriteOnly));
    QCOMPARE(token.write(service->token), qint64(service->token.size()));
    qRegisterMetaType<OnlineSong>();
    qRegisterMetaType<QMediaPlayer::State>();
    qRegisterMetaType<QMediaPlayer::MediaStatus>();
    qRegisterMetaType<QMediaPlayer::Error>();
}

void NetEaseIntegrationTest::cleanup()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    // MainWindow owns the default database; close it before deleting this
    // test's temporary data directory and installing the next isolated path.
    if (QSqlDatabase::contains()) {
        QSqlDatabase::database(QSqlDatabase::defaultConnection, false).close();
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }
    service.reset();
    environment.reset();
    data.reset();
}

void NetEaseIntegrationTest::authorizedPlaybackPersistsCanonicalIdentity()
{
    MainWindow window;
    constexpr qint64 id = 3000000123LL;
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    QVERIFY(resolved.isValid());
    window.playOnlineSong(song(id));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
    QCOMPARE(resolved.first().at(0).toLongLong(), id);
    QVERIFY(!resolved.first().at(1).toBool());
    QCOMPARE(window.p_PlayerList->mediaCount(), 1);
    QCOMPARE(window.p_PlayerList->currentIndex(), 0);
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), service->streamUrl(id, 1));
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    window.p_PlayerObject->pause();
    auto *like = window.findChild<QPushButton *>("pushButton_AddLike");
    QVERIFY(like);
    like->click();
    QSqlQuery favorite;
    QVERIFY2(favorite.exec("SELECT song_id, media_url FROM favorite_songs"),
             qPrintable(favorite.lastError().text()));
    QVERIFY(favorite.next());
    QCOMPARE(favorite.value(0).toLongLong(), id);
    const QString canonical = QStringLiteral("https://music.163.com/song?id=%1").arg(id);
    QCOMPARE(favorite.value(1).toString(), canonical);
    QSqlQuery history;
    QVERIFY(history.exec("SELECT song_id, media_url FROM play_history"));
    QVERIFY(history.next());
    QCOMPARE(history.value(0).toLongLong(), id);
    QCOMPARE(history.value(1).toString(), canonical);
    history.finish();
    favorite.finish();
    window.p_PlayerObject->stop();
    QSqlQuery clear;
    QVERIFY(clear.exec("DELETE FROM play_history"));
    window.playOnlineSong(song(id));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 2, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QSqlQuery recreated;
    QVERIFY(recreated.exec("SELECT COUNT(*) FROM play_history"));
    QVERIFY(recreated.next());
    QCOMPARE(recreated.value(0).toInt(), 1);
    QCOMPARE(service->unauthorizedRequests, 0);
    window.p_PlayerObject->stop();
}

void NetEaseIntegrationTest::historyDoubleClickRefreshes64BitOnlineIdentity()
{
    MainWindow window;
    constexpr qint64 id = 3000000999LL;
    const QString canonical = QStringLiteral("https://music.163.com/song?id=%1").arg(id);
    QSqlQuery query;
    query.prepare("INSERT INTO play_history (song_id,song_name,artist_name,media_url) "
                  "VALUES (42,'历史中的原版','原版歌手',?)");
    query.addBindValue(canonical);
    QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
    // A stale legacy numeric column must never override the stable URL id.
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    QCOMPARE(activateHistoryRow(window, 0), canonical);
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
    QCOMPARE(resolved.first().at(0).toLongLong(), id);
    QCOMPARE(service->resolutionCount.value(id), 1);
    QCOMPARE(service->resolutionCount.value(42), 0);
    auto *title = window.findChild<QLabel *>("songTitle");
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("历史中的原版"));
    window.p_PlayerObject->stop();
    window.onSongSelected1(canonical);
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 2, 7000);
    QCOMPARE(service->resolutionCount.value(id), 2);
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), service->streamUrl(id, 2));
    QCOMPARE(window.p_PlayerList->mediaCount(), 1);
    window.p_PlayerObject->stop();
}

void NetEaseIntegrationTest::historyDoubleClickResolvesDuplicateLocalNames()
{
    MainWindow window;
    QVERIFY(QDir().mkpath(data->filePath("first")));
    QVERIFY(QDir().mkpath(data->filePath("second")));
    const QString firstPath = data->filePath("first/同名歌曲.wav");
    const QString secondPath = data->filePath("second/同名歌曲.wav");
    for (const QString &path : {firstPath, secondPath}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(wave(12)), qint64(wave(12).size()));
    }
    window.addLocalFiles({secondPath});
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    window.p_PlayerObject->stop();
    QSqlQuery query;
    QVERIFY(query.exec("DELETE FROM play_history"));
    for (const QString &path : {firstPath, secondPath}) {
        query.prepare("INSERT INTO play_history (song_id,song_name,artist_name,media_url) "
                      "VALUES (-42,'同名歌曲','本地音乐',?)");
        query.addBindValue(QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
        QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
    }
    const QUrl firstUrl = QUrl::fromLocalFile(firstPath);
    QCOMPARE(activateHistoryRow(window, 1), firstUrl.toString(QUrl::FullyEncoded));
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->currentMedia().request().url(), firstUrl, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QCOMPARE(window.p_PlayerList->currentIndex(), 1);
    window.p_PlayerObject->stop();
}

void NetEaseIntegrationTest::historyUnavailableSourcesLeaveCurrentPlaybackIntact()
{
    MainWindow window;
    const QString localPath = data->filePath("current.wav");
    QFile local(localPath);
    QVERIFY(local.open(QIODevice::WriteOnly));
    QCOMPARE(local.write(wave(12)), qint64(wave(12).size()));
    local.close();
    window.addLocalFiles({localPath});
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    const QUrl currentUrl = window.p_PlayerObject->currentMedia().request().url();
    const int currentIndex = window.p_PlayerList->currentIndex();
    int missingWarnings = 0;
    QTimer closer;
    connect(&closer, &QTimer::timeout, &closer, [&] {
        for (auto *message : window.findChildren<QMessageBox *>()) {
            if (message->isVisible()) {
                ++missingWarnings;
                message->accept();
            }
        }
    });
    closer.start(10);
    window.onSongSelected1(QUrl::fromLocalFile(data->filePath("removed.wav"))
                               .toString(QUrl::FullyEncoded));
    QCOMPARE(missingWarnings, 1);
    for (const QString &source : {QStringLiteral("https://p1.music.126.net/expired.wav"),
                                  QStringLiteral("https://music.163.com/song?id=invalid"),
                                  QStringLiteral("https://music.163.com/song?id=9007199254740992")}) {
        window.onSongSelected1(source);
        QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), currentUrl);
        QCOMPARE(window.p_PlayerList->currentIndex(), currentIndex);
        QCOMPARE(window.p_PlayerList->mediaCount(), 1);
    }
    QVERIFY(service->resolutionCount.isEmpty());
    window.p_PlayerObject->stop();
}

void NetEaseIntegrationTest::delayedOnlineResultCannotReplaceNewLocalSelection()
{
    MainWindow window;
    constexpr qint64 id = 3000000124LL;
    service->delaySongs.insert(id);
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    window.playOnlineSong(song(id));
    QTRY_COMPARE_WITH_TIMEOUT(service->resolutionCount.value(id), 1, 7000);
    const QString localPath = data->filePath("new-selection.wav");
    QFile local(localPath);
    QVERIFY(local.open(QIODevice::WriteOnly));
    QCOMPARE(local.write(wave(12)), qint64(wave(12).size()));
    local.close();
    window.addLocalFiles({localPath});
    const QUrl localUrl = QUrl::fromLocalFile(localPath);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->currentMedia().request().url(), localUrl, 5000);
    service->releaseDelayed();
    QTest::qWait(300);
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), localUrl);
    QCOMPARE(window.p_PlayerList->currentIndex(), 1);
    QCOMPARE(resolved.count(), 0);
    window.p_PlayerObject->stop();
}

void NetEaseIntegrationTest::replayAndAutomaticNextRefreshStreamUrls()
{
    MainWindow window;
    constexpr qint64 firstId = 3000000125LL;
    constexpr qint64 secondId = 3000000126LL;
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    window.playOnlineSong(song(firstId));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
    window.playOnlineSong(song(secondId));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 2, 7000);
    auto *previous = window.findChild<QPushButton *>("pushButton_PreviousSong");
    QVERIFY(previous);
    previous->click();
    QTRY_COMPARE_WITH_TIMEOUT(service->resolutionCount.value(firstId), 2, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 3, 7000);
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), service->streamUrl(firstId, 2));
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);

    // Feed the actual backend event to the controller, avoiding a twelve
    // second audio wait while still exercising automatic queue advancement.
    QVERIFY(QMetaObject::invokeMethod(window.p_PlayerObject, "mediaStatusChanged",
        Qt::DirectConnection, Q_ARG(QMediaPlayer::MediaStatus, QMediaPlayer::EndOfMedia)));
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerList->currentIndex(), 1, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(service->resolutionCount.value(secondId), 2, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 4, 7000);
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), service->streamUrl(secondId, 2));

    window.p_PlayerList->setPlaybackMode(QMediaPlaylist::CurrentItemInLoop);
    QVERIFY(QMetaObject::invokeMethod(window.p_PlayerObject, "mediaStatusChanged",
        Qt::DirectConnection, Q_ARG(QMediaPlayer::MediaStatus, QMediaPlayer::EndOfMedia)));
    QTRY_COMPARE_WITH_TIMEOUT(service->resolutionCount.value(secondId), 3, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 5, 7000);
    QCOMPARE(window.p_PlayerList->currentIndex(), 1);
    window.p_PlayerObject->stop();
}

void NetEaseIntegrationTest::onlineWordTimingReachesBothLyricViews()
{
    service->yrc = QStringLiteral("[0,4000](0,2000,0)Song (2000,2000,0)beginning\n"
                                  "[4000,4000](4000,2000,0)Trial (6000,2000,0)excerpt begins");
    MainWindow window;
    window.show();
    auto *details = window.findChild<QPushButton *>("miniPlayerButton");
    QVERIFY(details);
    details->click();
    window.playOnlineSong(song(3000000199LL));
    auto *embedded = window.findChild<LyricsView *>("lyricsView");
    auto *floating = window.findChild<FloatingLyricsWidget *>("floatingLyrics");
    QVERIFY(embedded);
    QVERIFY(floating);
    QTRY_VERIFY_WITH_TIMEOUT(embedded->document().hasWordTiming(), 7000);
    QVERIFY(floating->document().hasWordTiming());
    QCOMPARE(embedded->document().lines().first().words.size(), 2);
    QCOMPARE(floating->document().lines().first().words.at(1).startMs, qint64(2000));
    QCOMPARE(embedded->document().lineProgress(0, 1000), qreal(2.5 / 14.0));
    auto *toggle = window.findChild<QPushButton *>("floatingLyricsButton");
    QVERIFY(toggle);
    toggle->setChecked(true);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    window.p_PlayerObject->pause();
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PausedState, 3000);
    for (auto *timer : embedded->findChildren<QTimer *>()) QVERIFY(!timer->isActive());
    for (auto *timer : floating->findChildren<QTimer *>()) QVERIFY(!timer->isActive());
    QVERIFY(floating->testAttribute(Qt::WA_TranslucentBackground));
    QVERIFY(floating->windowFlags().testFlag(Qt::FramelessWindowHint));
    toggle->setChecked(false);
}

void NetEaseIntegrationTest::trialAndDeniedResultsRemainDistinct()
{
    MainWindow window;
    constexpr qint64 trialId = 3000000127LL;
    constexpr qint64 deniedId = 3000000128LL;
    service->access.insert(trialId, FakeCloudService::Trial);
    service->access.insert(deniedId, FakeCloudService::Denied);
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    QSignalSpy failed(&window, SIGNAL(onlinePlaybackFailed(qint64,QString)));
    QVERIFY(failed.isValid());
    window.playOnlineSong(song(trialId));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
    QVERIFY(resolved.first().at(1).toBool());
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    auto *embedded = window.findChild<LyricsView *>("lyricsView");
    auto *floating = window.findChild<FloatingLyricsWidget *>("floatingLyrics");
    QVERIFY(embedded);
    QVERIFY(floating);
    QTRY_COMPARE_WITH_TIMEOUT(embedded->document().lines().size(), 2, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(embedded->currentLineIndex(), 1, 5000);
    QCOMPARE(floating->currentLineIndex(), 1);
    QCOMPARE(embedded->accessibleDescription(), QStringLiteral("Trial excerpt begins"));
    QCOMPARE(floating->accessibleDescription(), QStringLiteral("Trial excerpt begins"));
    QTRY_VERIFY_WITH_TIMEOUT(window.p_PlayerObject->position() > 250, 5000);
    auto *play = window.findChild<QPushButton *>("pushButton_PlaySong");
    QVERIFY(play);
    play->click();
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PausedState, 5000);
    // A full-song lyric time of five seconds is one second into a clip that
    // starts at four seconds. Clicking lyrics must seek on the clip's clock.
    QVERIFY(QMetaObject::invokeMethod(embedded, "seekRequested", Qt::DirectConnection,
                                     Q_ARG(qint64, 5000)));
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(window.p_PlayerObject->position() - 1000) < 300, 5000);
    QVERIFY(QMetaObject::invokeMethod(embedded, "seekRequested", Qt::DirectConnection,
                                     Q_ARG(qint64, 2000)));
    QTRY_VERIFY_WITH_TIMEOUT(window.p_PlayerObject->position() < 300, 5000);
    play->click();
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 5000);
    bool visibleTrialMarker = false;
    for (const auto *label : window.findChildren<QLabel *>())
        visibleTrialMarker |= label->text().contains(QStringLiteral("仅试听"));
    QVERIFY2(visibleTrialMarker, "The trial entitlement must remain visible after pause/resume");
    window.playOnlineSong(song(deniedId));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 7000);
    QCOMPARE(failed.first().at(0).toLongLong(), deniedId);
    QVERIFY(!failed.first().at(1).toString().isEmpty());
    QCOMPARE(resolved.count(), 1);
    QVERIFY(window.p_PlayerObject->state() != QMediaPlayer::PlayingState);
    // No retry loop and no implicit skip back to the previous trial track.
    QTest::qWait(200);
    QCOMPARE(service->resolutionCount.value(deniedId), 1);
    QCOMPARE(window.p_PlayerList->currentIndex(), 1);
    window.p_PlayerObject->stop();
}

void NetEaseIntegrationTest::logoutInvalidatesPendingAndCurrentPlayback()
{
    MainWindow window;
    constexpr qint64 id = 3000000129LL;
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    window.playOnlineSong(song(id));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QVERIFY(QMetaObject::invokeMethod(&window, "logoutNetease", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(service->logoutRequests, 1, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::StoppedState, 5000);
    QVERIFY(window.p_PlayerObject->currentMedia().isNull());

    constexpr qint64 pendingId = 3000000130LL;
    service->delaySongs.insert(pendingId);
    window.playOnlineSong(song(pendingId));
    QTRY_COMPARE_WITH_TIMEOUT(service->resolutionCount.value(pendingId), 1, 7000);
    QVERIFY(QMetaObject::invokeMethod(&window, "logoutNetease", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(service->logoutRequests, 2, 7000);
    service->releaseDelayed();
    QTest::qWait(300);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(window.p_PlayerObject->state(), QMediaPlayer::StoppedState);
    QVERIFY(window.p_PlayerObject->currentMedia().isNull());
    QCOMPARE(service->unauthorizedRequests, 0);
}

void NetEaseIntegrationTest::backendErrorAllowsFreshPlaybackRetry()
{
    MainWindow window;
    constexpr qint64 id = 3000000132LL;
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    QSignalSpy failed(&window, SIGNAL(onlinePlaybackFailed(qint64,QString)));
    window.playOnlineSong(song(id));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);

    // Some native failures arrive while Qt still reports PlayingState. Feed
    // that backend signal into the controller without relying on a platform's
    // codec-specific timeout or corrupting an otherwise valid WAV fixture.
    QVERIFY(QMetaObject::invokeMethod(window.p_PlayerObject, "error", Qt::DirectConnection,
                                     Q_ARG(QMediaPlayer::Error, QMediaPlayer::ResourceError)));
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::StoppedState, 5000);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.first().at(0).toLongLong(), id);
    auto *play = window.findChild<QPushButton *>("pushButton_PlaySong");
    QVERIFY(play);
    play->click();
    QTRY_COMPARE_WITH_TIMEOUT(service->resolutionCount.value(id), 2, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 2, 7000);
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), service->streamUrl(id, 2));
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QTRY_VERIFY_WITH_TIMEOUT(window.p_PlayerObject->position() > 100, 5000);
    QCOMPARE(service->unauthorizedRequests, 0);
    window.p_PlayerObject->stop();
}

void NetEaseIntegrationTest::manualLyricsPersistAndWinOverDelayedCloudLyrics()
{
    constexpr qint64 id = 3000000131LL;
    const QString lyricPath = data->filePath("user-provided.lrc");
    QFile lyricFile(lyricPath);
    QVERIFY(lyricFile.open(QIODevice::WriteOnly));
    const QByteArray lyricText = "[00:00.000]User imported lyrics\n[00:04.000]User second line\n";
    QCOMPARE(lyricFile.write(lyricText), qint64(lyricText.size()));
    lyricFile.close();
    service->delayLyrics = true;
    {
        MainWindow window;
        QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
        window.playOnlineSong(song(id));
        QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
        QTRY_COMPARE_WITH_TIMEOUT(service->lyricsRequests, 1, 7000);
        auto *import = window.findChild<QPushButton *>("importLyricsButton");
        auto *embedded = window.findChild<LyricsView *>("lyricsView");
        QVERIFY(import);
        QVERIFY(embedded);

        // Use the actual import action, with only this application's Qt file
        // picker automated. No platform-wide keyboard or desktop input is sent.
        const bool previousNativeSetting = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        bool selectedFile = false;
        QTimer chooser;
        connect(&chooser, &QTimer::timeout, &window, [&] {
            for (auto *dialog : window.findChildren<QFileDialog *>()) {
                if (!dialog->isVisible())
                    continue;
                if (!dialog->property("testPickerObserved").toBool()) {
                    dialog->setProperty("testPickerObserved", true);
                    connect(dialog, &QDialog::accepted, &window, [&selectedFile] {
                        selectedFile = true;
                    });
                }
                dialog->setDirectory(QFileInfo(lyricPath).absolutePath());
                dialog->selectFile(QFileInfo(lyricPath).fileName());
                if (auto *filename = dialog->findChild<QLineEdit *>("fileNameEdit"))
                    filename->setText(QFileInfo(lyricPath).fileName());
                QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            }
        });
        QTimer deadline;
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, &window, [&] {
            for (auto *dialog : window.findChildren<QFileDialog *>())
                dialog->reject();
        });
        chooser.start(30);
        deadline.start(5000);
        import->click();
        chooser.stop();
        deadline.stop();
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previousNativeSetting);
        QVERIFY2(selectedFile, "The owned lyrics file picker did not accept the test file");
        QCOMPARE(embedded->document().lines().size(), 2);
        QCOMPARE(embedded->document().lines().first().text, QStringLiteral("User imported lyrics"));
        service->releaseDelayed();
        QTest::qWait(200);
        QCOMPARE(embedded->document().lines().first().text, QStringLiteral("User imported lyrics"));
        window.p_PlayerObject->stop();
    }
    service->delayLyrics = false;
    {
        MainWindow restored;
        QSignalSpy resolved(&restored, SIGNAL(onlinePlaybackResolved(qint64,bool)));
        restored.playOnlineSong(song(id));
        QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
        auto *embedded = restored.findChild<LyricsView *>("lyricsView");
        auto *floating = restored.findChild<FloatingLyricsWidget *>("floatingLyrics");
        QVERIFY(embedded);
        QVERIFY(floating);
        QCOMPARE(embedded->document().lines().size(), 2);
        QCOMPARE(embedded->document().lines().first().text, QStringLiteral("User imported lyrics"));
        QCOMPARE(floating->document().lines().first().text, QStringLiteral("User imported lyrics"));
        QTest::qWait(100);
        QCOMPARE(service->lyricsRequests, 1);
        restored.p_PlayerObject->stop();
    }
}

void NetEaseIntegrationTest::unsuccessfulAccountVerificationAllowsQrRetry()
{
    MainWindow window;
    service->accountLoggedIn = false;
    bool controlsRestored = false;
    QTimer observer;
    connect(&observer, &QTimer::timeout, &window, [&] {
        for (auto *dialog : window.findChildren<NetEaseLoginDialog *>()) {
            if (!dialog->isVisible())
                continue;
            for (auto *timer : dialog->findChildren<QTimer *>()) {
                if (timer->interval() == 2000)
                    timer->setInterval(10);
            }
            if (service->qrCheckRequests < 1 || service->accountRequests < 2)
                continue;
            auto *refresh = dialog->findChild<QPushButton *>("button_NeteaseRefreshQr");
            auto *progress = dialog->findChild<QProgressBar *>("progress_NeteaseLogin");
            auto *status = dialog->findChild<QLabel *>("label_NeteaseLoginStatus");
            if (refresh && progress && status && refresh->isEnabled() && !progress->isVisible()
                && status->text().contains(QStringLiteral("登录尚未生效"))) {
                controlsRestored = true;
                dialog->reject();
            }
        }
    });
    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, &window, [&] {
        for (auto *dialog : window.findChildren<NetEaseLoginDialog *>())
            dialog->reject();
    });
    observer.start(10);
    deadline.start(6000);
    const bool invoked = QMetaObject::invokeMethod(&window, "openNeteaseAccount", Qt::DirectConnection);
    observer.stop();
    deadline.stop();
    QVERIFY(invoked);
    QVERIFY2(controlsRestored, "A QR 803 response with logged-out account status must permit retry");
    QCOMPARE(service->unauthorizedRequests, 0);
}

void NetEaseIntegrationTest::focusSkipsUnavailableTracksAndTerminatesBoundedFailures()
{
    MainWindow window;
    auto *session = window.findChild<FocusSession *>("focusSession");
    QVERIFY(session);
    constexpr qint64 trialId = 3000000201LL;
    constexpr qint64 deniedId = 3000000202LL;
    constexpr qint64 fullId = 3000000203LL;
    service->access.insert(trialId, FakeCloudService::Trial);
    service->access.insert(deniedId, FakeCloudService::Denied);
    service->access.insert(fullId, FakeCloudService::Full);
    const auto canonical = [](qint64 id) {
        return QStringLiteral("https://music.163.com/song?id=%1").arg(id);
    };
    const auto favorite = [&](qint64 id) {
        QSqlQuery query;
        query.prepare("INSERT INTO favorite_songs(song_id,song_name,artist_name,media_url) "
                      "VALUES(?,?,?,?)");
        query.addBindValue(id);
        query.addBindValue(QStringLiteral("专注隔离测试 %1").arg(id));
        query.addBindValue(QStringLiteral("隔离测试歌手"));
        query.addBindValue(canonical(id));
        return query.exec();
    };
    // Favorites are read newest first. Build the plan from the same public
    // candidate path used by the dialog, without pre-playing any song.
    QVERIFY(favorite(fullId));
    QVERIFY(favorite(deniedId));
    QVERIFY(favorite(trialId));
    FocusRequest request;
    request.minutes = 1;
    request.source = QStringLiteral("favorites");
    const auto plan = FocusPlanner::build(request, window.focusCandidates(request.source));
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.tracks.size(), 3);
    QCOMPARE(plan.tracks.first().key, canonical(trialId));
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    QVERIFY(window.startFocusPlan(plan));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
    QCOMPARE(resolved.first().at(0).toLongLong(), fullId);
    QVERIFY(!resolved.first().at(1).toBool());
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QTRY_VERIFY_WITH_TIMEOUT(window.p_PlayerObject->position() > 0, 7000);
    QVERIFY(session->isActive());
    QVERIFY(session->isRunning());
    QCOMPARE(session->currentKey(), canonical(fullId));
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), service->streamUrl(fullId, 1));
    QCOMPARE(service->resolutionCount.value(trialId), 1);
    QCOMPARE(service->resolutionCount.value(deniedId), 1);
    QCOMPARE(service->resolutionCount.value(fullId), 1);
    QCOMPARE(service->audioRequestCount.value(trialId), 0);
    QCOMPARE(service->audioRequestCount.value(deniedId), 0);
    QVERIFY(service->audioRequestCount.value(fullId) > 0);

    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_StopPlaySong_clicked",
                                    Qt::DirectConnection));
    QVERIFY(!session->isActive());
    QSqlQuery clear;
    QVERIFY(clear.exec("DELETE FROM favorite_songs"));
    QVERIFY(favorite(deniedId));
    QVERIFY(favorite(trialId));
    const auto deniedPlan = FocusPlanner::build(request, window.focusCandidates(request.source));
    QCOMPARE(deniedPlan.tracks.size(), 2);
    QSignalSpy finished(session, SIGNAL(finished(QString,bool)));
    QVERIFY(window.startFocusPlan(deniedPlan));
    QTRY_VERIFY_WITH_TIMEOUT(!session->isActive(), 7000);
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.first().at(1).toBool());
    QVERIFY(window.p_PlayerObject->state() != QMediaPlayer::PlayingState);
    QCOMPARE(service->resolutionCount.value(trialId), 2);
    QCOMPARE(service->resolutionCount.value(deniedId), 2);
    QTest::qWait(100);
    // An all-unavailable plan must finish once instead of retrying forever.
    QCOMPARE(service->resolutionCount.value(trialId), 2);
    QCOMPARE(service->resolutionCount.value(deniedId), 2);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(service->unauthorizedRequests, 0);
}

void NetEaseIntegrationTest::focusPauseDuringResolutionSurvivesDeniedTrackSkip()
{
    MainWindow window;
    auto *session = window.findChild<FocusSession *>("focusSession");
    auto *play = window.findChild<QPushButton *>("pushButton_PlaySong");
    QVERIFY(session);
    QVERIFY(play);
    constexpr qint64 deniedId = 3000000204LL;
    constexpr qint64 fullId = 3000000205LL;
    const auto canonical = [](qint64 id) {
        return QStringLiteral("https://music.163.com/song?id=%1").arg(id);
    };
    service->access.insert(deniedId, FakeCloudService::Denied);
    service->delaySongs.insert(deniedId);
    for (qint64 id : {fullId, deniedId}) {
        QSqlQuery query;
        query.prepare("INSERT INTO favorite_songs(song_id,song_name,artist_name,media_url) "
                      "VALUES(?,?,?,?)");
        query.addBindValue(id);
        query.addBindValue(QStringLiteral("专注暂停测试 %1").arg(id));
        query.addBindValue(QStringLiteral("隔离测试歌手"));
        query.addBindValue(canonical(id));
        QVERIFY(query.exec());
    }
    FocusRequest request;
    request.minutes = 1;
    request.source = QStringLiteral("favorites");
    const auto plan = FocusPlanner::build(request, window.focusCandidates(request.source));
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.tracks.size(), 2);
    QCOMPARE(plan.tracks.first().key, canonical(deniedId));
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    QVERIFY(window.startFocusPlan(plan));
    QTRY_COMPARE_WITH_TIMEOUT(service->resolutionCount.value(deniedId), 1, 7000);
    QVERIFY(session->isActive());
    QVERIFY(!session->isRunning());
    play->click();
    QVERIFY(session->isPaused());
    QCOMPARE(session->remainingSeconds(), 60);
    QSignalSpy states(window.p_PlayerObject, SIGNAL(stateChanged(QMediaPlayer::State)));
    QVERIFY(states.isValid());
    service->releaseDelayed();
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
    QCOMPARE(resolved.first().at(0).toLongLong(), fullId);
    QCOMPARE(session->currentKey(), canonical(fullId));
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), service->streamUrl(fullId, 1));
    QTest::qWait(300);
    QVERIFY(session->isActive());
    QVERIFY(session->isPaused());
    QVERIFY(!session->isRunning());
    QVERIFY(window.p_PlayerObject->state() != QMediaPlayer::PlayingState);
    QCOMPARE(session->remainingSeconds(), 60);
    for (const auto &state : states)
        QVERIFY(state.first().toInt() != int(QMediaPlayer::PlayingState));
    QCOMPARE(service->resolutionCount.value(deniedId), 1);
    QCOMPARE(service->resolutionCount.value(fullId), 1);

    // The user, rather than a delayed permission response, resumes playback.
    play->click();
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QTRY_VERIFY_WITH_TIMEOUT(window.p_PlayerObject->position() > 0, 7000);
    QVERIFY(!session->isPaused());
    QVERIFY(session->isRunning());
    // A backend that kept the prepared media stopped may re-resolve when the
    // user starts it for the first time; neither path may retry autonomously.
    const int resolutions = service->resolutionCount.value(fullId);
    QVERIFY(resolutions >= 1 && resolutions <= 2);
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(),
             service->streamUrl(fullId, resolutions));
    QVERIFY(service->audioRequestCount.value(fullId) > 0);
    QCOMPARE(service->audioRequestCount.value(deniedId), 0);
    QCOMPARE(service->unauthorizedRequests, 0);
    session->cancel();
}

void NetEaseIntegrationTest::radioReplacesQueuePreservesHistoryFavoritesAndMetadata()
{
    MainWindow window;
    constexpr qint64 oldId = 3000000301LL;
    constexpr qint64 firstId = 3000000302LL;
    constexpr qint64 secondId = 3000000303LL;
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    window.playOnlineSong(song(oldId));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    window.p_PlayerObject->pause();
    auto *like = window.findChild<QPushButton *>("pushButton_AddLike");
    QVERIFY(like);
    like->click();
    const auto oldRecordExists = [](const QString &table) {
        QSqlQuery query;
        if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE song_id=%2").arg(table).arg(oldId)) ||
            !query.next())
            return false;
        return query.value(0).toInt() == 1;
    };
    QVERIFY(oldRecordExists(QStringLiteral("favorite_songs")));
    QVERIFY(oldRecordExists(QStringLiteral("play_history")));
    auto first = radioSong(firstId, QStringLiteral("新的第一首"));
    first.song.artist = QStringLiteral("电台歌手");
    first.song.album = QStringLiteral("电台专辑");
    // Keep the artwork-role sentinel on this isolated loopback server. The
    // production image policy rejects non-CDN URLs without external traffic.
    first.song.coverUrl = QStringLiteral("http://127.0.0.1:%1/radio-cover.png").arg(service->port());
    const auto second = radioSong(secondId, QStringLiteral("新的第二首"));
    QVERIFY(window.startRadio({first, second}));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 2, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QCOMPARE(resolved.last().at(0).toLongLong(), firstId);
    QCOMPARE(window.p_PlayerList->mediaCount(), 2);
    QCOMPARE(window.p_PlayerList->currentIndex(), 0);
    for (int index = 0; index < 2; ++index) {
        const qint64 id = index == 0 ? firstId : secondId;
        QCOMPARE(window.p_PlayerList->media(index).request().url(),
                 QUrl(QStringLiteral("https://music.163.com/song?id=%1").arg(id)));
    }
    auto *queue = window.findChild<QTableView *>("queueView");
    QVERIFY(queue);
    QCOMPARE(queue->model()->rowCount(), 2);
    const auto firstRow = queue->model()->index(0, 1);
    QCOMPARE(firstRow.data().toString(), first.song.title);
    QCOMPARE(firstRow.data(SongListDelegate::ArtistRole).toString(), first.song.artist);
    QCOMPARE(firstRow.data(SongListDelegate::SongIdRole).toLongLong(), firstId);
    QCOMPARE(firstRow.data(SongListDelegate::CoverUrlRole).toString(), first.song.coverUrl);
    QCOMPARE(queue->model()->index(0, 2).data().toString(), first.song.album);
    QCOMPARE(firstRow.data(SongListDelegate::MediaUrlRole).toUrl(),
             QUrl(QStringLiteral("https://music.163.com/song?id=%1").arg(firstId)));
    QVERIFY(oldRecordExists(QStringLiteral("favorite_songs")));
    QVERIFY(oldRecordExists(QStringLiteral("play_history")));
    QCOMPARE(service->resolutionCount.value(oldId), 1);
    QCOMPARE(service->resolutionCount.value(secondId), 0);
    QCOMPARE(service->unauthorizedRequests, 0);
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_StopPlaySong_clicked", Qt::DirectConnection));
}

void NetEaseIntegrationTest::radioSkipsUnavailableTracksAndLoopsPlayableSongs()
{
    MainWindow window;
    constexpr qint64 trialId = 3000000311LL;
    constexpr qint64 deniedId = 3000000312LL;
    constexpr qint64 firstId = 3000000313LL;
    constexpr qint64 secondId = 3000000314LL;
    service->access.insert(trialId, FakeCloudService::Trial);
    service->access.insert(deniedId, FakeCloudService::Denied);
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    QVERIFY(window.startRadio({radioSong(trialId), radioSong(deniedId), radioSong(firstId), radioSong(secondId)}));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
    QCOMPARE(resolved.first().at(0).toLongLong(), firstId);
    QVERIFY(!resolved.first().at(1).toBool());
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QTRY_VERIFY_WITH_TIMEOUT(window.p_PlayerObject->position() > 0, 7000);
    QCOMPARE(service->resolutionCount.value(trialId), 1);
    QCOMPARE(service->resolutionCount.value(deniedId), 1);
    QCOMPARE(service->audioRequestCount.value(trialId), 0);
    QCOMPARE(service->audioRequestCount.value(deniedId), 0);
    QVERIFY(service->audioRequestCount.value(firstId) > 0);

    QVERIFY(QMetaObject::invokeMethod(window.p_PlayerObject, "mediaStatusChanged",
        Qt::DirectConnection, Q_ARG(QMediaPlayer::MediaStatus, QMediaPlayer::EndOfMedia)));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 2, 7000);
    QCOMPARE(resolved.last().at(0).toLongLong(), secondId);
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QCOMPARE(window.p_PlayerList->currentIndex(), 3);

    // End the final song through the real controller event. The radio loops
    // to the next full track and never retries the failed candidates.
    QVERIFY(QMetaObject::invokeMethod(window.p_PlayerObject, "mediaStatusChanged",
        Qt::DirectConnection, Q_ARG(QMediaPlayer::MediaStatus, QMediaPlayer::EndOfMedia)));
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 3, 7000);
    QCOMPARE(resolved.last().at(0).toLongLong(), firstId);
    QCOMPARE(window.p_PlayerList->currentIndex(), 2);
    QCOMPARE(service->resolutionCount.value(firstId), 2);
    QCOMPARE(service->resolutionCount.value(secondId), 1);
    QCOMPARE(service->resolutionCount.value(trialId), 1);
    QCOMPARE(service->resolutionCount.value(deniedId), 1);
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), service->streamUrl(firstId, 2));
    QCOMPARE(service->unauthorizedRequests, 0);
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_StopPlaySong_clicked", Qt::DirectConnection));
}

void NetEaseIntegrationTest::radioAllUnavailableTerminatesBoundedFailures()
{
    MainWindow window;
    constexpr qint64 deniedId = 3000000321LL;
    constexpr qint64 trialId = 3000000322LL;
    service->access.insert(deniedId, FakeCloudService::Denied);
    service->access.insert(trialId, FakeCloudService::Trial);
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    QVERIFY(window.startRadio({radioSong(deniedId), radioSong(trialId)}));
    QTRY_COMPARE_WITH_TIMEOUT(service->resolutionCount.value(trialId), 1, 7000);
    QTRY_VERIFY_WITH_TIMEOUT(window.p_PlayerObject->state() != QMediaPlayer::PlayingState, 7000);
    QTest::qWait(300);
    QCOMPARE(resolved.count(), 0);
    QCOMPARE(service->resolutionCount.value(deniedId), 1);
    QCOMPARE(service->resolutionCount.value(trialId), 1);
    QCOMPARE(service->audioRequestCount.value(deniedId), 0);
    QCOMPARE(service->audioRequestCount.value(trialId), 0);
    QCOMPARE(window.p_PlayerObject->state(), QMediaPlayer::StoppedState);
    QVERIFY(window.p_PlayerObject->currentMedia().isNull());
    QCOMPARE(service->unauthorizedRequests, 0);
}

void NetEaseIntegrationTest::radioPauseDuringResolutionSurvivesDeniedTrackSkip()
{
    MainWindow window;
    constexpr qint64 deniedId = 3000000331LL;
    constexpr qint64 fullId = 3000000332LL;
    service->access.insert(deniedId, FakeCloudService::Denied);
    service->delaySongs.insert(deniedId);
    auto *play = window.findChild<QPushButton *>("pushButton_PlaySong");
    QVERIFY(play);
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    QVERIFY(window.startRadio({radioSong(deniedId), radioSong(fullId)}));
    QTRY_COMPARE_WITH_TIMEOUT(service->resolutionCount.value(deniedId), 1, 7000);
    play->click();
    QSignalSpy states(window.p_PlayerObject, SIGNAL(stateChanged(QMediaPlayer::State)));
    QVERIFY(states.isValid());
    service->releaseDelayed();
    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
    QCOMPARE(resolved.first().at(0).toLongLong(), fullId);
    QCOMPARE(window.p_PlayerList->currentIndex(), 1);
    QTest::qWait(300);
    QVERIFY(window.p_PlayerObject->state() != QMediaPlayer::PlayingState);
    for (const auto &state : states)
        QVERIFY(state.first().toInt() != int(QMediaPlayer::PlayingState));
    QCOMPARE(service->resolutionCount.value(deniedId), 1);
    QCOMPARE(service->resolutionCount.value(fullId), 1);
    QCOMPARE(service->audioRequestCount.value(deniedId), 0);

    play->click();
    QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    QTRY_VERIFY_WITH_TIMEOUT(window.p_PlayerObject->position() > 0, 7000);
    const int resolutions = service->resolutionCount.value(fullId);
    QVERIFY(resolutions >= 1 && resolutions <= 2);
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), service->streamUrl(fullId, resolutions));
    QCOMPARE(service->unauthorizedRequests, 0);
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_StopPlaySong_clicked", Qt::DirectConnection));
}

void NetEaseIntegrationTest::radioLateRepliesCannotReplaceExternalSelection_data()
{
    QTest::addColumn<QString>("action");
    QTest::newRow("add-local-file") << QStringLiteral("local");
    QTest::newRow("manual-online-selection") << QStringLiteral("online");
    QTest::newRow("logout-netease") << QStringLiteral("logout");
}

void NetEaseIntegrationTest::radioLateRepliesCannotReplaceExternalSelection()
{
    QFETCH(QString, action);
    MainWindow window;
    constexpr qint64 pendingId = 3000000341LL;
    constexpr qint64 secondId = 3000000342LL;
    constexpr qint64 manualId = 3000000343LL;
    service->delaySongs.insert(pendingId);
    QSignalSpy resolved(&window, SIGNAL(onlinePlaybackResolved(qint64,bool)));
    QVERIFY(window.startRadio({radioSong(pendingId), radioSong(secondId)}));
    QTRY_COMPARE_WITH_TIMEOUT(service->resolutionCount.value(pendingId), 1, 7000);
    QUrl expected;
    if (action == QStringLiteral("local")) {
        const QString path = data->filePath("after-radio.wav");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray audio = wave(12);
        QCOMPARE(file.write(audio), qint64(audio.size()));
        file.close();
        window.addLocalFiles({path});
        expected = QUrl::fromLocalFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    } else if (action == QStringLiteral("online")) {
        window.playOnlineSong(song(manualId, QStringLiteral("用户手动选择")));
        QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 7000);
        QCOMPARE(resolved.first().at(0).toLongLong(), manualId);
        expected = service->streamUrl(manualId, 1);
        QTRY_COMPARE_WITH_TIMEOUT(window.p_PlayerObject->state(), QMediaPlayer::PlayingState, 7000);
    } else {
        QVERIFY(QMetaObject::invokeMethod(&window, "logoutNetease", Qt::DirectConnection));
        QTRY_COMPARE_WITH_TIMEOUT(service->logoutRequests, 1, 7000);
        QCOMPARE(window.p_PlayerObject->state(), QMediaPlayer::StoppedState);
        QVERIFY(window.p_PlayerObject->currentMedia().isNull());
    }
    service->releaseDelayed();
    QTest::qWait(300);
    QCOMPARE(window.p_PlayerObject->currentMedia().request().url(), expected);
    QCOMPARE(resolved.count(), action == QStringLiteral("online") ? 1 : 0);
    QCOMPARE(service->resolutionCount.value(pendingId), 1);
    QCOMPARE(service->resolutionCount.value(secondId), 0);
    QCOMPARE(service->audioRequestCount.value(pendingId), 0);
    QCOMPARE(service->unauthorizedRequests, 0);
    if (action != QStringLiteral("logout"))
        QCOMPARE(window.p_PlayerObject->state(), QMediaPlayer::PlayingState);
    QVERIFY(QMetaObject::invokeMethod(&window, "on_pushButton_StopPlaySong_clicked", Qt::DirectConnection));
}

QTEST_MAIN(NetEaseIntegrationTest)
#include "netease_integration_test.moc"
