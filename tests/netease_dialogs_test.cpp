#include <QtTest>
#include <QBuffer>
#include <QLabel>
#include <QHeaderView>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>

#include "neteasedialogs.h"
#include "songlistdelegate.h"

namespace {

QString imageDataUrl(const QSize &size)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::white);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(encoded.toBase64());
}

QVector<OnlineSong> makeSongs(int count, qint64 firstId = 1)
{
    QVector<OnlineSong> songs;
    for (int index = 0; index < count; ++index)
        songs.append({firstId + index, QStringLiteral("歌曲 %1").arg(firstId + index),
                      QStringLiteral("歌手"), 190000, index % 2 ? 4 : 1, {}, {}});
    return songs;
}

} // namespace

class NetEaseDialogsTest : public QObject
{
    Q_OBJECT

private slots:
    void accountQrAndBusyState();
    void qrRejectsExternalMalformedAndOversizedInput();
    void searchSelectionAndBusyState();
    void searchPaginationAndBoundedRows();
};

void NetEaseDialogsTest::accountQrAndBusyState()
{
    NetEaseLoginDialog dialog;
    QVERIFY(!dialog.windowFlags().testFlag(Qt::FramelessWindowHint));
    auto *refresh = dialog.findChild<QPushButton *>("button_NeteaseRefreshQr");
    auto *logout = dialog.findChild<QPushButton *>("button_NeteaseLogout");
    auto *qr = dialog.findChild<QLabel *>("label_NeteaseQr");
    auto *nickname = dialog.findChild<QLabel *>("label_NeteaseNickname");
    auto *vip = dialog.findChild<QLabel *>("label_NeteaseVip");
    auto *progress = dialog.findChild<QProgressBar *>("progress_NeteaseLogin");
    QVERIFY(refresh && logout && qr && nickname && vip && progress);
    QVERIFY(!refresh->isEnabled());
    QSignalSpy refreshes(&dialog, &NetEaseLoginDialog::refreshQrRequested);
    QSignalSpy logouts(&dialog, &NetEaseLoginDialog::logoutRequested);
    dialog.setQrImage(imageDataUrl(QSize(1024, 1024)));
    QVERIFY(!qr->pixmap(Qt::ReturnByValue).isNull());
    QVERIFY(qr->pixmap(Qt::ReturnByValue).width() <= 320);
    QVERIFY(qr->pixmap(Qt::ReturnByValue).height() <= 320);
    dialog.setBusy(false);
    refresh->click();
    QCOMPARE(refreshes.count(), 1);
    dialog.setAccount(true, QStringLiteral("<b>真实昵称</b>"), 11);
    QCOMPARE(nickname->textFormat(), Qt::PlainText);
    QCOMPARE(nickname->text(), QStringLiteral("<b>真实昵称</b>"));
    QVERIFY(vip->text().contains(QStringLiteral("VIP")));
    QVERIFY(progress->isHidden());
    QVERIFY(qr->pixmap(Qt::ReturnByValue).isNull());
    logout->click();
    QCOMPARE(logouts.count(), 1);
    dialog.setAccount(true, QString(), 0);
    QVERIFY(vip->text().contains(QStringLiteral("普通账号")));
    QVERIFY(!nickname->text().isEmpty());
}

void NetEaseDialogsTest::qrRejectsExternalMalformedAndOversizedInput()
{
    NetEaseLoginDialog dialog;
    auto *qr = dialog.findChild<QLabel *>("label_NeteaseQr");
    QVERIFY(qr);
    const QStringList invalid{
        QStringLiteral("https://example.invalid/qr.png"),
        QStringLiteral("data:image/png;base64,%%%"),
        QStringLiteral("data:image/svg+xml;base64,PHN2Zy8+"),
        QStringLiteral("data:image/png;base64,") + QString(800000, QLatin1Char('A')),
        imageDataUrl(QSize(5000, 2))
    };
    for (const QString &input : invalid) {
        dialog.setQrImage(imageDataUrl(QSize(64, 64)));
        QVERIFY(!qr->pixmap(Qt::ReturnByValue).isNull());
        dialog.setQrImage(input);
        QVERIFY(qr->pixmap(Qt::ReturnByValue).isNull());
        QVERIFY(qr->text().contains(QStringLiteral("不可用")));
    }
}

