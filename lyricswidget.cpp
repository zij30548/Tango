#include "lyricswidget.h"

#include <QCloseEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSet>
#include <QShowEvent>
#include <QTimer>
#include <algorithm>
#include <limits>

namespace {

QFont lyricFont(const QWidget *widget, int pixelSize, int weight = QFont::Normal)
{
    QFont font = widget->font();
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    return font;
}

QString singleLine(const QString &text)
{
    QString result = text;
    result.replace(QLatin1Char('\n'), QStringLiteral("  ·  "));
    return result;
}

QString timingDescription(const LyricsDocument &document)
{
    return document.hasWordTiming()
        ? QStringLiteral("逐字歌词，按歌曲提供的字词时间同步")
        : QStringLiteral("逐行歌词，行内进度为估算");
}

qreal highlightedWidth(const LyricLine &line, const QFontMetricsF &metrics,
                       qint64 position, qreal estimatedProgress, qreal textWidth)
{
    if (line.words.isEmpty())
        return textWidth * estimatedProgress;
    QString completed;
    for (const auto &word : line.words) {
        const qreal before = metrics.horizontalAdvance(completed);
        if (position <= word.startMs)
            return before;
        completed += word.text;
        const qreal after = metrics.horizontalAdvance(completed);
        if (position < word.startMs + word.durationMs)
            return before + (after - before) * qreal(position - word.startMs) / word.durationMs;
    }
    return metrics.horizontalAdvance(completed);
}

void paintLyric(QPainter &painter, const QRectF &rect, const QString &text,
                const QFont &font, const LyricLine *line, qint64 position,
                qreal progress, bool centered, bool desktop)
{
    const QFontMetricsF metrics(font);
    const QString displayed = metrics.elidedText(text, Qt::ElideRight, rect.width());
    const qreal textWidth = metrics.horizontalAdvance(displayed);
    const qreal x = centered ? rect.center().x() - textWidth / 2 : rect.left();
    const qreal y = rect.center().y() + (metrics.ascent() - metrics.descent()) / 2;
    QPainterPath glyphs;
    glyphs.addText(QPointF(x, y), font, displayed);
    if (desktop) {
        painter.save();
        painter.translate(0, 2);
        painter.strokePath(glyphs, QPen(QColor(0, 0, 0, 170), 4, Qt::SolidLine,
                                      Qt::RoundCap, Qt::RoundJoin));
        painter.restore();
        painter.strokePath(glyphs, QPen(QColor(0, 0, 0, 205), 1.8, Qt::SolidLine,
                                      Qt::RoundCap, Qt::RoundJoin));
    }
    painter.fillPath(glyphs, desktop ? QColor("#f1f4ef") : QColor("#bcc9c1"));
    if (!line) return;
    const qreal filled = qBound(qreal(0), highlightedWidth(*line, metrics, position,
                                                           progress, textWidth), textWidth);
    painter.save();
    painter.setClipRect(QRectF(x - 1, rect.top(), filled + (filled > 0 ? 1 : 0), rect.height()));
    painter.fillPath(glyphs, QColor("#b9edb1"));
    painter.restore();
}

} // namespace

