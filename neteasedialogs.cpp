#include "neteasedialogs.h"
#include "dialogstyle.h"
#include "songlistdelegate.h"

#include <QBuffer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {

QLabel *makeLabel(const QString &text, const char *name, QWidget *parent,
                 const char *role = nullptr)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QString::fromLatin1(name));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    if (role) label->setProperty("role", role);
    return label;
}

QProgressBar *makeProgress(QWidget *parent, const char *name)
{
    auto *progress = new QProgressBar(parent);
    progress->setObjectName(QString::fromLatin1(name));
    progress->setRange(0, 0);
    progress->setTextVisible(false);
    progress->setFixedHeight(3);
    return progress;
}

QString catalogLabel(int fee)
{
    switch (fee) {
    case 1: return QStringLiteral("VIP");
    case 4: return QStringLiteral("数字专辑");
    case 8: return QStringLiteral("部分音质付费");
    case 0: return QStringLiteral("免费");
    default: return QStringLiteral("以授权为准");
    }
}

QString durationLabel(int milliseconds)
{
    if (milliseconds <= 0) return QStringLiteral("—");
    const int seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

void styleProgress(QProgressBar *progress)
{
    progress->setStyleSheet(QStringLiteral(
        "QProgressBar { border: none; background: #252d27; border-radius: 1px; }"
        "QProgressBar::chunk { background: #b4d9a0; border-radius: 1px; }"));
}

} // namespace

NetEaseLoginDialog::NetEaseLoginDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("neteaseLoginDialog"));
    setWindowTitle(QStringLiteral("网易云音乐账号"));
    setMinimumSize(420, 590);
    resize(460, 610);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 26, 28, 24);
    layout->setSpacing(14);
    layout->addWidget(makeLabel(QStringLiteral("连接网易云音乐"), "label_NeteaseHeading", this, "heading"));
    layout->addWidget(makeLabel(QStringLiteral("使用手机上的网易云音乐扫码，并在手机上确认登录。无需在此输入账号密码。"),
                                "label_NeteaseInstructions", this, "subtitle"));

    accountPages = new QStackedWidget(this);
    accountPages->setObjectName(QStringLiteral("stack_NeteaseAccount"));
    auto *qrPage = new QWidget(accountPages);
    // Keep the fixed scan target and refresh control apart at the minimum size.
    qrPage->setMinimumHeight(314);
    auto *qrLayout = new QVBoxLayout(qrPage);
    qrLayout->setContentsMargins(0, 8, 0, 8);
    qrLayout->setSpacing(14);
    qrImage = makeLabel(QStringLiteral("正在生成二维码…"), "label_NeteaseQr", qrPage);
    qrImage->setFixedSize(248, 248);
    qrImage->setAlignment(Qt::AlignCenter);
    qrImage->setAccessibleName(QStringLiteral("网易云音乐登录二维码"));
    qrLayout->addWidget(qrImage, 0, Qt::AlignCenter);
    refreshButton = new QPushButton(QStringLiteral("刷新二维码"), qrPage);
    refreshButton->setObjectName(QStringLiteral("button_NeteaseRefreshQr"));
    refreshButton->setAutoDefault(false);
    qrLayout->addWidget(refreshButton, 0, Qt::AlignCenter);
    accountPages->addWidget(qrPage);

    auto *accountPage = new QWidget(accountPages);
    auto *accountLayout = new QVBoxLayout(accountPage);
    accountLayout->setContentsMargins(20, 32, 20, 24);
    accountLayout->setSpacing(16);
    accountLayout->addStretch();
    accountLayout->addWidget(makeLabel(QStringLiteral("已连接网易云音乐账号"), "label_NeteaseConnected", accountPage, "subtitle"));
    nicknameLabel = makeLabel(QString(), "label_NeteaseNickname", accountPage, "heading");
    accountLayout->addWidget(nicknameLabel);
    vipLabel = makeLabel(QString(), "label_NeteaseVip", accountPage);
    accountLayout->addWidget(vipLabel);
    accountLayout->addStretch();
    logoutButton = new QPushButton(QStringLiteral("退出网易云账号"), accountPage);
    logoutButton->setObjectName(QStringLiteral("button_NeteaseLogout"));
    logoutButton->setProperty("role", "danger");
    logoutButton->setAutoDefault(false);
    accountLayout->addWidget(logoutButton, 0, Qt::AlignLeft);
    accountPages->addWidget(accountPage);
    layout->addWidget(accountPages, 1);

    progress = makeProgress(this, "progress_NeteaseLogin");
    layout->addWidget(progress);
    statusLabel = makeLabel(QStringLiteral("正在生成二维码…"), "label_NeteaseLoginStatus", this);
    statusLabel->setMinimumHeight(36);
    layout->addWidget(statusLabel);
    layout->addWidget(makeLabel(QStringLiteral("此账号与播放器的本机账号独立。歌曲能否完整播放，以网易云返回的当前账号权益和歌曲授权为准。"),
                                "label_NeteaseRightsHint", this, "subtitle"));
    auto *footer = new QHBoxLayout;
    footer->addStretch();
    auto *closeButton = new QPushButton(QStringLiteral("完成"), this);
    closeButton->setObjectName(QStringLiteral("button_NeteaseClose"));
    closeButton->setAutoDefault(false);
    footer->addWidget(closeButton);
    layout->addLayout(footer);

    connect(refreshButton, &QPushButton::clicked, this, &NetEaseLoginDialog::refreshQrRequested);
    connect(logoutButton, &QPushButton::clicked, this, &NetEaseLoginDialog::logoutRequested);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    applyDialogStyle(this);
    qrImage->setStyleSheet(QStringLiteral("background: #f8faf6; color: #526057; border-radius: 12px; padding: 10px;"));
    styleProgress(progress);
    setBusy(true);
}

