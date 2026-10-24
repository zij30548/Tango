#include <QtTest>
#include <QTimer>
#include "lyricswidget.h"

class LyricsTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesUtf8MultiTimestampAndOffset();
    void mergesTranslationsAndRejectsMalformedTimes();
    void mergesDenseTimestampsWithoutLosingOrderOrEmptyCues();
    void seeksForwardAndBackwardAtExactBoundaries();
    void widgetsStayInSyncAndClickToSeek();
    void animationOnlyWhilePlayingAndVisible();
    void parsesWordTimesAndKeepsGaps();
    void onlineFallbackAndTranslations();
    void floatingBackgroundIsTransparentEvenWithGlobalStyle();
    void visualWordProgressChangesPixels();
    void floatingCloseHidesAndCanReopen();
};

void LyricsTest::parsesUtf8MultiTimestampAndOffset()
{
    const auto document = LyricsDocument::fromLrc(QString::fromUtf8(
        "\xEF\xBB\xBF[ti:测试歌曲]\r\n"
        "[00:12.34][00:03.2]夜色温柔\r\n"
        "[00:01.005]第一句\n[offset:-200]\n"));
    QCOMPARE(document.lines().size(), 3);
    QCOMPARE(document.lines().at(0).timeMs, qint64(805));
    QCOMPARE(document.lines().at(0).text, QStringLiteral("第一句"));
    QCOMPARE(document.lines().at(1).timeMs, qint64(3000));
    QCOMPARE(document.lines().at(1).text, QStringLiteral("夜色温柔"));
    QCOMPARE(document.lines().at(2).timeMs, qint64(12140));
    QCOMPARE(document.indexAt(804), -1);
    QCOMPARE(document.indexAt(805), 0);

    const auto positive = LyricsDocument::fromLrc(
        QStringLiteral("[offset:+150]\n[00:00.2]Start\n[00:01:250]Next"));
    QCOMPARE(positive.lines().at(0).timeMs, qint64(350));
    QCOMPARE(positive.lines().at(1).timeMs, qint64(1400));
}

void LyricsTest::mergesTranslationsAndRejectsMalformedTimes()
{
    const auto document = LyricsDocument::fromLrc(QStringLiteral(
        "[00:02]Hello\n[00:02.000]你好\n[00:02]Hello\n"
        "[00:99]Invalid seconds\n[bad]Invalid\n"
        "[999999999999999999999999:03]Overflow\n[00:04.250]\n"));
    QCOMPARE(document.lines().size(), 2);
    QCOMPARE(document.lines().first().text, QStringLiteral("Hello\n你好"));
    QCOMPARE(document.lines().last().text, QString());
    QCOMPARE(document.lines().last().timeMs, qint64(4250));
    QVERIFY(LyricsDocument::fromLrc(QStringLiteral("[ti:No timestamps]\nPlain text")).isEmpty());

    const auto clamped = LyricsDocument::fromLrc(
        QStringLiteral("[offset:-5000]\n[00:00.5]Early\n[00:06]Later"));
    QCOMPARE(clamped.lines().first().timeMs, qint64(0));
    QCOMPARE(clamped.lines().last().timeMs, qint64(1000));
}

void LyricsTest::mergesDenseTimestampsWithoutLosingOrderOrEmptyCues()
{
    QStringList source{QStringLiteral("[00:00]"), QStringLiteral("[00:01]中间句")};
    QStringList expected;
    expected.reserve(12000);
    for (int index = 0; index < 12000; ++index) {
        const QString text = QStringLiteral("歌词 %1").arg(index);
        expected.append(text);
        source.append(QStringLiteral("[00:00]") + text);
        if (index % 7 == 0) source.append(QStringLiteral("[00:00]") + text);
    }
    source.append({QStringLiteral("[00:00]"), QStringLiteral("[00:02]"),
                   QStringLiteral("[00:02]")});
    const auto document = LyricsDocument::fromLrc(source.join(QLatin1Char('\n')));
    QCOMPARE(document.lines().size(), 3);
    QCOMPARE(document.lines().at(0).timeMs, qint64(0));
    QCOMPARE(document.lines().at(0).text, expected.join(QLatin1Char('\n')));
    QCOMPARE(document.lines().at(1).text, QStringLiteral("中间句"));
    QCOMPARE(document.lines().at(2).timeMs, qint64(2000));
    QVERIFY(document.lines().at(2).text.isEmpty());
    QCOMPARE(document.indexAt(2000), 2);
}

void LyricsTest::seeksForwardAndBackwardAtExactBoundaries()
{
    const auto document = LyricsDocument::fromLrc(
        QStringLiteral("[00:10]First\n[00:20]Second\n[00:30]Third"));
    QCOMPARE(document.indexAt(0), -1);
    QCOMPARE(document.indexAt(9999), -1);
    QCOMPARE(document.indexAt(10000), 0);
    QCOMPARE(document.indexAt(19999), 0);
    QCOMPARE(document.indexAt(20000), 1);
    QCOMPARE(document.indexAt(999999), 2);
    QCOMPARE(document.indexAt(15000), 0);
    QCOMPARE(document.indexAt(-10), -1);
    QCOMPARE(LyricsDocument().indexAt(0), -1);
}