LyricsDocument LyricsDocument::fromLrc(const QString &text)
{
    LyricsDocument result;
    // LRC offsets apply to the entire file, including tags appearing before
    // the offset declaration. Accept the common dot and colon fractions.
    static const QRegularExpression offsetPattern(
        QStringLiteral("\\[offset:\\s*([+-]?\\d+)\\s*\\]"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression timePattern(
        QStringLiteral("^\\[(\\d+):(\\d{1,2})(?:[.:](\\d{1,3}))?\\]"));
    qint64 offset = 0;
    auto offsetMatches = offsetPattern.globalMatch(text);
    while (offsetMatches.hasNext()) {
        bool ok = false;
        const qint64 value = offsetMatches.next().captured(1).toLongLong(&ok);
        if (ok)
            offset = value;
    }

    for (QString rawLine : text.split(QLatin1Char('\n'))) {
        rawLine.remove(QChar(0xfeff));
        QString remaining = rawLine.trimmed();
        QVector<qint64> timestamps;
        while (true) {
            const auto match = timePattern.match(remaining);
            if (!match.hasMatch())
                break;
            bool minutesOk = false;
            const qint64 minutes = match.captured(1).toLongLong(&minutesOk);
            const int seconds = match.captured(2).toInt();
            const QString fraction = match.captured(3);
            const int milliseconds = fraction.isEmpty()
                ? 0 : fraction.leftJustified(3, QLatin1Char('0')).toInt();
            // Leave enough headroom for seconds, fractions, and the offset.
            if (minutesOk && seconds < 60
                && minutes <= (std::numeric_limits<qint64>::max() - 60000) / 60000) {
                const qint64 base = minutes * 60000 + seconds * 1000 + milliseconds;
                qint64 adjusted;
                if (offset > 0 && base > std::numeric_limits<qint64>::max() - offset)
                    adjusted = std::numeric_limits<qint64>::max();
                else if (offset < 0 && offset < -base)
                    adjusted = 0;
                else
                    adjusted = base + offset;
                timestamps.append(adjusted);
            }
            remaining = remaining.mid(match.capturedLength()).trimmed();
        }
        for (qint64 timestamp : timestamps)
            result.m_lines.append({timestamp, remaining, {}, 0});
    }

    std::stable_sort(result.m_lines.begin(), result.m_lines.end(),
                     [](const LyricLine &a, const LyricLine &b) {
        return a.timeMs < b.timeMs;
    });
    // Translation lines often share a timestamp with their original. Preserve
    // both rather than allowing binary search to silently discard one.
    QVector<LyricLine> merged;
    merged.reserve(result.m_lines.size());
    for (int first = 0; first < result.m_lines.size();) {
        const qint64 timestamp = result.m_lines.at(first).timeMs;
        QStringList texts;
        QSet<QString> seen;
        int next = first;
        while (next < result.m_lines.size() && result.m_lines.at(next).timeMs == timestamp) {
            const QString &text = result.m_lines.at(next).text;
            if (!text.isEmpty() && !seen.contains(text)) {
                seen.insert(text);
                texts.append(text);
            }
            ++next;
        }
        // Preserve first appearance order, and allocate the combined text once.
        // Repeatedly splitting the accumulated text makes dense LRC files quadratic.
        merged.append({timestamp, texts.join(QLatin1Char('\n')), {}, 0});
        first = next;
    }
    result.m_lines.swap(merged);
    return result;
}

LyricsDocument LyricsDocument::fromYrc(const QString &text)
{
    LyricsDocument result;
    // Public upstream format: [lineStart,lineDuration](wordStart,wordDuration,0)text.
    // Reject unreasonable or ambiguous timing rather than inventing word times.
    if (text.size() > 2 * 1024 * 1024) return result;
    static const QRegularExpression header(QStringLiteral("^\\[(\\d+),(\\d+)\\]"));
    static const QRegularExpression token(QStringLiteral("\\((\\d+),(\\d+),\\d+\\)"));
    constexpr qint64 maxTime = 7LL * 24 * 60 * 60 * 1000;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        if (result.m_lines.size() >= 20000) break;
        const QString source = raw.trimmed();
        if (source.size() > 16384) continue;
        const auto match = header.match(source);
        if (!match.hasMatch()) continue;
        bool startOk = false, durationOk = false;
        const qint64 start = match.captured(1).toLongLong(&startOk);
        const qint64 duration = match.captured(2).toLongLong(&durationOk);
        if (!startOk || !durationOk || start < 0 || duration <= 0
            || start > maxTime || duration > maxTime - start) continue;
        const QString body = source.mid(match.capturedLength());
        auto matches = token.globalMatch(body);
        QVector<QRegularExpressionMatch> tokens;
        while (matches.hasNext() && tokens.size() < 513) tokens.append(matches.next());
        if (tokens.isEmpty() || tokens.size() > 512 || tokens.first().capturedStart() != 0)
            continue;
        LyricLine line;
        line.timeMs = start;
        line.durationMs = duration;
        qint64 previousEnd = start;
        bool valid = true;
        for (int index = 0; index < tokens.size(); ++index) {
            const auto &wordToken = tokens.at(index);
            bool wordStartOk = false, wordDurationOk = false;
            const qint64 wordStart = wordToken.captured(1).toLongLong(&wordStartOk);
            const qint64 wordDuration = wordToken.captured(2).toLongLong(&wordDurationOk);
            const int next = index + 1 < tokens.size()
                ? tokens.at(index + 1).capturedStart() : body.size();
            const QString wordText = body.mid(wordToken.capturedEnd(), next - wordToken.capturedEnd());
            if (!wordStartOk || !wordDurationOk || wordStart < previousEnd
                || wordStart > start + duration || wordDuration <= 0
                || wordDuration > start + duration - wordStart || wordText.isEmpty()) {
                valid = false;
                break;
            }
            line.words.append({wordStart, wordDuration, wordText});
            line.text += wordText;
            previousEnd = wordStart + wordDuration;
        }
        if (valid && !line.text.trimmed().isEmpty()) result.m_lines.append(line);
    }
    std::stable_sort(result.m_lines.begin(), result.m_lines.end(),
                     [](const LyricLine &a, const LyricLine &b) { return a.timeMs < b.timeMs; });
    // Duplicate cues cannot describe two independently highlighted sentences.
    result.m_lines.erase(std::unique(result.m_lines.begin(), result.m_lines.end(),
                         [](const LyricLine &a, const LyricLine &b) {
        return a.timeMs == b.timeMs;
    }), result.m_lines.end());
    return result;
}

LyricsDocument LyricsDocument::fromOnlineLyrics(const QString &lrc, const QString &yrc)
{
    LyricsDocument timed = fromYrc(yrc);
    if (timed.isEmpty()) return fromLrc(lrc);
    const auto ordinary = fromLrc(lrc);
    // Preserve translations already merged by the LRC parser, only when the
    // original sentence matches. A different rendition is never paired blindly.
    for (auto &line : timed.m_lines) {
        int index = ordinary.indexAt(line.timeMs + 1000);
        for (; index >= 0 && ordinary.lines().at(index).timeMs >= line.timeMs - 1000; --index) {
            const QStringList parts = ordinary.lines().at(index).text.split(QLatin1Char('\n'));
            if (!parts.contains(line.text.trimmed())) continue;
            for (const QString &part : parts)
                if (part != line.text.trimmed() && !part.isEmpty()) line.text += QLatin1Char('\n') + part;
            break;
        }
    }
    return timed;
}

bool LyricsDocument::hasWordTiming() const
{
    return !m_lines.isEmpty() && !m_lines.first().words.isEmpty();
}

qint64 LyricsDocument::lineEnd(int index) const
{
    if (index < 0 || index >= m_lines.size()) return 0;
    const auto &line = m_lines.at(index);
    qint64 end = line.durationMs > 0 ? line.timeMs + line.durationMs
                                    : (index + 1 < m_lines.size() ? m_lines.at(index + 1).timeMs : line.timeMs);
    if (index + 1 < m_lines.size()) end = qMin(end, m_lines.at(index + 1).timeMs);
    return end;
}

qreal LyricsDocument::lineProgress(int index, qint64 positionMs) const
{
    if (index < 0 || index >= m_lines.size()) return 0;
    const auto &line = m_lines.at(index);
    if (positionMs < line.timeMs) return 0;
    if (line.words.isEmpty()) {
        const qint64 end = lineEnd(index);
        // Without a following cue, retain ordinary whole-line highlighting;
        // never invent a duration or keep an animation timer alive forever.
        return end > line.timeMs ? qBound(qreal(0), qreal(positionMs - line.timeMs) / (end - line.timeMs), qreal(1)) : 1;
    }
    qreal complete = 0;
    int total = 0;
    for (const auto &word : line.words) {
        total += word.text.size();
        if (positionMs >= word.startMs + word.durationMs) complete += word.text.size();
        else if (positionMs > word.startMs)
            complete += word.text.size() * qreal(positionMs - word.startMs) / word.durationMs;
    }
    return total ? qBound(qreal(0), complete / total, qreal(1)) : 0;
}

int LyricsDocument::indexAt(qint64 positionMs) const
{
    const auto after = std::upper_bound(m_lines.cbegin(), m_lines.cend(), positionMs,
                                       [](qint64 position, const LyricLine &line) {
        return position < line.timeMs;
    });
    return int(after - m_lines.cbegin()) - 1;
}

LyricsView::LyricsView(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(240, 220);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("同步歌词"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_animationTimer = new QTimer(this);
    m_animationTimer->setObjectName(QStringLiteral("lyricsAnimationTimer"));
    m_animationTimer->setInterval(33);
    connect(m_animationTimer, &QTimer::timeout, this, [this] {
        refreshPosition();
        updateAnimation();
        update();
    });
}

QSize LyricsView::sizeHint() const
{
    return QSize(380, 370);
}

void LyricsView::setLyrics(const QString &lrc)
{
    setDocument(LyricsDocument::fromLrc(lrc));
}

void LyricsView::setDocument(const LyricsDocument &document)
{
    m_document = document;
    setToolTip(timingDescription(document));
    m_currentLine = m_document.indexAt(m_position);
    setAccessibleDescription(m_currentLine >= 0
                                 ? m_document.lines().at(m_currentLine).text : QString());
    update();
    updateAnimation();
}

void LyricsView::setPosition(qint64 positionMs)
{
    m_position = positionMs;
    m_elapsed.restart();
    refreshPosition();
    updateAnimation();
    update();
}

qint64 LyricsView::playbackPosition() const
{
    return m_position + (m_playing && m_elapsed.isValid() ? m_elapsed.elapsed() : 0);
}

void LyricsView::refreshPosition()
{
    const int current = m_document.indexAt(playbackPosition());
    if (current == m_currentLine)
        return;
    m_currentLine = current;
    setAccessibleDescription(current >= 0 ? m_document.lines().at(current).text
                                         : QString());
    update();
}

void LyricsView::setPlaying(bool playing)
{
    if (m_playing == playing) return;
    m_position = playbackPosition();
    m_elapsed.restart();
    m_playing = playing;
    updateAnimation();
    update();
}

void LyricsView::updateAnimation()
{
    const bool animate = m_playing && isVisible() && !m_document.isEmpty()
        && m_currentLine >= 0 && playbackPosition() < m_document.lineEnd(m_currentLine);
    if (animate && !m_animationTimer->isActive()) m_animationTimer->start();
    else if (!animate) m_animationTimer->stop();
}

void LyricsView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refreshPosition();
    updateAnimation();
}

void LyricsView::hideEvent(QHideEvent *event)
{
    m_animationTimer->stop();
    QWidget::hideEvent(event);
}

void LyricsView::setTrackTitle(const QString &title)
{
    if (m_trackTitle == title)
        return;
    m_trackTitle = title;
    if (m_document.isEmpty())
        update();
}

QVector<LyricsView::VisibleRow> LyricsView::visibleRows() const
{
    QVector<VisibleRow> rows;
    if (m_document.isEmpty() || m_currentLine < 0)
        return rows;
    const int pivot = qMax(0, m_currentLine);
    const bool compact = height() < 220;
    const int radius = compact ? 1 : 2;
    const qreal top = 36;
    const qreal available = qMax(0.0, height() - top - 4);
    const qreal spacing = qMin(compact ? 36.0 : 64.0,
                               available / (radius * 2 + 1));
    const qreal center = compact ? top + available / 2.0
                                : 30 + (height() - 30) / 2.0;
    for (int offset = -radius; offset <= radius; ++offset) {
        const int index = pivot + offset;
        const QRectF row(12, center + offset * spacing - spacing / 2,
                         width() - 24, spacing);
        if (index >= 0 && index < m_document.lines().size()
            && row.top() >= top - 0.5 && row.bottom() <= height() + 0.5)
            rows.append({index, row});
    }
    return rows;
}

void LyricsView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor("#69746f"));
    painter.setFont(lyricFont(this, 11, QFont::DemiBold));
    painter.drawText(QRect(16, 8, width() - 32, 24), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("歌词  /  LYRICS"));
    if (m_document.isEmpty() || m_currentLine < 0) {
        const bool compact = height() < 220;
        const qreal center = 36 + (height() - 36) / 2.0;
        painter.setFont(lyricFont(this, compact ? 18 : 20, QFont::DemiBold));
        painter.setPen(QColor("#d9e2dd"));
        painter.drawText(QRectF(16, center - 29, width() - 32, 30), Qt::AlignCenter,
                         m_document.isEmpty() ? QStringLiteral("暂无同步歌词")
                                              : QStringLiteral("前奏"));
        painter.setFont(lyricFont(this, compact ? 11 : 12));
        painter.setPen(QColor("#748079"));
        painter.drawText(QRectF(16, center + 8, width() - 32, compact ? 30 : 42),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                         m_document.isEmpty()
                             ? QStringLiteral("本地歌曲可添加同名 .lrc 文件")
                             : QStringLiteral("歌词将在第一句开始时显示"));
        return;
    }

    for (const VisibleRow &row : visibleRows()) {
        const bool active = row.index == m_currentLine;
        const int distance = qAbs(row.index - qMax(0, m_currentLine));
        const QString text = singleLine(m_document.lines().at(row.index).text);
        const bool compact = height() < 220;
        painter.setFont(lyricFont(this, active ? (compact ? 18 : 21) : (compact ? 14 : 16),
                                  active ? QFont::DemiBold : QFont::Normal));
        painter.setPen(active ? QColor("#c2ead5")
                              : (distance <= 1 ? QColor("#8c9991")
                                               : QColor("#505d55")));
        const QString displayed = text.isEmpty() ? QStringLiteral("♪") : text;
        if (active) {
            paintLyric(painter, row.rect, displayed, painter.font(),
                       &m_document.lines().at(row.index), playbackPosition(),
                       m_document.lineProgress(row.index, playbackPosition()), false, false);
        } else painter.drawText(row.rect, Qt::AlignVCenter | Qt::AlignLeft,
                         painter.fontMetrics().elidedText(displayed, Qt::ElideRight,
                                                          int(row.rect.width())));
    }
}

