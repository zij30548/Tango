#ifndef SONGLISTDELEGATE_H
#define SONGLISTDELEGATE_H

#include <QStyledItemDelegate>

class QTableView;
class CoverArtCache;

class SongListDelegate : public QStyledItemDelegate
{
    Q_OBJECT
  public:
    enum Role { ArtistRole = Qt::UserRole + 201, MediaUrlRole, SongIdRole, CoverUrlRole };
    explicit SongListDelegate(int titleColumn, QObject *parent = nullptr,
                              CoverArtCache *artwork = nullptr);
    static void install(QTableView *view, int titleColumn, CoverArtCache *artwork = nullptr);
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

  private:
    int titleColumn;
    CoverArtCache *artwork;
};
#endif
