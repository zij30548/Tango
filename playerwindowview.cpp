#include "playerwindowview.h"
#include "lyricswidget.h"
#include "coverartcache.h"
#include "songlistdelegate.h"
#include <QApplication>
#include <QBoxLayout>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSlider>
#include <QStackedWidget>
#include <QTableView>
#include <QTextLayout>

namespace
{

class ElidedLabel : public QLabel
{
  public:
    explicit ElidedLabel(const QString &text, QWidget *parent) : QLabel(text, parent)
    {
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.setFont(font());
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};

class TrackTitleLabel : public QLabel
{
  public:
    explicit TrackTitleLabel(const QString &text, QWidget *parent) : QLabel(text, parent) {}

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.setFont(font());
        const QFontMetrics metrics(font());
        const int lineHeight = metrics.lineSpacing();
        QTextLayout layout(text(), font());
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        QTextLine first = layout.createLine();
        if (!first.isValid())
        {
            layout.endLayout();
            return;
        }
        first.setLineWidth(width());
        QTextLine second = layout.createLine();
        if (second.isValid())
            second.setLineWidth(width());
        layout.endLayout();
        if (!second.isValid() || height() < lineHeight * 2)
        {
            painter.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                             metrics.elidedText(text(), Qt::ElideRight, width()));
            return;
        }
        first.draw(&painter, QPointF(0, 0));
        QString remaining = text().mid(second.textStart());
        remaining.replace(QLatin1Char('\n'), QLatin1Char(' '));
        painter.drawText(QRect(0, lineHeight, width(), lineHeight),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(remaining, Qt::ElideRight, width()));
    }
};

} // namespace

QIcon playerIcon(const QString &name, const QColor &color)
{
    QPixmap pix(48, 48);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    p.setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    if (name == "play")
    {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        path.moveTo(9, 5);
        path.lineTo(19, 12);
        path.lineTo(9, 19);
        path.closeSubpath();
    }
    else if (name == "pause")
    {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(7, 5, 3, 14), 1, 1);
        p.drawRoundedRect(QRectF(14, 5, 3, 14), 1, 1);
    }
    else if (name == "next" || name == "previous")
    {
        if (name == "previous")
        {
            p.translate(24, 0);
            p.scale(-1, 1);
        }
        path.moveTo(7, 5);
        path.lineTo(16, 12);
        path.lineTo(7, 19);
        path.closeSubpath();
        p.drawLine(18, 5, 18, 19);
    }
    else if (name == "plus")
    {
        p.drawLine(12, 5, 12, 19);
        p.drawLine(5, 12, 19, 12);
    }
    else if (name == "search")
    {
        p.drawEllipse(QRectF(4, 4, 11, 11));
        p.drawLine(14, 14, 20, 20);
    }
    else if (name == "heart")
    {
        path.moveTo(12, 20);
        path.cubicTo(9, 17, 3, 13, 3, 8);
        path.cubicTo(3, 2, 10, 2, 12, 7);
        path.cubicTo(14, 2, 21, 2, 21, 8);
        path.cubicTo(21, 13, 15, 17, 12, 20);
    }
    else if (name == "history")
    {
        p.drawArc(QRectF(4, 4, 16, 16), 35 * 16, 285 * 16);
        path.moveTo(3, 4);
        path.lineTo(3, 10);
        path.lineTo(8, 10);
        path.moveTo(12, 7);
        path.lineTo(12, 12);
        path.lineTo(16, 14);
    }
    else if (name == "music")
    {
        path.moveTo(9, 17);
        path.lineTo(9, 5);
        path.lineTo(20, 3);
        path.lineTo(20, 15);
        p.drawEllipse(QRectF(3, 15, 6, 5));
        p.drawEllipse(QRectF(14, 13, 6, 5));
        p.drawLine(9, 9, 20, 7);
    }
    else if (name == "volume" || name == "muted")
    {
        path.moveTo(4, 9);
        path.lineTo(8, 9);
        path.lineTo(13, 5);
        path.lineTo(13, 19);
        path.lineTo(8, 15);
        path.lineTo(4, 15);
        path.closeSubpath();
        if (name == "volume")
        {
            p.drawArc(QRectF(8, 5, 12, 14), -60 * 16, 120 * 16);
        }
        else
        {
            p.drawLine(17, 9, 22, 15);
            p.drawLine(17, 15, 22, 9);
        }
    }
    else if (name == "lyrics")
    {
        p.drawRoundedRect(QRectF(3, 4, 18, 14), 3, 3);
        p.drawLine(7, 9, 17, 9);
        p.drawLine(7, 13, 14, 13);
        p.drawLine(8, 18, 6, 21);
    }
    else if (name == "queue")
    {
        p.drawLine(4, 6, 20, 6);
        p.drawLine(4, 12, 20, 12);
        p.drawLine(4, 18, 14, 18);
    }
    else if (name == "repeat")
    {
        path.moveTo(17, 4);
        path.lineTo(21, 8);
        path.lineTo(17, 12);
        path.moveTo(21, 8);
        path.lineTo(7, 8);
        path.quadTo(3, 8, 3, 12);
        path.moveTo(7, 20);
        path.lineTo(3, 16);
        path.lineTo(7, 12);
        path.moveTo(3, 16);
        path.lineTo(17, 16);
        path.quadTo(21, 16, 21, 12);
    }
    else if (name == "shuffle")
    {
        path.moveTo(3, 6);
        path.lineTo(6, 6);
        path.cubicTo(10, 6, 14, 18, 18, 18);
        path.lineTo(21, 18);
        path.moveTo(18, 15);
        path.lineTo(21, 18);
        path.lineTo(18, 21);
        path.moveTo(3, 18);
        path.lineTo(6, 18);
        path.cubicTo(10, 18, 14, 6, 18, 6);
        path.lineTo(21, 6);
        path.moveTo(18, 3);
        path.lineTo(21, 6);
        path.lineTo(18, 9);
    }
    else if (name == "user")
    {
        p.drawEllipse(QRectF(8, 3, 8, 8));
        path.moveTo(4, 21);
        path.cubicTo(4, 12, 20, 12, 20, 21);
    }
    else
    {
        p.drawEllipse(QRectF(4, 4, 16, 16));
        p.drawLine(12, 11, 12, 17);
        p.drawPoint(12, 7);
    }
    p.drawPath(path);
    return QIcon(pix);
}

int QueueModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : rows.size();
}
int QueueModel::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : 4; }
QVariant QueueModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rows.size())
        return {};
    const Row &row = rows.at(index.row());
    if (index.column() == 1) {
        if (role == SongListDelegate::ArtistRole) return row.artist;
        if (role == SongListDelegate::MediaUrlRole) return row.mediaUrl;
        if (role == SongListDelegate::SongIdRole) return row.songId;
        if (role == SongListDelegate::CoverUrlRole) return row.coverUrl;
    }
    if (role == Qt::ForegroundRole)
        return QColor(index.row() == currentTrack ? "#b4d9a0"
                      : index.column() == 1       ? "#e5eae3"
                                                  : "#859188");
    if (role == Qt::TextAlignmentRole && index.column() == 0)
        return int(Qt::AlignCenter);
    if (role == Qt::ToolTipRole)
        return row.title + " · " + row.artist;
    if (role != Qt::DisplayRole)
        return {};
    switch (index.column())
    {
    case 0:
        return index.row() == currentTrack
                   ? QStringLiteral("•")
                   : QString("%1").arg(index.row() + 1, 2, 10, QLatin1Char('0'));
    case 1:
        return row.title;
    case 2:
        return row.album.isEmpty() ? QStringLiteral("—") : row.album;
    default:
        return row.local ? QStringLiteral("本地音频") : QStringLiteral("在线音频");
    }
}
QVariant QueueModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    return QStringList{"#", "歌曲", "专辑", "来源"}.value(section);
}
void QueueModel::append(const QString &title, const QString &artist, bool local,
                        const QUrl &mediaUrl, qint64 songId)
{
    beginInsertRows({}, rows.size(), rows.size());
    rows.append({title, artist, local, mediaUrl, songId, {}, {}});
    endInsertRows();
}
void QueueModel::setArtwork(int index, const QString &album, const QString &coverUrl)
{
    if (index < 0 || index >= rows.size()) return;
    rows[index].album = album;
    rows[index].coverUrl = coverUrl;
    emit dataChanged(this->index(index, 1), this->index(index, 2));
}
void QueueModel::clear()
{
    beginResetModel(); rows.clear(); currentTrack = -1; endResetModel();
}
void QueueModel::setCurrentTrack(int index)
{
    const int previous = currentTrack;
    currentTrack = index;
    if (previous >= 0)
        emit dataChanged(this->index(previous, 0), this->index(previous, 3));
    if (index >= 0)
        emit dataChanged(this->index(index, 0), this->index(index, 3));
}