void NetEaseLoginDialog::setAccount(bool loggedIn, const QString &nickname, int vipType)
{
    accountPages->setCurrentIndex(loggedIn ? 1 : 0);
    nicknameLabel->setText(nickname.trimmed().isEmpty() ? QStringLiteral("网易云用户") : nickname);
    vipLabel->setText(vipType > 0 ? QStringLiteral("网易云会员身份 · VIP") : QStringLiteral("网易云普通账号"));
    if (loggedIn) {
        qrImage->clear();
        setBusy(false);
    }
}

void NetEaseLoginDialog::setQrImage(const QString &dataUrl)
{
    constexpr int maxImageBytes = 512 * 1024;
    // Accept an inline raster only. No URL is fetched by this view, and both the
    // compressed input and decoded dimensions are bounded before allocation.
    const int comma = dataUrl.indexOf(QLatin1Char(','));
    const QString header = dataUrl.left(comma);
    if (comma < 0 || dataUrl.size() > maxImageBytes * 4 / 3 + 128
        || (header != QStringLiteral("data:image/png;base64")
            && header != QStringLiteral("data:image/jpeg;base64"))) {
        qrImage->setText(QStringLiteral("二维码不可用\n请刷新后重试"));
        return;
    }
    QByteArray bytes = QByteArray::fromBase64(dataUrl.mid(comma + 1).toLatin1(),
                                             QByteArray::AbortOnBase64DecodingErrors);
    if (bytes.isEmpty() || bytes.size() > maxImageBytes) {
        qrImage->setText(QStringLiteral("二维码不可用\n请刷新后重试"));
        return;
    }
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, header.contains(QStringLiteral("png")) ? "png" : "jpeg");
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > 4096 || size.height() > 4096) {
        qrImage->setText(QStringLiteral("二维码不可用\n请刷新后重试"));
        return;
    }
    if (size.width() > 320 || size.height() > 320)
        reader.setScaledSize(size.scaled(320, 320, Qt::KeepAspectRatio));
    const QImage decoded = reader.read();
    if (decoded.isNull()) {
        qrImage->setText(QStringLiteral("二维码不可用\n请刷新后重试"));
        return;
    }
    qrImage->setPixmap(QPixmap::fromImage(decoded).scaled(228, 228, Qt::KeepAspectRatio, Qt::FastTransformation));
}

