#ifndef LYRICSWIDGET_H
#define LYRICSWIDGET_H

#include <QVector>
#include <QWidget>
#include <QElapsedTimer>

class QCloseEvent;
class QHideEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;
class QTimer;

struct LyricWord
{
    qint64 startMs = 0;
    qint64 durationMs = 0;
    QString text;
};

struct LyricLine
{
    qint64 timeMs = 0;
    QString text;
    QVector<LyricWord> words;
    qint64 durationMs = 0;
};

// A small, implicitly shared value. Parse once and pass the same document to
// both widgets; position changes only perform a binary search.
class LyricsDocument
{
public:
    static LyricsDocument fromLrc(const QString &text);
    static LyricsDocument fromYrc(const QString &text);
    static LyricsDocument fromOnlineLyrics(const QString &lrc, const QString &yrc);
    const QVector<LyricLine> &lines() const { return m_lines; }
    bool isEmpty() const { return m_lines.isEmpty(); }
    int indexAt(qint64 positionMs) const;
    bool hasWordTiming() const;
    // Character-width progress, using genuine word timestamps when present.
    // Ordinary LRC has only line timestamps, so its intra-line sweep is an estimate.
    qreal lineProgress(int index, qint64 positionMs) const;
    qint64 lineEnd(int index) const;

private:
    QVector<LyricLine> m_lines;
};

class LyricsView : public QWidget
{
    Q_OBJECT

public:
    explicit LyricsView(QWidget *parent = nullptr);
    const LyricsDocument &document() const { return m_document; }
    int currentLineIndex() const { return m_currentLine; }
    QSize sizeHint() const override;

public slots:
    void setLyrics(const QString &lrc);
    void setDocument(const LyricsDocument &document);
    void setPosition(qint64 positionMs);
    void setTrackTitle(const QString &title);
    void setPlaying(bool playing);

signals:
    void seekRequested(qint64 positionMs);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct VisibleRow {
        int index;
        QRectF rect;
    };
    QVector<VisibleRow> visibleRows() const;
    int rowAt(const QPoint &point) const;
    qint64 playbackPosition() const;
    void refreshPosition();
    void updateAnimation();
    LyricsDocument m_document;
    QString m_trackTitle;
    qint64 m_position = 0;
    int m_currentLine = -1;
    bool m_playing = false;
    QElapsedTimer m_elapsed;
    QTimer *m_animationTimer = nullptr;
};

class FloatingLyricsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit FloatingLyricsWidget(QWidget *parent = nullptr);
    const LyricsDocument &document() const { return m_document; }
    int currentLineIndex() const { return m_currentLine; }
    QSize sizeHint() const override;

public slots:
    void setLyrics(const QString &lrc);
    void setDocument(const LyricsDocument &document);
    void setPosition(qint64 positionMs);
    void setTrackTitle(const QString &title);
    void setPlaying(bool playing);

signals:
    void visibilityChanged(bool visible);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void enterEvent(QEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRect closeButtonRect() const;
    qint64 playbackPosition() const;
    void refreshPosition();
    void updateAnimation();
    LyricsDocument m_document;
    QString m_trackTitle;
    qint64 m_position = 0;
    int m_currentLine = -1;
    QPoint m_dragOffset;
    bool m_dragging = false;
    bool m_hovered = false;
    bool m_playing = false;
    QElapsedTimer m_elapsed;
    QTimer *m_animationTimer = nullptr;
};

#endif // LYRICSWIDGET_H