void LyricsTest::widgetsStayInSyncAndClickToSeek()
{
    LyricsView embedded;
    FloatingLyricsWidget floating;
    embedded.resize(380, 370);
    embedded.setLyrics(QStringLiteral("[00:01]First\n[00:02]Second\n[00:03]Third"));
    floating.setDocument(embedded.document());
    embedded.setPosition(2000);
    floating.setPosition(2000);
    QCOMPARE(embedded.currentLineIndex(), 1);
    QCOMPARE(floating.currentLineIndex(), 1);
    embedded.setPosition(500);
    floating.setPosition(500);
    QCOMPARE(embedded.currentLineIndex(), -1);
    QCOMPARE(floating.currentLineIndex(), -1);

    embedded.setPosition(1000);
    QSignalSpy seekSpy(&embedded, &LyricsView::seekRequested);
    // The current row sits at center y = 30 + (height - 30) / 2.
    QTest::mouseClick(&embedded, Qt::LeftButton, Qt::NoModifier, QPoint(50, 264));
    QCOMPARE(seekSpy.count(), 1);
    QCOMPARE(seekSpy.takeFirst().first().toLongLong(), qint64(2000));

    embedded.setLyrics(QString());
    floating.setLyrics(QString());
    QCOMPARE(embedded.currentLineIndex(), -1);
    QCOMPARE(floating.currentLineIndex(), -1);
    QTest::mouseClick(&embedded, Qt::LeftButton, Qt::NoModifier, QPoint(50, 200));
    QCOMPARE(seekSpy.count(), 0);
}

class PaintCounter : public QObject
{
public:
    int paints = 0;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Paint)
            ++paints;
        return QObject::eventFilter(watched, event);
    }
};

void LyricsTest::animationOnlyWhilePlayingAndVisible()
{
    LyricsView embedded;
    FloatingLyricsWidget floating;
    PaintCounter counter;
    embedded.installEventFilter(&counter);
    embedded.setLyrics(QStringLiteral("[00:00]First\n[00:10]Second"));
    embedded.show();
    QTest::qWait(30);
    const auto *timer = embedded.findChild<QTimer *>(QStringLiteral("lyricsAnimationTimer"));
    QVERIFY(timer);
    QVERIFY(!timer->isActive());
    counter.paints = 0;
    QTest::qWait(80);
    QCOMPARE(counter.paints, 0);
    embedded.setPlaying(true);
    QVERIFY(timer->isActive());
    QTest::qWait(90);
    QVERIFY(counter.paints > 0);
    embedded.setPlaying(false);
    QVERIFY(!timer->isActive());
    counter.paints = 0;
    QTest::qWait(80);
    QVERIFY(counter.paints <= 1); // one queued pause paint, no sustained animation
    embedded.setPlaying(true);
    embedded.hide();
    QVERIFY(!timer->isActive());
    embedded.show();
    QVERIFY(timer->isActive());
    embedded.setLyrics(QString());
    QVERIFY(!timer->isActive());
    embedded.setLyrics(QStringLiteral("[00:00]First\n[00:10]Second"));
    embedded.setPosition(10000);
    QCoreApplication::processEvents();
    QCOMPARE(embedded.currentLineIndex(), 1);
    QVERIFY(!timer->isActive()); // Last ordinary LRC cue has no invented duration.

    floating.setLyrics(QStringLiteral("[00:00]First\n[00:10]Second"));
    floating.setPlaying(true);
    const auto *desktopTimer = floating.findChild<QTimer *>();
    QVERIFY(desktopTimer && !desktopTimer->isActive());
    floating.show();
    QVERIFY(desktopTimer->isActive());
    floating.hide();
    QVERIFY(!desktopTimer->isActive());
}

void LyricsTest::parsesWordTimesAndKeepsGaps()
{
    const auto document = LyricsDocument::fromYrc(QStringLiteral(
        "{\"t\":0,\"c\":[{\"tx\":\"metadata\"}]}\n"
        "[1000,3000](1000,500,0)你(2000,1000,0)好\n"
        "[5000,1000](5000,1000,0)World\n"
        "[7000,100](7000,0,0)Invalid\n"
        "[8000,100](8000,200,0)Out of line\n"
        "[9000,1000](9000,800,0)Over(9500,500,0)lap\n"
        "[99999999999999999999999,1](1,1,0)Overflow"));
    QCOMPARE(document.lines().size(), 2);
    QVERIFY(document.hasWordTiming());
    QCOMPARE(document.lines().at(0).words.size(), 2);
    QCOMPARE(document.lines().at(0).text, QStringLiteral("你好"));
    QCOMPARE(document.lines().at(0).words.at(1).startMs, qint64(2000));
    QCOMPARE(document.lineProgress(0, 900), qreal(0));
    QCOMPARE(document.lineProgress(0, 1250), qreal(.25));
    QCOMPARE(document.lineProgress(0, 1750), qreal(.5)); // real pause between words
    QCOMPARE(document.lineProgress(0, 2500), qreal(.75));
    QCOMPARE(document.lineProgress(0, 4000), qreal(1));
    QCOMPARE(document.lineEnd(0), qint64(4000));
    QCOMPARE(document.indexAt(999), -1);
    QCOMPARE(document.indexAt(5000), 1);
    QCOMPARE(document.lineEnd(1), qint64(6000));
}