void NetEaseLoginDialog::setStatus(const QString &text)
{
    statusLabel->setText(text);
}

void NetEaseLoginDialog::setBusy(bool busy)
{
    progress->setVisible(busy);
    refreshButton->setEnabled(!busy);
    logoutButton->setEnabled(!busy);
}

OnlineSearchDialog::OnlineSearchDialog(QWidget *parent)
    : QDialog(parent)
{
    qRegisterMetaType<OnlineSong>();
    setObjectName(QStringLiteral("onlineSearchDialog"));
    setWindowTitle(QStringLiteral("网易云音乐 · 搜索结果"));
    setMinimumSize(720, 480);
    resize(860, 620);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 28, 28, 22);
    layout->setSpacing(14);
    layout->addWidget(makeLabel(QStringLiteral("搜索结果"), "label_OnlineSearchHeading", this, "heading"));
    queryLabel = makeLabel(QStringLiteral("正在搜索网易云音乐…"), "label_OnlineSearchQuery", this, "subtitle");
    layout->addWidget(queryLabel);
    progress = makeProgress(this, "progress_OnlineSearch");
    layout->addWidget(progress);

    table = new QTableWidget(0, 4, this);
    table->setObjectName(QStringLiteral("table_OnlineSearchResults"));
    table->setHorizontalHeaderLabels({QStringLiteral("歌曲"), QStringLiteral("专辑"), QStringLiteral("时长"), QStringLiteral("标记")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table->setColumnWidth(1, 148);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    table->setColumnWidth(2, 68);
    table->setColumnWidth(3, 108);
    table->setTextElideMode(Qt::ElideRight);
    table->setAccessibleName(QStringLiteral("网易云歌曲搜索结果，双击播放所选歌曲"));
    layout->addWidget(table, 1);

    auto *pageRow = new QHBoxLayout;
    statusLabel = makeLabel(QStringLiteral("正在搜索…"), "label_OnlineSearchStatus", this, "subtitle");
    pageRow->addWidget(statusLabel, 1);
    previousButton = new QPushButton(QStringLiteral("上一页"), this);
    previousButton->setObjectName(QStringLiteral("button_OnlineSearchPrevious"));
    previousButton->setAutoDefault(false);
    pageRow->addWidget(previousButton);
    pageLabel = makeLabel(QStringLiteral("1 / 1"), "label_OnlineSearchPage", this, "subtitle");
    pageLabel->setAlignment(Qt::AlignCenter);
    pageLabel->setMinimumWidth(54);
    pageLabel->setWordWrap(false);
    pageRow->addWidget(pageLabel);
    nextButton = new QPushButton(QStringLiteral("下一页"), this);
    nextButton->setObjectName(QStringLiteral("button_OnlineSearchNext"));
    nextButton->setAutoDefault(false);
    pageRow->addWidget(nextButton);
    layout->addLayout(pageRow);
    layout->addWidget(makeLabel(QStringLiteral("完整播放以当前网易云账号权益为准。"),
                                "label_OnlineSearchRightsHint", this, "subtitle"));

    auto *buttons = new QHBoxLayout;
    officialButton = new QPushButton(QStringLiteral("在网易云打开"), this);
    officialButton->setObjectName(QStringLiteral("button_OnlineSearchOfficial"));
    officialButton->setAutoDefault(false);
    buttons->addWidget(officialButton);
    buttons->addStretch();
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setObjectName(QStringLiteral("button_OnlineSearchClose"));
    closeButton->setAutoDefault(false);
    buttons->addWidget(closeButton);
    playButton = new QPushButton(QStringLiteral("播放所选"), this);
    playButton->setObjectName(QStringLiteral("button_OnlineSearchPlay"));
    playButton->setProperty("role", "primary");
    playButton->setDefault(true);
    buttons->addWidget(playButton);
    layout->addLayout(buttons);

    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(playButton, &QPushButton::clicked, this, &OnlineSearchDialog::activateSelectedSong);
    connect(table, &QTableWidget::cellDoubleClicked, this, [this](int, int) { activateSelectedSong(); });
    connect(table, &QTableWidget::itemSelectionChanged, this, &OnlineSearchDialog::updateControls);
    connect(previousButton, &QPushButton::clicked, this, [this] { requestPage(currentOffset - PageSize); });
    connect(nextButton, &QPushButton::clicked, this, [this] { requestPage(currentOffset + PageSize); });
    connect(officialButton, &QPushButton::clicked, this, [this] {
        const int row = table->currentRow();
        if (!busy && table->selectionModel()->hasSelection() && row >= 0 && row < results.size())
            emit officialPageRequested(results.at(row).id);
    });
    applyDialogStyle(this);
    SongListDelegate::install(table, 0);
    styleProgress(progress);
    setBusy(true);
}

void OnlineSearchDialog::setResults(const QVector<OnlineSong> &songs, int total, const QString &query)
{
    if (query != currentQuery) {
        currentOffset = 0;
        requestedOffset = 0;
    } else {
        currentOffset = requestedOffset;
    }
    currentQuery = query;
    // A misbehaving service cannot create an unbounded widget tree.
    results = songs.mid(0, PageSize);
    totalResults = std::max(total, currentOffset + results.size());
    queryLabel->setText(QStringLiteral("“%1” · 共 %2 首").arg(query).arg(totalResults));
    table->setRowCount(results.size());
    for (int row = 0; row < results.size(); ++row) {
        const OnlineSong &song = results.at(row);
        const QStringList values{song.title, song.album.isEmpty() ? QStringLiteral("—") : song.album,
                                 durationLabel(song.durationMs), catalogLabel(song.fee)};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setToolTip(values.at(column));
            if (column == 2) item->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, column, item);
        }
        auto *title = table->item(row, 0);
        title->setData(SongListDelegate::ArtistRole, song.artist);
        title->setData(SongListDelegate::MediaUrlRole,
                       QUrl(QStringLiteral("https://music.163.com/song?id=%1").arg(song.id)));
        title->setData(SongListDelegate::SongIdRole, song.id);
        title->setData(SongListDelegate::CoverUrlRole, song.coverUrl);
        title->setToolTip(song.title + QStringLiteral(" · ") + song.artist);
    }
    if (!results.isEmpty()) table->selectRow(0);
    setStatus(results.isEmpty() ? QStringLiteral("没有找到相关歌曲，试试歌名和歌手。")
                                : QStringLiteral("双击歌曲或点击「播放所选」"));
    setBusy(false);
}

