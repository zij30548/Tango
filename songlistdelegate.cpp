#include "songlistdelegate.h"
#include "coverartcache.h"
#include <QHeaderView>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QTableView>

SongListDelegate::SongListDelegate(int column, QObject *parent, CoverArtCache *cache)
    : QStyledItemDelegate(parent), titleColumn(column),
      artwork(cache ? cache : CoverArtCache::instance())
{
}

void SongListDelegate::install(QTableView *view, int titleColumn, CoverArtCache *artwork)
{
    if (!view)
        return;
    auto *delegate = new SongListDelegate(titleColumn, view, artwork);
    view->setItemDelegate(delegate);
    view->setShowGrid(false);
    view->setAlternatingRowColors(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setMouseTracking(true);
    view->viewport()->setMouseTracking(true);
    // The delegate paints the row once; disable the inherited square selection layer.
    const QString selectionStyle = QStringLiteral(
        "\n/* artwork row selection */\n"
        "QTableView { selection-background-color: transparent; }\n"
        "QTableView::item:selected, QTableView::item:hover { background: transparent; }\n");
    if (!view->styleSheet().contains(QStringLiteral("/* artwork row selection */")))
        view->setStyleSheet(view->styleSheet() + selectionStyle);
    view->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    view->verticalHeader()->setDefaultSectionSize(72);
    view->verticalHeader()->setMinimumSectionSize(72);
    view->verticalHeader()->hide();
    QObject::connect(delegate->artwork, &CoverArtCache::imageReady,
                     view->viewport(), QOverload<>::of(&QWidget::update));
}

void SongListDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                             const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setClipRect(option.rect);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const auto *view = qobject_cast<QTableView *>(parent());
    if (selected || hovered)
    {
        const QRect row(6, option.rect.top() + 3,
                        view ? view->viewport()->width() - 12 : option.rect.width(),
                        option.rect.height() - 6);
        painter->setPen(Qt::NoPen);
        painter->setBrush(selected ? QColor(111, 183, 151, 34) : QColor(229, 243, 235, 9));
        painter->drawRoundedRect(row, 10, 10);
    }
    if (index.column() != titleColumn)
    {
        QFont font = option.font;
        font.setPixelSize(12);
        painter->setFont(font);
        painter->setPen(selected ? QColor("#b9d1c5") : QColor("#93a69d"));
        auto alignment = Qt::Alignment(index.data(Qt::TextAlignmentRole).toInt());
        if (!alignment)
            alignment = Qt::AlignLeft | Qt::AlignVCenter;
        const QRect text = option.rect.adjusted(12, 0, -12, 0);
        painter->drawText(text, alignment,
                          QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight,
                                                        qMax(0, text.width())));
        painter->restore();
        return;
    }
    const QRectF cover(option.rect.left() + 12, option.rect.center().y() - 24, 48, 48);
    QPainterPath clip;
    clip.addRoundedRect(cover, 9, 9);
    const QImage image = artwork->image(
        index.data(MediaUrlRole).toUrl(), index.data(SongIdRole).toLongLong(),
        index.data(CoverUrlRole).toString());
    painter->save();
    painter->setClipPath(clip, Qt::IntersectClip);
    if (image.isNull())
    {
        QLinearGradient gradient(cover.topLeft(), cover.bottomRight());
        gradient.setColorAt(0, QColor("#2c423b"));
        gradient.setColorAt(1, QColor("#1d2c29"));
        painter->fillRect(cover, gradient);
        painter->setPen(QPen(QColor(170, 203, 185, 58), 1.2));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(cover.center(), 13, 13);
        painter->drawEllipse(cover.center(), 4, 4);
        painter->setPen(QPen(QColor(170, 203, 185, 28), 1));
        painter->drawArc(cover.adjusted(6, 6, -6, -6), 40 * 16, 90 * 16);
    }
    else
    {
        const QSizeF size = image.size();
        const qreal side = qMin(size.width(), size.height());
        const QRectF crop((size.width() - side) / 2, (size.height() - side) / 2, side, side);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->drawImage(cover, image, crop);
    }
    painter->restore();
    painter->setPen(QPen(QColor(211, 234, 221, 15), 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(cover.adjusted(.5, .5, -.5, -.5), 9, 9);
    const int textLeft = int(cover.right()) + 14;
    const int textWidth = qMax(0, option.rect.right() - textLeft - 12);
    QFont primary = option.font;
    primary.setPixelSize(14);
    primary.setWeight(QFont::Medium);
    painter->setFont(primary);
    painter->setPen(selected ? QColor("#e8f4ed") : QColor("#dce9e2"));
    painter->drawText(QRect(textLeft, option.rect.center().y() - 21, textWidth, 23),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(primary).elidedText(index.data().toString(), Qt::ElideRight,
                                                       textWidth));
    QFont secondary = option.font;
    secondary.setPixelSize(12);
    painter->setFont(secondary);
    painter->setPen(selected ? QColor("#9db9aa") : QColor("#82998d"));
    QString subtitle = index.data(ArtistRole).toString();
    if (subtitle.isEmpty())
        subtitle = index.data(MediaUrlRole).toUrl().isLocalFile() ? tr("本地音乐") : tr("音乐");
    painter->drawText(QRect(textLeft, option.rect.center().y() + 3, textWidth, 19),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(secondary).elidedText(subtitle, Qt::ElideRight, textWidth));
    painter->restore();
}

QSize SongListDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize hint = QStyledItemDelegate::sizeHint(option, index);
    hint.setHeight(72);
    if (index.column() == titleColumn)
        hint.setWidth(qMax(hint.width() + 76, 220));
    return hint;
}