ArtworkWidget::ArtworkWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(190, 190);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(CoverArtCache::instance(), &CoverArtCache::imageReady, this, [this] {
        cover = CoverArtCache::instance()->image(currentMedia, currentSongId, currentCoverUrl);
        update();
    });
}
void ArtworkWidget::loadCover(const QString &audioPath)
{
    loadTrack(audioPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(audioPath));
}
void ArtworkWidget::loadTrack(const QUrl &mediaUrl, qint64 songId, const QString &coverUrl)
{
    currentMedia = mediaUrl;
    currentSongId = songId;
    currentCoverUrl = coverUrl;
    cover = CoverArtCache::instance()->image(mediaUrl, songId, coverUrl);
    update();
}
void ArtworkWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal side = qMin(width(), height());
    p.translate((width() - side) / 2, (height() - side) / 2);
    p.scale(side / 320, side / 320);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, 320, 320), 12, 12);
    p.setClipPath(clip);
    if (!cover.isNull())
    {
        const int s = qMin(cover.width(), cover.height());
        p.drawImage(QRectF(0, 0, 320, 320), cover,
                    QRectF((cover.width() - s) / 2, (cover.height() - s) / 2, s, s));
        return;
    }
    QLinearGradient sky(0, 0, 210, 320);
    sky.setColorAt(0, QColor("#c6cdb2"));
    sky.setColorAt(.5, QColor("#7e9b84"));
    sky.setColorAt(1, QColor("#183e35"));
    p.fillRect(QRectF(0, 0, 320, 320), sky);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#e6dfb6"));
    p.drawEllipse(QPointF(216, 94), 40, 40);
    QPainterPath hill;
    hill.moveTo(0, 213);
    hill.cubicTo(78, 157, 112, 155, 182, 200);
    hill.cubicTo(242, 241, 269, 174, 320, 189);
    hill.lineTo(320, 320);
    hill.lineTo(0, 320);
    p.fillPath(hill, QColor("#688778"));
    hill = QPainterPath();
    hill.moveTo(0, 253);
    hill.cubicTo(95, 198, 138, 211, 223, 249);
    hill.cubicTo(267, 267, 298, 223, 320, 228);
    hill.lineTo(320, 320);
    hill.lineTo(0, 320);
    p.fillPath(hill, QColor("#355f50"));
    hill = QPainterPath();
    hill.moveTo(0, 285);
    hill.cubicTo(119, 235, 174, 295, 320, 267);
    hill.lineTo(320, 320);
    hill.lineTo(0, 320);
    p.fillPath(hill, QColor("#204739"));
    p.setPen(QColor("#f2f1da"));
    QFont f("Helvetica Neue");
    f.setPixelSize(11);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 3);
    p.setFont(f);
    p.drawText(QRectF(25, 26, 270, 20), Qt::AlignLeft, "TANGO / YOUR MUSIC");
    f.setPixelSize(8);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.8);
    p.setFont(f);
    p.drawText(QRectF(25, 284, 270, 16), Qt::AlignLeft, "A LITTLE SPACE FOR YOUR SOUND");
}