int LyricsView::rowAt(const QPoint &point) const
{
    for (const VisibleRow &row : visibleRows()) {
        if (row.rect.contains(point))
            return row.index;
    }
    return -1;
}

void LyricsView::mousePressEvent(QMouseEvent *event)
{
    const int index = rowAt(event->pos());
    if (event->button() == Qt::LeftButton && index >= 0) {
        emit seekRequested(m_document.lines().at(index).timeMs);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void LyricsView::mouseMoveEvent(QMouseEvent *event)
{
    setCursor(rowAt(event->pos()) >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(event);
}

void LyricsView::leaveEvent(QEvent *event)
{
    unsetCursor();
    QWidget::leaveEvent(event);
}

FloatingLyricsWidget::FloatingLyricsWidget(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAutoFillBackground(false);
    // The main window's QWidget rule otherwise gives this top-level tool the
    // application's opaque green background even with translucency enabled.
    setObjectName(QStringLiteral("floatingLyrics"));
    setStyleSheet(QStringLiteral("QWidget#floatingLyrics { background: transparent; border: none; }"));
    setMouseTracking(true);
    setWindowTitle(QStringLiteral("桌面歌词"));
    setAccessibleName(QStringLiteral("浮动桌面歌词，可拖动，按 Escape 隐藏"));
    setMinimumSize(400, 78);
    setMaximumSize(960, 150);
    m_animationTimer = new QTimer(this);
    m_animationTimer->setObjectName(QStringLiteral("lyricsAnimationTimer"));
    m_animationTimer->setInterval(33);
    connect(m_animationTimer, &QTimer::timeout, this, [this] {
        refreshPosition();
        updateAnimation();
        update();
    });
    resize(sizeHint());
}

QSize FloatingLyricsWidget::sizeHint() const
{
    return QSize(620, 105);
}

void FloatingLyricsWidget::setLyrics(const QString &lrc)
{
    setDocument(LyricsDocument::fromLrc(lrc));
}

void FloatingLyricsWidget::setDocument(const LyricsDocument &document)
{
    m_document = document;
    setToolTip(timingDescription(document) + QStringLiteral("；拖动移动，Escape 隐藏"));
    m_currentLine = m_document.indexAt(m_position);
    setAccessibleDescription(m_currentLine >= 0
                                 ? m_document.lines().at(m_currentLine).text : QString());
    update();
    updateAnimation();
}

void FloatingLyricsWidget::setPosition(qint64 positionMs)
{
    m_position = positionMs;
    m_elapsed.restart();
    refreshPosition();
    updateAnimation();
    update();
}

qint64 FloatingLyricsWidget::playbackPosition() const
{
    return m_position + (m_playing && m_elapsed.isValid() ? m_elapsed.elapsed() : 0);
}

void FloatingLyricsWidget::refreshPosition()
{
    const int current = m_document.indexAt(playbackPosition());
    if (current == m_currentLine)
        return;
    m_currentLine = current;
    setAccessibleDescription(current >= 0 ? m_document.lines().at(current).text
                                         : QString());
    update();
}

void FloatingLyricsWidget::setPlaying(bool playing)
{
    if (m_playing == playing) return;
    m_position = playbackPosition();
    m_elapsed.restart();
    m_playing = playing;
    updateAnimation();
    update();
}

void FloatingLyricsWidget::updateAnimation()
{
    const bool animate = m_playing && isVisible() && !m_document.isEmpty()
        && m_currentLine >= 0 && playbackPosition() < m_document.lineEnd(m_currentLine);
    if (animate && !m_animationTimer->isActive()) m_animationTimer->start();
    else if (!animate) m_animationTimer->stop();
}

void FloatingLyricsWidget::setTrackTitle(const QString &title)
{
    if (m_trackTitle == title)
        return;
    m_trackTitle = title;
    update();
}

QRect FloatingLyricsWidget::closeButtonRect() const
{
    return QRect(width() - 33, 3, 26, 24);
}

void FloatingLyricsWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing);
    // Paint only glyphs at rest. The desktop remains visible around and between
    // the letters; controls appear only while the user points at the lyrics.
    if (m_hovered || m_dragging) {
        const QRectF handle(10, 1, width() - 20, 27);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(15, 20, 17, 125));
        painter.drawRoundedRect(handle, 9, 9);
        painter.setFont(lyricFont(this, 10, QFont::DemiBold));
        painter.setPen(QColor("#d4ded8"));
        const QString title = m_trackTitle.isEmpty() ? QStringLiteral("拖动移动桌面歌词") : m_trackTitle;
        painter.drawText(QRect(23, 3, width() - 70, 23), Qt::AlignVCenter | Qt::AlignLeft,
                         painter.fontMetrics().elidedText(title, Qt::ElideRight, width() - 70));
        painter.setPen(QPen(QColor("#e2e9e5"), 1.3, Qt::SolidLine, Qt::RoundCap));
        const QPoint center = closeButtonRect().center();
        painter.drawLine(center + QPoint(-4, -4), center + QPoint(4, 4));
        painter.drawLine(center + QPoint(4, -4), center + QPoint(-4, 4));
    }

    QString current;
    QString next;
    if (m_document.isEmpty()) {
        current = QStringLiteral("暂无同步歌词");
        next = QStringLiteral("本地歌曲可添加同名 .lrc 文件");
    } else if (m_currentLine < 0) {
        current = QStringLiteral("前奏");
        next = QStringLiteral("歌词将在第一句开始时显示");
    } else {
        current = m_document.lines().at(m_currentLine).text;
        if (current.isEmpty())
            current = QStringLiteral("♪");
        if (m_currentLine + 1 < m_document.lines().size())
            next = m_document.lines().at(m_currentLine + 1).text;
    }
    const int horizontalPadding = 18;
    const int textWidth = width() - horizontalPadding * 2;
    const int lyricSize = width() < 450 ? 22 : 28;
    const LyricLine *line = m_currentLine >= 0 ? &m_document.lines().at(m_currentLine) : nullptr;
    const qreal top = height() < 90 ? 17 : 29;
    paintLyric(painter, QRectF(horizontalPadding, top, textWidth, 39), singleLine(current),
               lyricFont(this, lyricSize, QFont::DemiBold), line, playbackPosition(),
               m_document.lineProgress(m_currentLine, playbackPosition()), true, true);
    if (height() >= 90) {
        paintLyric(painter, QRectF(horizontalPadding, 73, textWidth, 23), singleLine(next),
                   lyricFont(this, 13), nullptr, 0, 0, true, true);
    }
}

void FloatingLyricsWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (closeButtonRect().contains(event->pos())) {
            close();
        } else {
            m_dragging = true;
            m_hovered = true;
            m_dragOffset = event->globalPos() - frameGeometry().topLeft();
            setCursor(Qt::ClosedHandCursor);
            update();
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void FloatingLyricsWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_hovered) {
        m_hovered = true;
        update();
    }
    if (m_dragging && event->buttons().testFlag(Qt::LeftButton)) {
        move(event->globalPos() - m_dragOffset);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void FloatingLyricsWidget::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    m_hovered = rect().contains(event->pos());
    unsetCursor();
    update();
    QWidget::mouseReleaseEvent(event);
}

void FloatingLyricsWidget::closeEvent(QCloseEvent *event)
{
    m_dragging = false;
    hide();
    event->ignore();
}

void FloatingLyricsWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        hide();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void FloatingLyricsWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refreshPosition();
    updateAnimation();
    emit visibilityChanged(true);
}

void FloatingLyricsWidget::hideEvent(QHideEvent *event)
{
    m_dragging = false;
    m_hovered = false;
    m_animationTimer->stop();
    unsetCursor();
    QWidget::hideEvent(event);
    emit visibilityChanged(false);
}

void FloatingLyricsWidget::enterEvent(QEvent *event)
{
    m_hovered = true;
    update();
    QWidget::enterEvent(event);
}

void FloatingLyricsWidget::leaveEvent(QEvent *event)
{
    m_hovered = false;
    update();
    QWidget::leaveEvent(event);
}