void LyricsTest::onlineFallbackAndTranslations()
{
    const QString lrc = QStringLiteral("[00:01]Hello\n[00:01]你好\n[00:05]Next");
    const auto fallback = LyricsDocument::fromOnlineLyrics(lrc, QStringLiteral("bad YRC"));
    QVERIFY(!fallback.hasWordTiming());
    QCOMPARE(fallback.lines().first().text, QStringLiteral("Hello\n你好"));
    QCOMPARE(fallback.lineProgress(0, 3000), qreal(.5));
    QCOMPARE(fallback.lineProgress(1, 5000), qreal(1));
    const auto timed = LyricsDocument::fromOnlineLyrics(lrc,
        QStringLiteral("[1000,1000](1000,1000,0)Hello"));
    QCOMPARE(timed.lines().first().text, QStringLiteral("Hello\n你好"));
    QCOMPARE(timed.lines().first().words.first().text, QStringLiteral("Hello"));
    QVERIFY(timed.hasWordTiming());
    QVERIFY(!LyricsDocument::fromYrc(QStringLiteral("[0,100](0,100,0)")).hasWordTiming());
}

void LyricsTest::floatingBackgroundIsTransparentEvenWithGlobalStyle()
{
    const QString original = qApp->styleSheet();
    qApp->setStyleSheet(QStringLiteral("QWidget { background: #182d20; }"));
    FloatingLyricsWidget floating;
    floating.setLyrics(QStringLiteral("[00:00]Transparent lyrics\n[00:04]Next"));
    floating.show();
    QTest::qWait(20);
    QVERIFY(floating.testAttribute(Qt::WA_TranslucentBackground));
    QVERIFY(floating.testAttribute(Qt::WA_NoSystemBackground));
    QVERIFY(floating.windowFlags().testFlag(Qt::FramelessWindowHint));
    QCOMPARE(floating.size(), QSize(620, 105));
    const QImage image = floating.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    QCOMPARE(qAlpha(image.pixel(0, 0)), 0);
    QCOMPARE(qAlpha(image.pixel(image.width() / 2, image.height() - 2)), 0);
    QCOMPARE(qAlpha(image.pixel(8, image.height() / 2)), 0);
    qApp->setStyleSheet(original);
}

void LyricsTest::visualWordProgressChangesPixels()
{
    FloatingLyricsWidget floating;
    floating.setDocument(LyricsDocument::fromYrc(QStringLiteral(
        "[1000,3000](1000,1000,0)逐字(2500,1000,0)歌词")));
    floating.setPosition(1000);
    floating.show();
    QTest::qWait(20);
    const QImage before = floating.grab().toImage();
    floating.setPosition(2800);
    const QImage after = floating.grab().toImage();
    QVERIFY(before != after);
    QVERIFY(floating.toolTip().contains(QStringLiteral("逐字")));
    floating.setPlaying(true);
    QVERIFY(floating.findChild<QTimer *>()->isActive());
    floating.setPosition(4100);
    QVERIFY(!floating.findChild<QTimer *>()->isActive());
    floating.setLyrics(QStringLiteral("[00:01]逐行\n[00:05]估算"));
    QVERIFY(floating.toolTip().contains(QStringLiteral("估算")));
    floating.setPlaying(true);
    floating.setPosition(5500);
    QVERIFY(!floating.findChild<QTimer *>()->isActive());
}

void LyricsTest::floatingCloseHidesAndCanReopen()
{
    FloatingLyricsWidget floating;
    QSignalSpy visibility(&floating, &FloatingLyricsWidget::visibilityChanged);
    floating.show();
    QVERIFY(floating.isVisible());
    QCOMPARE(visibility.count(), 1);
    floating.close();
    QVERIFY(!floating.isVisible());
    QCOMPARE(visibility.count(), 2);
    QCOMPARE(visibility.last().first().toBool(), false);
    floating.show();
    QVERIFY(floating.isVisible());
    QTest::keyClick(&floating, Qt::Key_Escape);
    QVERIFY(!floating.isVisible());
    floating.show();
    QTest::mouseClick(&floating, Qt::LeftButton, Qt::NoModifier,
                      QPoint(floating.width() - 29, 25));
    QVERIFY(!floating.isVisible());
}

QTEST_MAIN(LyricsTest)
#include "lyrics_test.moc"