void PlayerWindowView::setupUi(QMainWindow *window)
{
    window->setWindowTitle(QStringLiteral("探歌 · Tango"));
    window->resize(1120, 780);
    window->setMinimumSize(1000, 700);
    if (auto *screen = window->screen())
    {
        const QRect area = screen->availableGeometry();
        window->resize(qMin(1120, area.width() - 60), qMin(780, area.height() - 60));
    }
    QFont font = QApplication::font();
    font.setPixelSize(14);
    window->setFont(font);
    window->setStyleSheet(R"(
        QMainWindow, QWidget#root { background:#121614; color:#ecf0ea; }
        QWidget { color:#ecf0ea; }
        QLabel { background:transparent; border:0; }
        QFrame#sidebar {background:#0d110f;border-right:1px solid #252d27;}
        QFrame#transport {background:#101411;border-top:1px solid #29322b;}
        QStackedWidget {background:transparent;border:0;}
        QPushButton#miniPlayerButton {text-align:left;padding:0;border-radius:9px;}
        QPushButton#miniPlayerButton:hover {background:#202a23;}
        QLabel[muted="true"] {color:#8c998f;}
        QLabel#songTitle {font-size:24px;font-weight:600;}
        QPushButton {background:transparent;border:0;border-radius:7px;padding:7px 10px;color:#a5b0a7;}
        QPushButton:hover {background:#252e27;color:#ecf0ea;}
        QPushButton:pressed {background:#344236;}
        QPushButton:checked {background:#253427;color:#c4e2b3;}
        QPushButton[nav="true"] {text-align:left;padding:9px 15px;}
        QPushButton[primary="true"] {background:#b4d9a0;color:#162313;font-weight:600;padding:9px 16px;}
        QPushButton[primary="true"]:hover {background:#c7e9b5;}
        QPushButton[play="true"] {background:#b4d9a0;border-radius:23px;padding:0;}
        QPushButton[play="true"]:hover {background:#ceebbc;}
        QPushButton[role="subtle"] {border:1px solid #344137;border-radius:15px;padding:5px 11px;font-size:11px;}
        QLineEdit {background:#1c231e;border:1px solid #2b352d;border-radius:8px;padding:9px 12px;selection-background-color:#476e40;}
        QLineEdit:focus {border:1px solid #8cab7f;}
        QTableView {background:transparent;alternate-background-color:#151b17;border:0;color:#dce4da;gridline-color:transparent;selection-background-color:#27372a;outline:0;}
        QTableView::item {border:0;padding:3px 9px;}
        QTableView::item:selected {background:#27372a;}
        QHeaderView {background:transparent;}
        QHeaderView::section {background:#121614;color:#718075;border:0;border-bottom:1px solid #2b352d;padding:9px;text-align:left;font-size:11px;}
        QSlider::groove:horizontal {height:3px;background:#354039;border-radius:1px;}
        QSlider::sub-page:horizontal {background:#b4d9a0;border-radius:1px;}
        QSlider::handle:horizontal {background:#d7ebcb;width:9px;height:9px;margin:-3px 0;border-radius:4px;}
        QScrollBar:vertical {background:transparent;width:5px;margin:0;}
        QScrollBar::handle:vertical {background:#3a483e;border-radius:2px;min-height:24px;}
        QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical {height:0;}
        QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical {background:transparent;}
        QMenu {background:#1c251f;color:#ecf0ea;border:1px solid #38483d;padding:5px;}
        QMenu::item {padding:8px 18px;}
        QMenu::item:selected {background:#344736;}
        QToolTip {background:#27332a;color:#ecf0ea;border:1px solid #4c5b50;padding:5px;}
    )");
    auto *root = new QWidget(window);
    root->setObjectName("root");
    window->setCentralWidget(root);
    auto *outer = new QVBoxLayout(root);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *body = new QHBoxLayout;
    body->setSpacing(0);
    outer->addLayout(body, 1);
    auto makeLabel = [](const QString &text, QWidget *parent, bool muted = false)
    {
        auto *l = new QLabel(text, parent);
        l->setTextFormat(Qt::PlainText);
        l->setProperty("muted", muted);
        return l;
    };
    auto button = [](const QString &text, const QString &icon, const QString &name, QWidget *parent)
    {
        auto *b = new QPushButton(text, parent);
        b->setObjectName(name);
        if (!icon.isEmpty())
            b->setIcon(playerIcon(icon));
        b->setIconSize(QSize(19, 19));
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    auto *sidebar = new QFrame(root);
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(180);
    body->addWidget(sidebar);
    auto *side = new QVBoxLayout(sidebar);
    side->setContentsMargins(16, 18, 16, 14);
    side->setSpacing(4);
    auto *brand = makeLabel("◒  探歌", sidebar);
    brand->setStyleSheet("font-size:25px;font-weight:600;color:#c8e1ba;");
    side->addWidget(brand);
    auto *tag = makeLabel("T A N G O  /  M U S I C", sidebar, true);
    tag->setStyleSheet("font-size:9px;padding:0 0 16px 3px;");
    side->addWidget(tag);
    auto *lib = makeLabel("音乐空间", sidebar, true);
    lib->setStyleSheet("font-size:10px;padding:8px 14px;");
    side->addWidget(lib);
    libraryButton = button("  音乐库", "music", "libraryButton", sidebar);
    nowPlayingButton = button("  正在播放", "lyrics", "nowPlayingButton", sidebar);
    pushButton_LikeSong = button("  喜欢的音乐", "heart", "pushButton_LikeSong", sidebar);
    pushButton_HistorySong = button("  最近播放", "history", "pushButton_HistorySong", sidebar);
    for (auto *b : {libraryButton, nowPlayingButton, pushButton_LikeSong, pushButton_HistorySong})
    {
        b->setProperty("nav", true);
        b->setCheckable(true);
        side->addWidget(b);
    }
    radioButton = button("  音乐助手", "music", "radioButton", sidebar);
    radioButton->setProperty("nav", true);
    radioButton->setCheckable(true);
    radioButton->setToolTip(QStringLiteral("场景找歌、歌手新作和口味记忆"));
    radioButton->setStyleSheet(QStringLiteral("color:#c5e7b6;"));
    side->addWidget(radioButton);
    focusButton = button("  专注计划", "music", "focusButton", sidebar);
    focusButton->setProperty("nav", true);
    focusButton->setCheckable(true);
    side->addWidget(focusButton);
    focusStatusLabel = makeLabel("按你的节奏，安心听歌。", sidebar, true);
    focusStatusLabel->setObjectName("focusStatusLabel");
    focusStatusLabel->setWordWrap(true);
    focusStatusLabel->setStyleSheet("font-size:10px;padding:4px 14px;color:#829c88;");
    side->addWidget(focusStatusLabel);
    side->addStretch();
    auto *hint = makeLabel("让音乐，回到生活里。", sidebar, true);
    hint->setStyleSheet("font-size:11px;color:#637568;padding:8px 14px;");
    side->addWidget(hint);
    moreButton = button("  更多功能", "", "moreButton", sidebar);
    moreButton->setProperty("nav", true);
    side->addWidget(moreButton);
    pushButton_About = button("  账户列表", "info", "pushButton_About", sidebar);
    pushButton_About->setProperty("nav", true);
    side->addWidget(pushButton_About);
    neteaseAccountButton = button("  连接网易云", "music", "neteaseAccountButton", sidebar);
    neteaseAccountButton->setProperty("nav", true);
    neteaseAccountButton->setToolTip("扫码登录网易云，使用账号实际播放权益");
    side->addWidget(neteaseAccountButton);
    pushButton_SignIn = button("  登录探歌", "user", "pushButton_SignIn", sidebar);
    pushButton_SignIn->setProperty("nav", true);
    side->addWidget(pushButton_SignIn);
    label_DisplayUser = makeLabel("访客 · 数据仅保存在本机", sidebar, true);
    label_DisplayUser->setObjectName("label_DisplayUser");
    label_DisplayUser->setStyleSheet("font-size:10px;padding-left:15px;");
    side->addWidget(label_DisplayUser);
    auto *content = new QWidget(root);
    body->addWidget(content, 1);
    auto *main = new QVBoxLayout(content);
    main->setContentsMargins(24, 20, 24, 10);
    main->setSpacing(10);
    contentToolbar = new QWidget(content);
    contentToolbar->setObjectName("contentToolbar");
    auto *top = new QHBoxLayout(contentToolbar);
    top->setContentsMargins(0, 0, 0, 0);
    auto *eyebrow = makeLabel("一 / 你 的 私 人 音 乐 空 间", contentToolbar, true);
    eyebrow->setStyleSheet("font-size:10px;color:#829c88;");
    top->addWidget(eyebrow);
    top->addStretch();
    lineEdit_InputSong = new QLineEdit(content);
    lineEdit_InputSong->setObjectName("lineEdit_InputSong");
    lineEdit_InputSong->setPlaceholderText("搜索在线音乐");
    lineEdit_InputSong->setClearButtonEnabled(true);
    lineEdit_InputSong->setMaximumWidth(240);
    lineEdit_InputSong->setMinimumWidth(130);
    lineEdit_InputSong->setFixedHeight(36);
    top->addWidget(lineEdit_InputSong);
    pushButton_SearchSong = button("", "search", "pushButton_SearchSong", content);
    pushButton_SearchSong->setToolTip("搜索在线音乐（需要网络）");
    pushButton_SearchSong->setFixedSize(34, 34);
    top->addWidget(pushButton_SearchSong);
    pushButton_AddSong = button(" 添加音乐", "plus", "pushButton_AddSong", content);
    pushButton_AddSong->setProperty("primary", true);
    pushButton_AddSong->setIcon(playerIcon("plus", QColor("#172711")));
    top->addWidget(pushButton_AddSong);
    main->addWidget(contentToolbar);
    pageStack = new QStackedWidget(content);
    pageStack->setObjectName("pageStack");
    pageStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    main->addWidget(pageStack, 1);
    libraryPage = new QWidget(pageStack);
    libraryPage->setObjectName("libraryPage");
    auto *libraryLayout = new QVBoxLayout(libraryPage);
    libraryLayout->setContentsMargins(0, 12, 0, 0);
    libraryLayout->setSpacing(14);
    auto *libraryHeading = makeLabel("音乐库", libraryPage);
    libraryHeading->setStyleSheet("font-size:25px;font-weight:600;color:#e7eee4;");
    libraryLayout->addWidget(libraryHeading);
    auto *libraryCaption = makeLabel("收藏此刻的声音，随时回到喜欢的旋律。", libraryPage, true);
    libraryCaption->setStyleSheet("font-size:12px;color:#7f9385;padding-bottom:6px;");
    libraryLayout->addWidget(libraryCaption);
    pageStack->addWidget(libraryPage);
    nowPlayingPage = new QWidget(pageStack);
    nowPlayingPage->setObjectName("nowPlayingPage");
    pageStack->addWidget(nowPlayingPage);
    auto *hero = new QHBoxLayout;
    hero->setSpacing(28);
    artwork = new ArtworkWidget(nowPlayingPage);
    artwork->setMaximumSize(360, 360);
    artwork->setMinimumSize(220, 220);
    hero->addWidget(artwork, 4, Qt::AlignVCenter);
    auto *details = new QVBoxLayout;
    details->setSpacing(5);
    auto *metaRow = new QHBoxLayout;
    auto *playing = makeLabel("●  NOW PLAYING", nowPlayingPage);
    playing->setStyleSheet("color:#a9ce96;font-size:10px;letter-spacing:1px;");
    metaRow->addWidget(playing);
    metaRow->addStretch();
    importLyricsButton = button("导入歌词", "", "importLyricsButton", nowPlayingPage);
    importLyricsButton->setProperty("role", "subtle");
    importLyricsButton->setToolTip("为当前歌曲选择 LRC 歌词文件");
    metaRow->addWidget(importLyricsButton);
    details->addLayout(metaRow);
    label_DisplaySongTitle = new TrackTitleLabel("把喜欢的音乐，放进来", nowPlayingPage);
    label_DisplaySongTitle->setObjectName("songTitle");
    label_DisplaySongTitle->setWordWrap(true);
    label_DisplaySongTitle->setMaximumHeight(64);
    label_DisplaySongTitle->setMinimumWidth(0);
    label_DisplaySongTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    details->addWidget(label_DisplaySongTitle);
    artistLabel = makeLabel("从一首歌开始，享受属于你的片刻。", nowPlayingPage, true);
    artistLabel->setWordWrap(true);
    artistLabel->setMaximumHeight(36);
    artistLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    details->addWidget(artistLabel);
    lyricsView = new LyricsView(nowPlayingPage);
    lyricsView->setObjectName("lyricsView");
    lyricsView->setMinimumHeight(210);
    lyricsView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    details->addWidget(lyricsView, 1);
    lyricsView->setToolTip("LRC 同步歌词 · 点击歌词跳转");
    hero->addLayout(details, 6);
    auto *detailLayout = new QVBoxLayout(nowPlayingPage);
    detailLayout->setContentsMargins(8, 18, 8, 24);
    detailLayout->addLayout(hero, 1);
    auto *queueHeader = new QHBoxLayout;
    auto *queueTitle = makeLabel("播放队列", libraryPage);
    queueTitle->setStyleSheet("font-size:16px;font-weight:600;");
    queueHeader->addWidget(queueTitle);
    queueCountLabel = makeLabel("0 首歌曲", libraryPage, true);
    queueCountLabel->setStyleSheet("font-size:10px;padding-left:7px;");
    queueHeader->addWidget(queueCountLabel);
    queueHeader->addStretch();
    auto *tip = makeLabel("双击歌曲播放", libraryPage, true);
    tip->setStyleSheet("font-size:10px;");
    queueHeader->addWidget(tip);
    libraryLayout->addLayout(queueHeader);
    queueView = new QTableView(libraryPage);
    queueView->setObjectName("queueView");
    queueModel = new QueueModel(window);
    queueView->setModel(queueModel);
    queueView->setShowGrid(false);
    queueView->setSelectionBehavior(QAbstractItemView::SelectRows);
    queueView->setSelectionMode(QAbstractItemView::SingleSelection);
    queueView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    queueView->verticalHeader()->hide();
    queueView->verticalHeader()->setDefaultSectionSize(72);
    queueView->setAlternatingRowColors(false);
    SongListDelegate::install(queueView, 1);
    queueView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    queueView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    queueView->setColumnWidth(0, 42);
    queueView->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    queueView->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    queueView->setColumnWidth(2, 150);
    queueView->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    queueView->setColumnWidth(3, 95);
    queueView->setMinimumHeight(88);
    libraryLayout->addWidget(queueView, 1);
    emptyQueueLabel =
        makeLabel("还没有歌曲 · 添加音乐，或将音频文件拖入窗口", queueView->viewport(), true);
    emptyQueueLabel->setAlignment(Qt::AlignCenter);
    emptyQueueLabel->setWordWrap(true);
    emptyQueueLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    emptyQueueLabel->setStyleSheet("color:#829087;font-size:11px;");
    auto *emptyLayout = new QVBoxLayout(queueView->viewport());
    emptyLayout->setContentsMargins(8, 0, 8, 0);
    emptyLayout->addWidget(emptyQueueLabel);
    statusLabel = makeLabel("本地播放，安静而纯粹。", content, true);
    statusLabel->setStyleSheet("color:#687c6d;font-size:10px;");
    statusLabel->setFixedHeight(16);
    statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    main->addWidget(statusLabel);
    auto *transport = new QFrame(root);
    transport->setObjectName("transport");
    outer->addWidget(transport);
    auto *bar = new QVBoxLayout(transport);
    bar->setContentsMargins(20, 7, 20, 12);
    bar->setSpacing(7);
    auto *control = new QGridLayout;
    control->setContentsMargins(0, 0, 0, 0);
    control->setHorizontalSpacing(16);
    control->setColumnStretch(0, 1);
    control->setColumnStretch(2, 1);
    miniPlayerButton = button("", "", "miniPlayerButton", transport);
    miniPlayerButton->setMinimumWidth(210);
    miniPlayerButton->setMaximumWidth(310);
    miniPlayerButton->setFixedHeight(62);
    miniPlayerButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    miniPlayerButton->setToolTip("展开正在播放 · 封面与同步歌词");
    miniPlayerButton->setAccessibleName("展开正在播放");
    auto *miniLayout = new QHBoxLayout(miniPlayerButton);
    miniLayout->setContentsMargins(6, 5, 10, 5);
    miniLayout->setSpacing(11);
    miniArtwork = new ArtworkWidget(miniPlayerButton);
    miniArtwork->setMinimumSize(48, 48);
    miniArtwork->setMaximumSize(48, 48);
    miniArtwork->setAttribute(Qt::WA_TransparentForMouseEvents);
    miniLayout->addWidget(miniArtwork);
    auto *miniText = new QVBoxLayout;
    miniText->setSpacing(2);
    miniText->setContentsMargins(0, 5, 0, 5);
    miniTitleLabel = new ElidedLabel("还没有播放歌曲", miniPlayerButton);
    miniTitleLabel->setObjectName("miniTitleLabel");
    miniTitleLabel->setTextFormat(Qt::PlainText);
    miniTitleLabel->setStyleSheet("font-size:13px;font-weight:600;color:#e7eee4;");
    miniTitleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    miniArtistLabel = new ElidedLabel("选择一首歌，开始收听", miniPlayerButton);
    miniArtistLabel->setObjectName("miniArtistLabel");
    miniArtistLabel->setTextFormat(Qt::PlainText);
    miniArtistLabel->setStyleSheet("font-size:11px;color:#84998a;");
    miniArtistLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    miniText->addWidget(miniTitleLabel);
    miniText->addWidget(miniArtistLabel);
    miniLayout->addLayout(miniText, 1);
    auto *expandHint = makeLabel("⌃", miniPlayerButton, true);
    expandHint->setStyleSheet("font-size:15px;color:#718c78;");
    expandHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    miniLayout->addWidget(expandHint);
    control->addWidget(miniPlayerButton, 0, 0, Qt::AlignLeft);
    auto *playbackControls = new QHBoxLayout;
    playbackControls->setSpacing(8);
    shuffleButton = button("", "shuffle", "shuffleButton", transport);
    shuffleButton->setCheckable(true);
    shuffleButton->setToolTip("随机播放");
    shuffleButton->setFixedSize(32, 32);
    playbackControls->addWidget(shuffleButton);
    pushButton_PreviousSong = button("", "previous", "pushButton_PreviousSong", transport);
    pushButton_PreviousSong->setToolTip("上一首");
    pushButton_PreviousSong->setFixedSize(34, 34);
    playbackControls->addWidget(pushButton_PreviousSong);
    pushButton_PlaySong = button("", "play", "pushButton_PlaySong", transport);
    pushButton_PlaySong->setProperty("play", true);
    pushButton_PlaySong->setIcon(playerIcon("play", QColor("#172711")));
    pushButton_PlaySong->setIconSize(QSize(23, 23));
    pushButton_PlaySong->setFixedSize(46, 46);
    pushButton_PlaySong->setToolTip("播放 / 暂停 · 空格键");
    playbackControls->addWidget(pushButton_PlaySong);
    pushButton_PlayNextSong = button("", "next", "pushButton_PlayNextSong", transport);
    pushButton_PlayNextSong->setToolTip("下一首");
    pushButton_PlayNextSong->setFixedSize(34, 34);
    playbackControls->addWidget(pushButton_PlayNextSong);
    repeatButton = button("", "repeat", "repeatButton", transport);
    repeatButton->setCheckable(true);
    repeatButton->setChecked(true);
    repeatButton->setToolTip("列表循环（点击切换单曲循环 / 顺序播放）");
    repeatButton->setFixedSize(32, 32);
    playbackControls->addWidget(repeatButton);
    control->addLayout(playbackControls, 0, 1);
    auto *options = new QHBoxLayout;
    options->setSpacing(4);
    options->addStretch();
    pushButton_AddLike = button("", "heart", "pushButton_AddLike", transport);
    pushButton_AddLike->setToolTip("收藏当前歌曲");
    pushButton_AddLike->setFixedSize(32, 32);
    options->addWidget(pushButton_AddLike);
    floatingLyricsButton = button(" 歌词", "lyrics", "floatingLyricsButton", transport);
    floatingLyricsButton->setCheckable(true);
    floatingLyricsButton->setToolTip("显示 / 隐藏透明桌面歌词");
    floatingLyricsButton->setFixedHeight(32);
    options->addWidget(floatingLyricsButton);
    queueButton = button("", "queue", "queueButton", transport);
    queueButton->setToolTip("显示播放队列");
    queueButton->setFixedSize(32, 32);
    options->addWidget(queueButton);
    pushButton_SoundYesNo = button("", "volume", "pushButton_SoundYesNo", transport);
    pushButton_SoundYesNo->setToolTip("静音 / 取消静音");
    pushButton_SoundYesNo->setFixedSize(32, 32);
    options->addWidget(pushButton_SoundYesNo);
    horizontalSlider_Volume = new QSlider(Qt::Horizontal, transport);
    horizontalSlider_Volume->setObjectName("horizontalSlider_Volume");
    horizontalSlider_Volume->setRange(0, 100);
    horizontalSlider_Volume->setValue(36);
    horizontalSlider_Volume->setFixedWidth(64);
    horizontalSlider_Volume->setToolTip("音量");
    options->addWidget(horizontalSlider_Volume);
    control->addLayout(options, 0, 2);
    bar->addLayout(control);
    auto *progress = new QHBoxLayout;
    progress->setSpacing(13);
    elapsedLabel = makeLabel("0:00", transport, true);
    elapsedLabel->setMinimumWidth(37);
    elapsedLabel->setStyleSheet("font-size:10px;");
    progress->addWidget(elapsedLabel);
    horizontalSlider_PlayProgress = new QSlider(Qt::Horizontal, transport);
    horizontalSlider_PlayProgress->setObjectName("horizontalSlider_PlayProgress");
    horizontalSlider_PlayProgress->setRange(0, 0);
    progress->addWidget(horizontalSlider_PlayProgress, 1);
    durationLabel = makeLabel("0:00", transport, true);
    durationLabel->setMinimumWidth(37);
    durationLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    durationLabel->setStyleSheet("font-size:10px;");
    progress->addWidget(durationLabel);
    bar->addLayout(progress);
    setPage(libraryPage);
}

void PlayerWindowView::addPage(QWidget *page)
{
    if (page && pageStack && pageStack->indexOf(page) < 0)
        pageStack->addWidget(page);
}

void PlayerWindowView::setPage(QWidget *page)
{
    // 聊天页面只留下一个输入入口；切回曲库等页面时恢复搜索和添加音乐。
    const bool chatting = page && page->objectName() == QStringLiteral("radioDialog");
    contentToolbar->setVisible(!chatting);
    statusLabel->setVisible(!chatting);
    if (!page || !pageStack)
        return;
    addPage(page);
    pageStack->setCurrentWidget(page);
    for (auto *button : {libraryButton, nowPlayingButton, pushButton_LikeSong,
                         pushButton_HistorySong, radioButton, focusButton})
        button->setChecked(button == libraryButton ? page == libraryPage
                           : button == nowPlayingButton ? page == nowPlayingPage : false);
}

void PlayerWindowView::setNowPlaying(const QString &title, const QString &artist,
                                     const QUrl &mediaUrl, qint64 songId,
                                     const QString &coverUrl)
{
    label_DisplaySongTitle->setText(title);
    artistLabel->setText(artist);
    miniTitleLabel->setText(title.isEmpty() ? QStringLiteral("还没有播放歌曲") : title);
    miniTitleLabel->setToolTip(title);
    miniArtistLabel->setText(artist);
    miniArtistLabel->setToolTip(artist);
    miniPlayerButton->setToolTip(title.isEmpty() ? QStringLiteral("展开正在播放 · 封面与同步歌词")
                                : title + QStringLiteral(" · ") + artist
                                  + QStringLiteral("\n点击展开封面与同步歌词"));
    artwork->loadTrack(mediaUrl, songId, coverUrl);
    miniArtwork->loadTrack(mediaUrl, songId, coverUrl);
}