void OnlineSearchDialog::setStatus(const QString &text)
{
    statusLabel->setText(text);
}

void OnlineSearchDialog::setBusy(bool isBusy)
{
    busy = isBusy;
    progress->setVisible(busy);
    table->setEnabled(!busy);
    updateControls();
}

void OnlineSearchDialog::activateSelectedSong()
{
    const int row = table->currentRow();
    if (!busy && table->selectionModel()->hasSelection() && row >= 0 && row < results.size())
        emit songActivated(results.at(row));
}

void OnlineSearchDialog::updateControls()
{
    const bool selected = table->selectionModel()->hasSelection()
        && table->currentRow() >= 0 && table->currentRow() < results.size();
    playButton->setEnabled(!busy && selected);
    officialButton->setEnabled(!busy && selected);
    previousButton->setEnabled(!busy && currentOffset > 0);
    nextButton->setEnabled(!busy && currentOffset + PageSize < totalResults && !results.isEmpty());
    pageLabel->setText(QStringLiteral("%1 / %2").arg(currentOffset / PageSize + 1)
                       .arg(totalResults > 0 ? (totalResults - 1) / PageSize + 1 : 1));
}

void OnlineSearchDialog::requestPage(int offset)
{
    if (busy || offset < 0 || offset >= totalResults) return;
    requestedOffset = offset;
    setStatus(QStringLiteral("正在加载…"));
    setBusy(true);
    emit pageRequested(offset);
}