void NetEaseDialogsTest::searchSelectionAndBusyState()
{
    OnlineSearchDialog dialog;
    QVERIFY(!dialog.windowFlags().testFlag(Qt::FramelessWindowHint));
    auto *play = dialog.findChild<QPushButton *>("button_OnlineSearchPlay");
    auto *official = dialog.findChild<QPushButton *>("button_OnlineSearchOfficial");
    auto *table = dialog.findChild<QTableWidget *>("table_OnlineSearchResults");
    auto *query = dialog.findChild<QLabel *>("label_OnlineSearchQuery");
    QVERIFY(play && official && table && query);
    QVERIFY(!play->isEnabled());
    QSignalSpy activations(&dialog, &OnlineSearchDialog::songActivated);
    QSignalSpy officialPages(&dialog, &OnlineSearchDialog::officialPageRequested);
    auto songs = makeSongs(2);
    songs[0].album = QStringLiteral("黑白灰");
    songs[0].coverUrl = QStringLiteral("https://p3.music.126.net/fixture.jpg");
    dialog.setResults(songs, 2, QStringLiteral("<b>歌名</b>"));
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(query->textFormat(), Qt::PlainText);
    QCOMPARE(table->item(0, 2)->text(), QStringLiteral("3:10"));
    QCOMPARE(table->item(0, 3)->text(), QStringLiteral("VIP"));
    QCOMPARE(table->item(1, 3)->text(), QStringLiteral("数字专辑"));
    QCOMPARE(table->item(0, 1)->text(), songs[0].album);
    QCOMPARE(table->item(0, 0)->data(SongListDelegate::ArtistRole).toString(), songs[0].artist);
    QCOMPARE(table->item(0, 0)->data(SongListDelegate::CoverUrlRole).toString(), songs[0].coverUrl);
    QCOMPARE(table->item(0, 0)->data(SongListDelegate::SongIdRole).toLongLong(), songs[0].id);
    QVERIFY(dynamic_cast<SongListDelegate *>(table->itemDelegate()));
    QCOMPARE(table->verticalHeader()->defaultSectionSize(), 72);
    play->click();
    QCOMPARE(activations.count(), 1);
    QCOMPARE(qvariant_cast<OnlineSong>(activations.at(0).at(0)).id, qint64(1));
    table->selectRow(1);
    official->click();
    QCOMPARE(officialPages.count(), 1);
    QCOMPARE(officialPages.at(0).at(0).toLongLong(), qint64(2));
    dialog.setBusy(true);
    QVERIFY(!play->isEnabled());
    QVERIFY(!official->isEnabled());
    play->click();
    QCOMPARE(activations.count(), 1);
    dialog.setBusy(false);
    table->clearSelection();
    // Selection is required; a stale current index must not trigger playback.
    QVERIFY(!table->selectionModel()->hasSelection());
    QVERIFY(!play->isEnabled());
    play->click();
    QCOMPARE(activations.count(), 1);
}

void NetEaseDialogsTest::searchPaginationAndBoundedRows()
{
    OnlineSearchDialog dialog;
    auto *next = dialog.findChild<QPushButton *>("button_OnlineSearchNext");
    auto *previous = dialog.findChild<QPushButton *>("button_OnlineSearchPrevious");
    auto *table = dialog.findChild<QTableWidget *>("table_OnlineSearchResults");
    auto *page = dialog.findChild<QLabel *>("label_OnlineSearchPage");
    QVERIFY(next && previous && table && page);
    QSignalSpy pages(&dialog, &OnlineSearchDialog::pageRequested);
    dialog.setResults(makeSongs(40), 61, QStringLiteral("测试"));
    QCOMPARE(table->rowCount(), OnlineSearchDialog::PageSize);
    QVERIFY(!previous->isEnabled());
    QVERIFY(next->isEnabled());
    next->click();
    QCOMPARE(pages.count(), 1);
    QCOMPARE(pages.at(0).at(0).toInt(), 30);
    QVERIFY(!next->isEnabled());
    next->click();
    QCOMPARE(pages.count(), 1);
    dialog.setResults(makeSongs(30, 31), 61, QStringLiteral("测试"));
    QCOMPARE(page->text(), QStringLiteral("2 / 3"));
    QVERIFY(previous->isEnabled());
    next->click();
    QCOMPARE(pages.at(1).at(0).toInt(), 60);
    dialog.setResults(makeSongs(1, 61), 61, QStringLiteral("测试"));
    QCOMPARE(page->text(), QStringLiteral("3 / 3"));
    QVERIFY(!next->isEnabled());
    previous->click();
    QCOMPARE(pages.at(2).at(0).toInt(), 30);
    dialog.setResults({}, 0, QStringLiteral("另一首"));
    QCOMPARE(page->text(), QStringLiteral("1 / 1"));
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(!previous->isEnabled());
    QVERIFY(!next->isEnabled());
}

QTEST_MAIN(NetEaseDialogsTest)
#include "netease_dialogs_test.moc"
