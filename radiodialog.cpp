#include "radiodialog.h"
#include "dialogstyle.h"
#include "songlistdelegate.h"
#include "coverartcache.h"

#include <QHeaderView>
#include <QDateTime>
#include <QHideEvent>
#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QJsonArray>
#include <QListWidget>
#include <QRegularExpression>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QTextDocument>
#include <QTextBrowser>
#include <QPainter>
#include <QTimer>
#include <functional>
#include <cmath>

namespace {
constexpr int maximumMessages = 24; // 最多保留十二轮本机非持久对话。
constexpr int maximumMessageLength = 1600;
constexpr int maximumInstructionLength = 1000;

// 输入法组词时回车仅选字；普通回车发送，Shift+回车换行。
class AssistantComposer : public QPlainTextEdit
{
public:
    explicit AssistantComposer(QWidget *parent) : QPlainTextEdit(parent) {}
    std::function<void()> send;
    void resetComposition() { composing = false; }
protected:
    void inputMethodEvent(QInputMethodEvent *event) override
    {
        composing = !event->preeditString().isEmpty();
        QPlainTextEdit::inputMethodEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            !(event->modifiers() & Qt::ShiftModifier) && !composing) {
            if (send) send();
            event->accept();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }
private:
    bool composing = false;
};

// 文档本身也拒绝资源回退，避免Qt绕过浏览器读取file或data地址。
class ConversationDocument : public QTextDocument
{
public:
    explicit ConversationDocument(QObject *parent) : QTextDocument(parent) {}
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

// 封面只接受程序登记的内存资源；消息不能加载图片、网页或外部资源。
class ConversationBrowser : public QTextBrowser
{
public:
    explicit ConversationBrowser(QWidget *parent) : QTextBrowser(parent)
    {
        setDocument(new ConversationDocument(this));
    }
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

QLabel *label(const QString &text, const char *name, QWidget *parent, const char *role = nullptr)
{
    auto *value = new QLabel(text, parent);
    value->setObjectName(QString::fromLatin1(name));
    value->setTextFormat(Qt::PlainText);
    value->setWordWrap(true);
    if (role) value->setProperty("role", role);
    return value;
}

QPushButton *button(const QString &text, const char *name, QWidget *parent)
{
    auto *value = new QPushButton(text, parent);
    value->setObjectName(QString::fromLatin1(name));
    value->setAutoDefault(false);
    return value;
}

QString bounded(const QString &text, int limit)
{
    const QString clean = text.trimmed();
    return clean.size() > limit ? clean.left(limit - 1) + QChar(0x2026) : clean;
}

QString htmlText(const QString &text)
{
    QString escaped = text.toHtmlEscaped();
    escaped.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
    return escaped;
}

QString chatBubble(const QString &speaker, const QString &body, bool user)
{
    // 外层留出对侧空间；内层边框与底色是气泡本身，窄窗仍按比例换行。
    const QString content = QStringLiteral("<table width=\"100%\" border=\"1\" cellspacing=\"0\" cellpadding=\"13\" "
        "style=\"border-color:%1;border-style:solid;\" bgcolor=\"%2\"><tr><td>"
        "<span style=\"font-size:11px;color:%3;\">%4</span><br/>"
        "<span style=\"font-size:14px;color:#e5ede7;\">%5</span></td></tr></table>")
        .arg(user ? QStringLiteral("#456650") : QStringLiteral("#33443a"),
             user ? QStringLiteral("#263e30") : QStringLiteral("#1b2720"),
             user ? QStringLiteral("#c3dfc7") : QStringLiteral("#9cad9f"), htmlText(speaker), body);
    const QString gap = QStringLiteral("<td width=\"22%\"></td>");
    const QString cell = QStringLiteral("<td width=\"78%\">%1</td>").arg(content);
    return QStringLiteral("<table width=\"100%\" border=\"0\" cellspacing=\"0\" cellpadding=\"0\"><tr>%1%2</tr></table>"
        "<p style=\"font-size:7px;margin:0;\">&nbsp;</p>").arg(user ? gap : cell, user ? cell : gap);
}
}

RadioDialog::RadioDialog(QWidget *parent, CoverArtCache *artwork) : QDialog(parent),
    artworkCache(artwork ? artwork : CoverArtCache::instance())
{
    setObjectName(QStringLiteral("radioDialog"));
    setWindowTitle(QStringLiteral("音乐助手 · 探歌"));
    setMinimumSize(760, 620);
    resize(860, 740);
    qRegisterMetaType<OnlineSong>();
    qRegisterMetaType<QVector<RadioRecommendation>>();
    qRegisterMetaType<AssistantArtist>();
    qRegisterMetaType<QVector<AssistantArtist>>();

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(22, 18, 22, 18);
    outer->setSpacing(7);
    auto *headerPanel = new QWidget(this);
    headerPanel->setObjectName(QStringLiteral("panel_RadioHeader"));
    auto *layout = new QVBoxLayout(headerPanel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(7);
    outer->addWidget(headerPanel);
    conversationToolbar = new QWidget(this);
    conversationToolbar->setObjectName(QStringLiteral("panel_ConversationToolbar"));
    auto *conversationHeading = new QHBoxLayout(conversationToolbar);
    conversationHeading->setContentsMargins(2, 0, 2, 6);
    conversationHeading->addWidget(label(QStringLiteral("探歌 · Tango"), "label_ConversationTitle", conversationToolbar));
    conversationHeading->addStretch();
    clearConversationsButton = button(QStringLiteral("清除全部对话"), "button_ConversationClearAll", conversationToolbar);
    clearConversationsButton->setToolTip(QStringLiteral("清除当前探歌账号的全部助手会话；音乐口味与播放队列分别管理。"));
    conversationHeading->addWidget(clearConversationsButton);
    outer->addWidget(conversationToolbar);
    conversationToolbar->hide();
    connect(clearConversationsButton, &QPushButton::clicked, this, [this] {
        if (conversationMode && loggedIn && !submissionPending && !conversationsClearing)
            emit clearConversationsRequested();
    });
    auto *heading = new QHBoxLayout;
    heading->addWidget(label(QStringLiteral("音乐助手"), "label_RadioHeading", this, "heading"));
    heading->addStretch();
    newConversationButton = button(QStringLiteral("新对话"), "button_ConversationNew", this);
    heading->addWidget(newConversationButton);
    newConversationButton->hide();
    connect(newConversationButton, &QPushButton::clicked, this, [this] {
        if (conversationMode && loggedIn && !busy && !submissionPending)
            emit taskActionRequested(QStringLiteral("new_conversation"));
    });
    loginButton = button(QStringLiteral("登录探歌"), "button_RadioLogin", this);
    heading->addWidget(loginButton);
    layout->addLayout(heading);
    layout->addWidget(label(QStringLiteral("场景找歌 · 歌手新作 · 口味记忆，听着再调整。"),
                            "label_RadioDescription", this, "subtitle"));
    availability = label(QString(), "label_RadioAvailability", this, "subtitle");
    quota = label(QString(), "label_RadioQuota", this, "subtitle");
    quota->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    quota->setMaximumWidth(360);
    quota->setAccessibleName(QStringLiteral("当前账号的 AI 使用额度"));
    quota->hide();
    auto *availabilityRow = new QHBoxLayout;
    availabilityRow->setSpacing(12);
    availabilityRow->addWidget(availability, 1);
    availabilityRow->addWidget(quota);
    layout->addLayout(availabilityRow);

    // 输入框固定在根布局，历史内容再长也不会把发送入口滚出窗口。
    auto *content = new QWidget(this);
    content->setObjectName(QStringLiteral("radioPageContent"));
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(7);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("scroll_RadioPage"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    scroll->setStyleSheet(QStringLiteral("QScrollArea#scroll_RadioPage { border: 0; background: #121614; }"
                                        "QWidget#radioPageContent { background: #121614; }"));
    outer->addWidget(scroll, 1);
    earlierMessages = button(QStringLiteral("加载更早消息"), "button_RadioEarlierMessages", content);
    earlierMessages->hide();
    layout->addWidget(earlierMessages, 0, Qt::AlignHCenter);
    connect(earlierMessages, &QPushButton::clicked, this, &RadioDialog::earlierMessagesRequested);

    taskPanel = new QWidget(this);
    taskPanel->setObjectName(QStringLiteral("panel_MusicTask"));
    auto *taskLayout = new QVBoxLayout(taskPanel);
    taskLayout->setContentsMargins(12, 10, 12, 10);
    taskLayout->setSpacing(6);
    taskSummary = label(QStringLiteral("描述一次听歌安排，可以同时指定场景、时长和歌手新作。"),
                        "label_TaskSummary", taskPanel);
    taskChecks = label(QString(), "label_TaskChecks", taskPanel, "subtitle");
    taskBudget = label(QString(), "label_TaskBudget", taskPanel, "subtitle");
    taskLayout->addWidget(taskSummary);
    taskLayout->addWidget(taskChecks);
    taskLayout->addWidget(taskBudget);
    auto *taskButtons = new QHBoxLayout;
    taskNew = button(QStringLiteral("新建安排"), "button_TaskNew", taskPanel);
    taskRestore = button(QStringLiteral("找回上次任务"), "button_TaskRestore", taskPanel);
    taskPause = button(QStringLiteral("暂停查找"), "button_TaskPause", taskPanel);
    taskResume = button(QStringLiteral("继续任务"), "button_TaskResume", taskPanel);
    taskButtons->addWidget(taskNew); taskButtons->addWidget(taskRestore);
    taskButtons->addWidget(taskPause); taskButtons->addWidget(taskResume);
    taskButtons->addStretch();
    taskLayout->addLayout(taskButtons);
    taskPanel->hide();
    layout->addWidget(taskPanel);
    for (const auto &entry : QVector<QPair<QPushButton *, QString>>{
             {taskNew, "new"}, {taskRestore, "restore"}, {taskPause, "pause"}, {taskResume, "resume"}})
        connect(entry.first, &QPushButton::clicked, this, [this, action = entry.second] {
            emit taskActionRequested(action);
        });

    preferencePanel = new QWidget(this);
    auto *preferenceLayout = new QHBoxLayout(preferencePanel);
    preferenceLayout->setContentsMargins(0, 0, 0, 0);
    preferenceLayout->setSpacing(8);
    preference = label(QString(), "label_RadioPreference", preferencePanel, "subtitle");
    preference->setMaximumHeight(40);
    preferenceLayout->addWidget(preference, 1);
    editPreferenceButton = button(QStringLiteral("编辑口味"), "button_RadioEditPreference", preferencePanel);
    preferenceLayout->addWidget(editPreferenceButton);
    clearPreferenceButton = button(QStringLiteral("清除口味"), "button_RadioClearPreference", preferencePanel);
    preferenceLayout->addWidget(clearPreferenceButton);
    layout->addWidget(preferencePanel);

    transcript = new QPlainTextEdit(this);
    transcript->setObjectName(QStringLiteral("text_RadioConversation"));
    transcript->setReadOnly(true);
    transcript->setPlaceholderText(QStringLiteral("可以聊此刻的心情，问歌手的新作，或告诉我想记住的音乐口味。"));
    transcript->setMinimumHeight(240);
    transcript->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    transcript->setAccessibleName(QStringLiteral("音乐助手对话"));
    transcript->hide();
    conversationView = new ConversationBrowser(this);
    conversationView->setObjectName(QStringLiteral("browser_RadioConversation"));
    conversationView->setOpenLinks(false);
    conversationView->setOpenExternalLinks(false);
    conversationView->setReadOnly(true);
    conversationView->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    conversationView->setMinimumHeight(240);
    conversationView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    conversationView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    conversationView->setAccessibleName(QStringLiteral("音乐助手聊天，用户在右侧，探歌回复在左侧"));
    conversationView->document()->setDocumentMargin(12);
    layout->addWidget(conversationView, 1);
    connect(conversationView, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        const auto playlist = conversationPlaylists.constFind(url.toString(QUrl::FullyEncoded));
        if (playlist != conversationPlaylists.cend()) {
            if (loggedIn && configured && !busy && !submissionPending && !conversationsClearing && artists.isEmpty())
                emit playlistActivated(qint64(playlist->value("id").toDouble()), playlist->value("name").toString());
            return;
        }
        if (!canActivateTrack()) return;
        const auto found = conversationTracks.constFind(url.toString(QUrl::FullyEncoded));
        if (found != conversationTracks.cend()) emit trackActivated(found.value());
    });

    progress = new QProgressBar(this);
    progress->setObjectName(QStringLiteral("progress_RadioGeneration"));
    progress->setRange(0, 0);
    progress->setTextVisible(false);
    progress->setFixedHeight(3);
    layout->addWidget(progress);
    stage = label(QString(), "label_RadioStage", this, "subtitle");
    layout->addWidget(stage);

    artistPanel = new QWidget(this);
    artistPanel->setObjectName(QStringLiteral("panel_RadioArtistChoices"));
    auto *artistLayout = new QHBoxLayout(artistPanel);
    artistLayout->setContentsMargins(0, 0, 0, 0);
    artistLayout->setSpacing(8);
    artistLayout->addWidget(label(QStringLiteral("请选择歌手"), "label_RadioArtistChoice", artistPanel));
    artistCombo = new QComboBox(artistPanel);
    artistCombo->setObjectName(QStringLiteral("combo_RadioArtistChoices"));
    artistCombo->setAccessibleName(QStringLiteral("同名歌手候选"));
    artistCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    artistCombo->setMinimumContentsLength(12);
    artistLayout->addWidget(artistCombo, 1);
    lookupArtistButton = button(QStringLiteral("查看新作"), "button_RadioLookupArtist", artistPanel);
    artistLayout->addWidget(lookupArtistButton);
    layout->addWidget(artistPanel);

    resultsPanel = new QWidget(content);
    resultsPanel->setObjectName(QStringLiteral("panel_RadioPreview"));
    auto *conversationLayout = layout;
    layout = new QVBoxLayout(resultsPanel);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(6);
    conversationLayout->addWidget(resultsPanel);
    playlistTitle = label(QStringLiteral("为此刻留一组歌"), "label_RadioPlaylistTitle", this);
    playlistNote = label(QStringLiteral("生成后先看看，点击开始收听才会切换音乐。"),
                          "label_RadioPlaylistNote", this, "subtitle");
    layout->addWidget(playlistTitle);
    layout->addWidget(playlistNote);
    table = new QTableWidget(0, 2, this);
    table->setObjectName(QStringLiteral("table_RadioRecommendations"));
    table->setHorizontalHeaderLabels({QStringLiteral("歌曲 / 歌手"), QStringLiteral("推荐理由")});
    table->setMinimumHeight(140);
    table->setMaximumHeight(200);
    table->horizontalHeader()->setMinimumSectionSize(120);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->setAccessibleName(QStringLiteral("音乐助手找到的歌曲，双击播放单首"));
    layout->addWidget(table, 3);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    playButton = button(QStringLiteral("开始收听"), "button_RadioPlay", this);
    playButton->setProperty("role", "primary");
    playButton->setToolTip(QStringLiteral("开始收听会替换当前播放队列，收藏和历史保留。"));
    actions->addWidget(playButton);
    rememberButton = button(QStringLiteral("记住口味"), "button_RadioRemember", this);
    rememberButton->setToolTip(QStringLiteral("先查看并编辑口味，再确认保存到当前账号的本机记忆；可随时清除。"));
    actions->addWidget(rememberButton);
    avoidArtistButton = button(QStringLiteral("这次避开歌手"), "button_RadioAvoidArtist", this);
    actions->addWidget(avoidArtistButton);
    taskLock = button(QStringLiteral("保留前三首"), "button_TaskLock", this);
    taskUndo = button(QStringLiteral("撤回修改"), "button_TaskUndo", this);
    actions->addWidget(taskLock);
    actions->addWidget(taskUndo);
    taskLock->hide(); taskUndo->hide();
    connect(taskLock, &QPushButton::clicked, this, [this] { emit taskActionRequested("lock"); });
    connect(taskUndo, &QPushButton::clicked, this, [this] { emit taskActionRequested("undo"); });
    actions->addStretch();
    layout->addLayout(actions);
    auto *queueHint = label(QStringLiteral("开始收听会替换当前播放队列，收藏和历史保留。"),
                            "label_RadioQueueHint", this, "subtitle");
    layout->addWidget(queueHint);
    playback = label(QString(), "label_RadioPlayback", this, "subtitle");
    layout->addWidget(playback);

    layout = conversationLayout;
    preferenceProposalPanel = new QWidget(this);
    preferenceProposalPanel->setObjectName(QStringLiteral("panel_RadioPreferenceProposal"));
    auto *proposalLayout = new QVBoxLayout(preferenceProposalPanel);
    proposalLayout->setContentsMargins(8, 6, 8, 6);
    proposalLayout->setSpacing(4);
    proposalNote = label(QString(), "label_RadioPreferenceProposal", preferenceProposalPanel, "subtitle");
    proposalLayout->addWidget(proposalNote);
    auto *proposalRow = new QHBoxLayout;
    proposedPreference = new QLineEdit(preferenceProposalPanel);
    proposedPreference->setObjectName(QStringLiteral("edit_RadioPreferenceProposal"));
    proposedPreference->setMaxLength(500);
    proposedPreference->setAccessibleName(QStringLiteral("待确认的本机长期音乐口味，可编辑，最多500字"));
    proposalRow->addWidget(proposedPreference, 1);
    confirmProposalButton = button(QStringLiteral("确认保存"), "button_RadioConfirmPreference", preferenceProposalPanel);
    cancelProposalButton = button(QStringLiteral("取消记忆"), "button_RadioCancelPreference", preferenceProposalPanel);
    proposalRow->addWidget(confirmProposalButton); proposalRow->addWidget(cancelProposalButton);
    proposalLayout->addLayout(proposalRow);
    preferenceProposalPanel->hide();
    outer->addWidget(preferenceProposalPanel);
    const auto decideProposal = [this](bool confirmed) {
        if (!loggedIn || proposalActionId.isEmpty() || proposalDecisionPending) return;
        proposalDecisionPending = true; updateControls();
        emit preferenceProposalDecided(proposalActionId, confirmed, proposedPreference->text().trimmed());
    };
    connect(confirmProposalButton, &QPushButton::clicked, this, [decideProposal] { decideProposal(true); });
    connect(cancelProposalButton, &QPushButton::clicked, this, [decideProposal] { decideProposal(false); });

    composerPanel = new QWidget(this);
    composerPanel->setObjectName(QStringLiteral("panel_RadioComposer"));
    auto *composerLayout = new QVBoxLayout(composerPanel);
    composerLayout->setContentsMargins(0, 8, 0, 0);
    composerLayout->setSpacing(4);
    auto *composer = new QHBoxLayout;
    auto *composerInput = new AssistantComposer(composerPanel);
    input = composerInput;
    input->setObjectName(QStringLiteral("edit_RadioInstruction"));
    input->setPlaceholderText(QStringLiteral("聊场景、问新歌、继续调整，或告诉我你的音乐口味…"));
    input->setAccessibleName(QStringLiteral("向音乐助手提问"));
    input->setAccessibleDescription(QStringLiteral("最多1000个UTF-16字符；Enter发送，Shift+Enter换行"));
    input->setMinimumHeight(78);
    input->setMaximumHeight(120);
    composer->addWidget(input, 1);
    auto *composerButtons = new QVBoxLayout;
    sendButton = button(QStringLiteral("发送"), "button_RadioGenerate", composerPanel);
    sendButton->setProperty("role", "primary");
    composerButtons->addWidget(sendButton);
    cancelButton = button(QStringLiteral("取消"), "button_RadioCancel", composerPanel);
    composerButtons->addWidget(cancelButton);
    routeObserveButton = button(QStringLiteral("观察原请求"), "button_RouteObserve", composerPanel);
    routeObserveButton->hide();
    composerButtons->addWidget(routeObserveButton);
    connect(routeObserveButton, &QPushButton::clicked, this, [this] {
        if (loggedIn && !routeObservationMessage.isEmpty()) emit routeObservationRequested(routeObservationMessage);
    });
    composerButtons->addStretch();
    composer->addLayout(composerButtons);
    composerLayout->addLayout(composer);
    inputCounter = label(QString(), "label_RadioInputCounter", composerPanel, "subtitle");
    inputCounter->setAlignment(Qt::AlignRight);
    composerLayout->addWidget(inputCounter);
    outer->addWidget(composerPanel);
    composerInput->send = [this] { submit(input->toPlainText()); };

    connect(loginButton, &QPushButton::clicked, this, &RadioDialog::loginRequested);
    connect(sendButton, &QPushButton::clicked, this, [this] { submit(input->toPlainText()); });
    connect(input, &QPlainTextEdit::textChanged, this, [this] { updateControls(); });
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (!busy) return;
        setBusy(false);
        stage->setText(taskMode ? QStringLiteral("正在请求取消，等待后台确认…") : QStringLiteral("已取消，正在播放的音乐继续。"));
        stage->show();
        emit cancelRequested();
    });
    connect(playButton, &QPushButton::clicked, this, [this] {
        if (!busy && !songs.isEmpty()) emit playRequested(songs);
    });
    connect(rememberButton, &QPushButton::clicked, this, &RadioDialog::rememberRequested);
    connect(clearPreferenceButton, &QPushButton::clicked, this, &RadioDialog::clearPreferenceRequested);
    connect(editPreferenceButton, &QPushButton::clicked, this, &RadioDialog::editPreference);
    connect(lookupArtistButton, &QPushButton::clicked, this, [this] {
        const int index = artistCombo->currentIndex();
        if (loggedIn && configured && !busy && index >= 0 && index < artists.size())
            emit artistSelected(artists.at(index));
    });
    connect(avoidArtistButton, &QPushButton::clicked, this, [this] {
        const QString artist = selectedArtist();
        if (!busy && !artist.isEmpty()) emit avoidArtistRequested(artist);
    });
    connect(table, &QTableWidget::itemSelectionChanged, this, [this] { updateControls(); });
    connect(table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (canActivateTrack() && row >= 0 && row < songs.size()) emit trackActivated(songs.at(row).song);
    });
    applyDialogStyle(this);
    setStyleSheet(styleSheet() + QStringLiteral("QPushButton[role=\"primary\"]:disabled {"
                                               "background: #1a201c; color: #526057; border-color: #252d27; }"));
    // 通用样式会清除控件样式，随后统一设置滚动视口，避免 macOS 默认浅色底。
    scroll->setStyleSheet(QStringLiteral("QScrollArea#scroll_RadioPage { border: 0; background: #121614; }"
                                        "QWidget#radioPageContent { background: #121614; }"));
    scroll->viewport()->setObjectName(QStringLiteral("viewport_RadioPage"));
    scroll->viewport()->setStyleSheet(QStringLiteral("QWidget#viewport_RadioPage { background: #121614; }"));
    content->setStyleSheet(QStringLiteral("QWidget#radioPageContent { background: #121614; }"));
    taskPanel->setStyleSheet(QStringLiteral("QWidget#panel_MusicTask { background: #191e1b; border-radius: 10px; }"));
    SongListDelegate::install(table, 0, artworkCache);
    connect(artworkCache, &CoverArtCache::imageReady, this, [this] {
        if (!conversationMode || artworkRefreshScheduled) return;
        artworkRefreshScheduled = true;
        QTimer::singleShot(0, this, [this] {
            artworkRefreshScheduled = false;
            if (conversationMode) renderConversation(conversationText);
        });
    });
    playlistTitle->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    progress->setStyleSheet(QStringLiteral("QProgressBar { border: none; background: #252d27; }"
                                          "QProgressBar::chunk { background: #b4d9a0; }"));
    transcript->setStyleSheet(QStringLiteral("QPlainTextEdit { border: none; background: #191e1b;"
                                            "border-radius: 10px; padding: 8px; color: #d4e1d8; }"));
    conversationView->setStyleSheet(QStringLiteral("QTextBrowser { border: 1px solid #29332d;"
        "border-radius: 14px; background: #131916; color: #e5ede7; padding: 5px; selection-background-color: #3e5947; }"));
    clearConversationsButton->setStyleSheet(QStringLiteral("QPushButton { color: #a9b5ac; background: transparent;"
        "border: 1px solid #344138; border-radius: 8px; padding: 6px 12px; }"
        "QPushButton:hover { color: #e7eee8; background: #26342b; border-color: #526b59; }"
        "QPushButton:disabled { color: #526057; border-color: #252d27; }"));
    input->setStyleSheet(QStringLiteral("QPlainTextEdit { background: #191e1b; color: #ecf0ea;"
                                       "border: 1px solid #354038; border-radius: 10px; padding: 9px; }"
                                       "QPlainTextEdit:focus { border-color: #b4d9a0; }"));
    artistCombo->setStyleSheet(QStringLiteral("QComboBox { color: #ecf0ea; background: #191e1b;"
                                             "border: 1px solid #354038; border-radius: 8px; padding: 7px 10px; }"
                                             "QComboBox QAbstractItemView { color: #ecf0ea; background: #191e1b;"
                                             "selection-background-color: #344b37; border: 1px solid #354038; }"));
    setPreference(QString());
    clearArtistChoices();
    setBusy(false);
    setPlaybackStatus(QString());
    setAvailability(false, false, QString());
}

void RadioDialog::setEmbedded(bool value)
{
    if (embedded == value) return;
    embedded = value;
    setProperty("embedded", value);
    if (value) {
        setMinimumSize(0, 0);
        layout()->setContentsMargins(24, 18, 24, 12);
    }
}

void RadioDialog::setTaskMode(bool enabled)
{
    taskMode = enabled;
    taskPanel->setVisible(enabled);
    taskLock->setVisible(enabled);
    taskUndo->setVisible(enabled);
    if (!enabled) { taskStatus.clear(); taskCanUndo = false; taskHasLocks = false; taskLockedIds.clear();
        taskConnectionInterrupted = false; taskLocalResumeRequired = false; taskConstraintWarnings.clear(); }
    playButton->setText(enabled ? QStringLiteral("采用并收听") : QStringLiteral("开始收听"));
    input->setPlaceholderText(enabled
        ? QStringLiteral("安排40分钟歌单并加入近期新作，或继续修改上次安排…")
        : QStringLiteral("聊场景、问新歌，或说“记住我喜欢华语 R&B”"));
    updateControls();
}

void RadioDialog::setConversation(const QJsonArray &conversation)
{
    messages.clear();
    QStringList lines;
    for (int i = conversationMode ? 0 : qMax(0, conversation.size() - maximumMessages); i < conversation.size(); ++i) {
        const auto item = conversation.at(i).toObject();
        const QString content = bounded(item.value("text").toString(item.value("content").toString()), maximumMessageLength);
        if (content.isEmpty()) continue;
        const QString role = item.value("role").toString();
        const QString speaker = role == "user" ? QStringLiteral("你") : QStringLiteral("探歌");
        messages.append(qMakePair(speaker, content));
        lines.append(speaker + QLatin1Char('\n') + content);
    }
    const QString text = lines.join(QStringLiteral("\n\n"));
    renderConversation(text);
}

void RadioDialog::setTaskSnapshot(const QJsonObject &snapshot)
{
    taskConstraintWarnings.clear();
    setTaskMode(true);
    taskStatus = snapshot.value("status").toString();
    taskLocalResumeRequired = taskStatus == QStringLiteral("waiting_tools") &&
        !snapshot.value("pendingSteps").toArray().isEmpty() &&
        snapshot.value("clientObservationOnly").toBool();
    taskCanUndo = snapshot.value("canUndo").toBool();
    taskLockedIds.clear();
    for (const auto &value : snapshot.value("lockedIds").toArray()) {
        const double id = value.toDouble(-1);
        if (value.isDouble() && std::isfinite(id) && id > 0 && id <= 9007199254740991. && std::floor(id) == id)
            taskLockedIds.insert(qint64(id));
        if (taskLockedIds.size() == 30) break;
    }
    taskHasLocks = !taskLockedIds.isEmpty();
    taskLock->setText(taskHasLocks ? QStringLiteral("解除歌曲锁定") : QStringLiteral("保留前三首"));
    const auto brief = snapshot.value("brief").toObject();
    const auto preview = snapshot.value("preview").toObject();
    const auto clientConnection = snapshot.value("clientConnection").toObject();
    taskConnectionInterrupted = !clientConnection.isEmpty();
    const auto connectionMessage = clientConnection.value("message").toString().left(1000);
    const bool taskFailed = taskStatus == QStringLiteral("failed") || taskStatus == QStringLiteral("budget_exhausted");
    const auto failure = snapshot.value("error").toObject();
    const auto recovery = snapshot.value("recovery").toObject();
    const QString recoveryMessage = bounded(recovery.value("message").toString(), 500);
    taskResume->setProperty("backendCanResume", recovery.contains("canResume")
        ? recovery.value("canResume").toBool() : true);
    QString summary = snapshot.value("briefSummary").toString();
    if (summary.isEmpty()) summary = brief.value("title").toString();
    if (summary.isEmpty()) summary = QStringLiteral("描述一次听歌安排，可以同时指定场景、时长和歌手新作。已保存的任务可在重新登录后找回。");
    if (taskFailed) {
        summary = QStringLiteral("本次安排未完成");
        const auto conversation = snapshot.value("messages").toArray();
        for (int i = conversation.size() - 1; i >= 0; --i) {
            const auto message = conversation.at(i).toObject();
            if (message.value("role").toString() != QStringLiteral("user")) continue;
            const QString request = message.value("content").toString(message.value("text").toString()).trimmed();
            if (!request.isEmpty()) summary += QStringLiteral(" · ") + bounded(request, 300);
            break;
        }
    }
    taskSummary->setText(bounded(summary, 420));
    taskSummary->setToolTip(summary.left(1500));
    QStringList checks;
    QStringList unresolved;
    for (const auto &value : preview.value("checks").toArray()) {
        const auto check = value.toObject();
        const QString status = check.value("status").toString();
        if (status == "not_requested") continue;
        const QString prefix = status == "satisfied" ? QStringLiteral("✓ ")
            : status == "unmet" ? QStringLiteral("未满足 · ") : QStringLiteral("待核验 · ");
        // 旧版成功检查不能覆盖之后的锁定配置；仅展示当前配置，不宣称重新验收成功。
        const bool currentLockConfiguration = check.value("key").toString() == QStringLiteral("locks") &&
            status == QStringLiteral("satisfied");
        const QString text = currentLockConfiguration
            ? (taskLockedIds.isEmpty() ? QStringLiteral("当前未锁定歌曲。")
                : QStringLiteral("当前锁定 %1 首歌曲，后续调整须保留。").arg(taskLockedIds.size()))
            : (taskFailed ? QStringLiteral("上一版 · ") : QString()) + prefix +
                check.value("detail").toString(check.value("text").toString()).left(240);
        if (status == "unmet") unresolved.prepend(text);
        else if (status == "unverified") unresolved.append(text);
        else checks.append(text);
    }
    // 聊天单独保留全部受限警告，不能沿用隐藏面板只展示前四项的截断。
    taskConstraintWarnings = unresolved.mid(0, 16);
    checks = unresolved + checks;
    const QString question = snapshot.value("question").toString(brief.value("question").toString());
    if (!question.isEmpty()) checks.prepend(question.left(400));
    const QString recoveryReason = snapshot.value("recoveryReason").toString();
    if (!recoveryReason.isEmpty()) checks.prepend(recoveryReason.left(400));
    if (taskFailed) {
        const QString code = bounded(failure.value("code").toString(), 100);
        const QString message = bounded(failure.value("message").toString(), 500);
        QString reason = QStringLiteral("失败原因");
        if (!code.isEmpty()) reason += QStringLiteral("（%1）").arg(code);
        reason += QStringLiteral("：") + (message.isEmpty()
            ? QStringLiteral("本次任务未完成，服务未返回具体说明。") : message);
        QString guidance = QStringLiteral("可修改要求后发送，或点击“继续任务”重试；继续可能产生新的模型调用费用。");
        if (code == QStringLiteral("MODEL_NOT_CONFIGURED"))
            guidance = QStringLiteral("请检查后台 AI 配置；配置完成后再点击“继续任务”。");
        else if (code == QStringLiteral("AI_DAILY_LIMIT") || code == QStringLiteral("AI_GLOBAL_DAILY_LIMIT"))
            guidance = QStringLiteral("调用额度已用完，请等待额度恢复后继续；也可以保留任务并修改要求。");
        else if (code == QStringLiteral("TASK_BUDGET"))
            guidance = QStringLiteral("达到任务调用上限，请缩小要求后发送；若仍达到总上限，请新建安排。新的执行可能产生调用费用。");
        if (!recoveryMessage.isEmpty()) guidance = recoveryMessage;
        checks.prepend(guidance);
        checks.prepend(reason);
    }
    else if (!recoveryMessage.isEmpty()) checks.prepend(recoveryMessage);
    if (taskConnectionInterrupted) checks.prepend(connectionMessage);
    const QString localResumeMessage = QStringLiteral("已恢复查找进度，点击继续后执行本机曲库查询");
    if (taskLocalResumeRequired) checks.prepend(localResumeMessage);
    taskChecks->setStyleSheet(taskFailed ? QStringLiteral("color: #e9b4a5;") : QString());
    taskChecks->setText(checks.mid(0, 4).join(QLatin1Char('\n')));
    taskChecks->setToolTip(checks.join(QLatin1Char('\n')));
    const auto budget = snapshot.value("budget").toObject();
    taskBudget->setText(snapshot.isEmpty() ? QString() : (taskFailed
        ? QStringLiteral("已保存失败进度 · 需求版本 %1 · 上次成功歌单版本 %2 · 本次模型调用 %3")
        : QStringLiteral("已保存任务 · 需求版本 %1 · 歌单版本 %2 · 本次模型调用 %3"))
        .arg(snapshot.value("revision").toInt()).arg(preview.value("version").toInt())
        .arg(budget.value("runCalls").toInt()));
    if (budget.value("cacheHits").toInt() > 0)
        taskBudget->setText(taskBudget->text() + QStringLiteral(" · 复用近期曲库结果 %1 次")
            .arg(budget.value("cacheHits").toInt()));
    if (!conversationMode) setConversation(snapshot.value("messages").toArray());
    QVector<AssistantArtist> choices;
    if (taskStatus == "needs_clarification") {
        for (const auto &value : snapshot.value("providedArtists").toArray()) {
            const auto row = value.toObject();
            AssistantArtist artist;
            artist.id = qint64(row.value("id").toDouble());
            artist.name = row.value("name").toString();
            const auto description = row.value("description").toString().trimmed().left(160);
            if (!description.isEmpty()) artist.aliases.append(description);
            if (artist.id > 0 && !artist.name.isEmpty()) choices.append(artist);
        }
    }
    setArtistChoices(choices);
    QVector<RadioRecommendation> recommendations;
    for (const auto &value : preview.value("tracks").toArray()) {
        const auto row = value.toObject();
        RadioRecommendation recommendation;
        recommendation.song.id = qint64(row.value("id").toDouble());
        recommendation.song.title = row.value("title").toString();
        recommendation.song.artist = row.value("artist").toString();
        recommendation.song.album = row.value("album").toString();
        recommendation.song.coverUrl = row.value("coverUrl").toString();
        recommendation.song.durationMs = row.value("durationMs").toInt();
        recommendation.reason = row.value("reason").toString();
        recommendation.sourceName = row.value("sourceName").toString();
        for (const auto &tag : row.value("sourceTags").toArray()) recommendation.sourceTags.append(tag.toString());
        recommendations.append(recommendation);
    }
    QString note = preview.value("note").toString();
    const qint64 duration = qint64(preview.value("totalDurationMs").toDouble());
    if (duration > 0) note.prepend(QStringLiteral("%1 分 %2 秒 · ").arg(duration / 60000).arg((duration / 1000) % 60));
    QString playlistName = preview.value("title").toString(brief.value("title").toString());
    if (taskFailed && !recommendations.isEmpty()) {
        playlistName = QStringLiteral("上次成功歌单 · ") + playlistName;
        note.prepend(QStringLiteral("这是上次成功的第 %1 版；本次修改失败，尚未生成新版本。 ")
            .arg(preview.value("version").toInt()));
    }
    setRecommendations(playlistName, note, recommendations);
    if (taskFailed && recommendations.isEmpty()) {
        playlistTitle->setText(QStringLiteral("尚未生成歌单"));
        playlistNote->setText(QStringLiteral("本次查找未完成，请先处理上方失败原因，再继续或修改要求。"));
    }
    const bool running = QStringList{"planning", "researching", "waiting_tools", "composing", "running"}.contains(taskStatus);
    const auto taskProgress = snapshot.value("progress").toObject();
    const bool currentProgress = taskProgress.value("taskId").toString() == snapshot.value("id").toString() &&
        taskProgress.value("revision").toInt() >= snapshot.value("revision").toInt() &&
        taskProgress.value("stateVersion").toDouble() >= snapshot.value("stateVersion").toDouble();
    QString progressMessage = currentProgress ? taskProgress.value("message").toString().trimmed() : QString();
    if (progressMessage.isEmpty() && currentProgress) {
        const auto progressStage = taskProgress.value("stage").toString();
        if (progressStage == "understanding") progressMessage = QStringLiteral("正在理解听歌要求…");
        else if (progressStage == "searching" || progressStage == "researching") progressMessage = QStringLiteral("正在查找真实曲库…");
        else if (progressStage == "checking") progressMessage = QStringLiteral("正在核对候选歌曲与要求…");
        else if (progressStage == "composing") progressMessage = QStringLiteral("正在整理歌单…");
    }
    if (progressMessage.isEmpty()) progressMessage = snapshot.value("statusText").toString();
    setBusy(running && !taskConnectionInterrupted && !taskLocalResumeRequired,
            taskLocalResumeRequired ? localResumeMessage : taskConnectionInterrupted ? connectionMessage : progressMessage);
    taskResume->setToolTip(taskLocalResumeRequired
        ? QStringLiteral("点击继续后执行尚未完成的本机曲库查询；不会重放已完成的模型请求。")
        : taskConnectionInterrupted ? QStringLiteral("先重新读取后台进度；不会自动重发任务或模型调用。") : recoveryMessage);
    updateControls();
}

void RadioDialog::hideEvent(QHideEvent *event)
{
    if (embedded && busy && !taskMode) {
        setBusy(false);
        emit cancelRequested();
    }
    QDialog::hideEvent(event);
}

void RadioDialog::setAvailability(bool accountLoggedIn, bool modelConfigured, const QString &status)
{
    loggedIn = accountLoggedIn;
    configured = modelConfigured;
    availabilityStatus = bounded(status, 320);
    if (!loggedIn) setAccountUsage({});
    if (!loggedIn)
        availability->setText(QStringLiteral("登录探歌后，即可让 AI 为你找歌。网易云授权用于歌曲播放。"));
    else if (!configured)
        availability->setText(availabilityStatus.isEmpty()
                                  ? QStringLiteral("AI 服务暂未就绪，请检查后台配置后重试。") : availabilityStatus);
    else
        availability->setText(availabilityStatus.isEmpty()
                                  ? QStringLiteral("AI 已就绪 · 歌曲完整播放以你的网易云账号权益为准。") : availabilityStatus);
    updateControls();
}

void RadioDialog::setAccountUsage(const QJsonObject &usage)
{
    const auto hide = [this] { quota->clear(); quota->setToolTip({}); quota->hide(); };
    if (!loggedIn || usage.isEmpty()) { hide(); return; }
    const auto daily = usage.value("daily").toObject();
    const auto total = usage.value("total").toObject();
    const auto count = [](const QJsonValue &value) -> qint64 {
        const auto number = value.toDouble(-1);
        return value.isDouble() && std::isfinite(number) && number >= 0 &&
            number <= 9007199254740991. && std::floor(number) == number ? qint64(number) : -1;
    };
    const auto dailyLimit = count(daily.value("limit")), dailyUsed = count(daily.value("used"));
    const auto dailyRemaining = count(daily.value("remaining"));
    const auto totalLimit = count(total.value("limit")), totalUsed = count(total.value("used"));
    const auto totalRemaining = count(total.value("remaining"));
    const auto resetText = daily.value("resetAt").toString();
    const auto resetAt = QDateTime::fromString(resetText, Qt::ISODate);
    if (!usage.value("disabled").isBool() || dailyLimit < 0 || dailyUsed < 0 || dailyRemaining < 0 ||
        totalLimit < 0 || totalUsed < dailyUsed || totalRemaining < 0 || resetText.size() > 64 || !resetAt.isValid() ||
        dailyRemaining != qMax<qint64>(0, dailyLimit - dailyUsed) ||
        totalRemaining != qMax<qint64>(0, totalLimit - totalUsed)) { hide(); return; }
    quota->setText(usage.value("disabled").toBool() ? QStringLiteral("AI 已停用") :
        QStringLiteral("今日剩余 %1/%2 次 · 内测剩余 %3 次").arg(dailyRemaining).arg(dailyLimit).arg(totalRemaining));
    quota->setToolTip(QStringLiteral("按模型尝试次数计数，不是歌曲数量或聊天条数。\n"
        "一次复杂请求可能使用多次；失败或结果不确定的尝试也计入。\n"
        "今日已用 %1/%2 次，内测累计已用 %3/%4 次。\n"
        "每日额度重置：%5（本机时间）；累计额度不随日期重置。")
        .arg(dailyUsed).arg(dailyLimit).arg(totalUsed).arg(totalLimit)
        .arg(resetAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
    quota->setVisible(!conversationMode);
}

void RadioDialog::setBusy(bool value, const QString &message)
{
    busy = value;
    progress->setVisible(busy);
    cancelButton->setVisible(busy);
    stage->setText(bounded(message.isEmpty() && busy ? QStringLiteral("正在为你寻找音乐，当前播放继续…") : message, 320));
    stage->setVisible(!stage->text().isEmpty());
    updateControls();
}

void RadioDialog::appendMessage(const QString &speaker, const QString &text)
{
    const QString content = bounded(text, maximumMessageLength);
    if (content.isEmpty()) return;
    messages.append(qMakePair(bounded(speaker, 24), content));
    while (!conversationMode && messages.size() > maximumMessages) messages.removeFirst();
    QStringList lines;
    for (const auto &message : messages)
        lines.append(message.first.isEmpty() ? message.second
                                            : message.first + QStringLiteral("\n") + message.second);
    // 歌名和模型文本一律按纯文本显示，不能解释成可执行标记。
    renderConversation(lines.join(QStringLiteral("\n\n")));
}

void RadioDialog::setPlaylistCandidates(const QJsonArray &playlists)
{
    playlistCandidates = {}; conversationPlaylists.clear(); ++recommendationRevision;
    QSet<qint64> seen;
    for (const auto &value : playlists) {
        const auto item = value.toObject();
        const auto number = item.value("id").toDouble();
        const auto name = item.value("name").toString().trimmed();
        if (!std::isfinite(number) || number < 1 || number > 9007199254740991. || std::floor(number) != number ||
            name.isEmpty() || name.size() > 160 || seen.contains(qint64(number))) continue;
        seen.insert(qint64(number));
        playlistCandidates.append(QJsonObject{{"id", number}, {"name", name},
            {"coverUrl", CoverArtCache::normalizedCoverUrl(item.value("coverUrl").toString()).toString()},
            {"trackCount", qBound(0, item.value("trackCount").toInt(), 100000)}});
        if (playlistCandidates.size() >= 5) break;
    }
    renderConversation(conversationText);
}

void RadioDialog::setRecommendations(const QString &title, const QString &note,
                                    const QVector<RadioRecommendation> &recommendations, int visibleLimit)
{
    playlistCandidates = {}; conversationPlaylists.clear();
    ++recommendationRevision;
    songs.clear();
    for (const auto &recommendation : recommendations) {
        if (recommendation.song.id <= 0) continue;
        songs.append(recommendation);
        if (songs.size() == (taskMode ? 40 : qBound(1, visibleLimit, 30))) break;
    }
    playlistTitle->setText(songs.isEmpty() ? QStringLiteral("暂时没有合适的歌曲")
                                         : bounded(title.isEmpty() ? QStringLiteral("为此刻找到 %1 首歌").arg(songs.size()) : title, 100));
    playlistNote->setText(bounded(note.isEmpty() ? (songs.isEmpty() ? QStringLiteral("试着换一种描述，或说一位你喜欢的歌手。")
                                                              : (conversationMode ? QStringLiteral("点击播放单首，也可以继续调整这组歌。")
                                                                                  : QStringLiteral("双击播放单首，也可以继续调整这组歌。"))) : note, 300));
    table->setRowCount(songs.size());
    for (int row = 0; row < songs.size(); ++row) {
        const auto &recommendation = songs.at(row);
        const auto &song = recommendation.song;
        auto *titleItem = new QTableWidgetItem(bounded(song.title.isEmpty() ? QStringLiteral("未命名歌曲") : song.title, 300));
        titleItem->setData(SongListDelegate::ArtistRole, bounded(song.artist, 180));
        titleItem->setData(SongListDelegate::SongIdRole, song.id);
        titleItem->setData(SongListDelegate::CoverUrlRole, song.coverUrl);
        titleItem->setData(SongListDelegate::MediaUrlRole, QUrl(QStringLiteral("https://music.163.com/song?id=%1").arg(song.id)));
        titleItem->setToolTip(bounded(song.title, 500) + QLatin1Char('\n') + bounded(song.artist, 250));
        table->setItem(row, 0, titleItem);
        auto *reason = new QTableWidgetItem(bounded(recommendation.reason, 300));
        QStringList details;
        if (!recommendation.reason.trimmed().isEmpty()) details.append(bounded(recommendation.reason, 500));
        if (!recommendation.sourceName.trimmed().isEmpty()) details.append(QStringLiteral("来源：") + bounded(recommendation.sourceName, 150));
        reason->setToolTip(details.join(QLatin1Char('\n')));
        table->setItem(row, 1, reason);
    }
    if (!songs.isEmpty()) table->selectRow(0);
    updateControls();
}

void RadioDialog::setPreference(const QString &summary)
{
    preferenceSummary = summary.trimmed().left(500);
    preference->setText(preferenceSummary.isEmpty()
                           ? QStringLiteral("口味记忆 · 本机当前账号，场景不会自动保存。")
                           : QStringLiteral("口味记忆 · 本机当前账号：") + bounded(preferenceSummary, 92));
    preference->setToolTip(preferenceSummary.isEmpty()
                              ? QStringLiteral("只有确认保存的音乐喜好会用于后续推荐。可编辑或清除，不会自动记住临时场景。")
                              : preferenceSummary);
    updateControls();
}

void RadioDialog::resetForAccount()
{
    ++accountRevision;
    clearPreferenceProposal(); setRouteObservationAvailable({});
    setAccountUsage({});
    submissionPending = false;
    pendingInstruction.clear();
    pendingSubmissionId = 0;
    earlierAvailable = false;
    conversationsClearing = false;
    earlierMessages->hide();
    messages.clear();
    conversationText.clear();
    transcript->clear();
    conversationTracks.clear();
    renderedConversationHtml.clear();
    conversationArtwork.clear();
    conversationView->clear();
    static_cast<AssistantComposer *>(input)->resetComposition();
    input->clear();
    setTaskMode(false);
    taskCanUndo = false;
    taskHasLocks = false;
    taskResume->setProperty("backendCanResume", QVariant());
    taskLock->setText(QStringLiteral("保留前三首"));
    taskSummary->setText(QStringLiteral("描述一次听歌安排，可以同时指定场景、时长和歌手新作。"));
    taskSummary->setToolTip(QString());
    taskChecks->clear();
    taskChecks->setToolTip(QString());
    taskChecks->setStyleSheet(QString());
    taskBudget->clear();
    clearArtistChoices();
    setRecommendations(QString(), QString(), {});
    playlistTitle->setText(QStringLiteral("为此刻留一组歌"));
    playlistNote->setText(QStringLiteral("生成后先看看，点击开始收听才会切换音乐。"));
    setPreference(QString());
    setPlaybackStatus(QString());
    setBusy(false);
    setAvailability(false, false, QString());
}

void RadioDialog::setArtistChoices(const QVector<AssistantArtist> &choices)
{
    artists.clear();
    artistCombo->clear();
    for (const auto &artist : choices) {
        if (artist.id <= 0 || artist.name.trimmed().isEmpty()) continue;
        bool duplicate = false;
        for (const auto &existing : artists) duplicate |= existing.id == artist.id;
        if (duplicate) continue;
        AssistantArtist safe = artist;
        safe.name = bounded(artist.name, 120);
        safe.aliases.clear();
        for (const auto &alias : artist.aliases.mid(0, 3)) {
            const QString value = bounded(alias, 80);
            if (!value.isEmpty()) safe.aliases.append(value);
        }
        // 受限的公开别名帮助区分同名歌手，不加载画像或暴露上游内部标识。
        QString display = safe.name;
        if (!safe.aliases.isEmpty()) display += QStringLiteral(" · ") + safe.aliases.join(QStringLiteral(" / "));
        artists.append(safe);
        artistCombo->addItem(display);
        artistCombo->setItemData(artists.size() - 1, display, Qt::ToolTipRole);
        if (artists.size() == 5) break;
    }
    updateControls();
}

void RadioDialog::clearArtistChoices()
{
    artists.clear();
    artistCombo->clear();
    updateControls();
}

void RadioDialog::editPreference()
{
    reviewPreference(preferenceSummary);
}

void RadioDialog::reviewPreference(const QString &proposal)
{
    if (!loggedIn || busy) return;
    const quint64 revision = accountRevision;
    QInputDialog editor(this);
    editor.setObjectName(QStringLiteral("dialog_RadioPreferenceEditor"));
    editor.setWindowTitle(QStringLiteral("确认音乐口味"));
    editor.setInputMode(QInputDialog::TextInput);
    editor.setLabelText(QStringLiteral("仅保存你希望长期记住的音乐喜好，最多 500 字。\n保存在当前探歌账号的本机记忆中；留空并保存可清除口味。"));
    editor.setTextValue(proposal.trimmed().left(500));
    editor.setOkButtonText(QStringLiteral("确认保存"));
    editor.setCancelButtonText(QStringLiteral("取消"));
    editor.resize(620, 170);
    if (auto *field = editor.findChild<QLineEdit *>()) {
        field->setObjectName(QStringLiteral("edit_RadioPreference"));
        field->setMaxLength(500);
        field->setAccessibleName(QStringLiteral("长期音乐口味，最多 500 字"));
        auto *entries = new QListWidget(&editor);
        entries->setObjectName(QStringLiteral("list_PreferenceItems"));
        entries->setMaximumHeight(110);
        entries->setAccessibleName(QStringLiteral("已确认音乐偏好条目，可选中一条移除"));
        auto *remove = new QPushButton(QStringLiteral("移除选中条目"), &editor);
        remove->setObjectName(QStringLiteral("button_RemovePreferenceItem"));
        remove->setAutoDefault(false);
        if (auto *box = qobject_cast<QVBoxLayout *>(editor.layout())) {
            box->insertWidget(1, entries);
            box->insertWidget(2, remove);
        }
        const auto refresh = [entries](const QString &text) {
            entries->clear();
            entries->addItems(text.split(QRegularExpression(QStringLiteral("[；;\\n]+")), Qt::SkipEmptyParts));
        };
        refresh(editor.textValue());
        connect(field, &QLineEdit::textChanged, &editor, refresh);
        connect(remove, &QPushButton::clicked, &editor, [&editor, entries] {
            const int row = entries->currentRow();
            if (row < 0) return;
            QStringList remaining;
            for (int i = 0; i < entries->count(); ++i)
                if (i != row) remaining.append(entries->item(i)->text());
            editor.setTextValue(remaining.join(QStringLiteral("；")));
        });
    }
    applyDialogStyle(&editor);
    if (editor.exec() == QDialog::Accepted && revision == accountRevision && loggedIn && !busy)
        emit preferenceEdited(editor.textValue().trimmed().left(500));
}

QVector<RadioRecommendation> RadioDialog::recommendations() const
{
    return songs;
}

void RadioDialog::setPlaybackStatus(const QString &status)
{
    playback->setText(bounded(status, 250));
    playback->setVisible(!playback->text().isEmpty());
    if (conversationMode) renderConversation(conversationText);
}

void RadioDialog::reject()
{
    if (!closing && !taskMode) {
        setBusy(false);
        emit cancelRequested();
    }
    QDialog::reject();
}

void RadioDialog::closeEvent(QCloseEvent *event)
{
    // 隐藏的复用窗口也能收到 close；不能只依赖可见窗口的 reject 来取消旧请求。
    closing = true;
    if (!taskMode) { setBusy(false); emit cancelRequested(); }
    QDialog::closeEvent(event);
    closing = false;
}

void RadioDialog::updateControls()
{
    const bool ready = loggedIn && configured && !conversationsClearing && (!busy || taskMode || conversationMode);
    conversationToolbar->setVisible(conversationMode);
    clearConversationsButton->setEnabled(conversationMode && loggedIn && !submissionPending && !conversationsClearing);
    clearConversationsButton->setText(conversationsClearing ? QStringLiteral("正在清除…") : QStringLiteral("清除全部对话"));
    newConversationButton->setVisible(conversationMode);
    newConversationButton->setEnabled(conversationMode && loggedIn && !busy && !submissionPending && !conversationsClearing);
    confirmProposalButton->setEnabled(loggedIn && !conversationsClearing && !proposalDecisionPending && !proposalActionId.isEmpty());
    cancelProposalButton->setEnabled(loggedIn && !conversationsClearing && !proposalDecisionPending && !proposalActionId.isEmpty());
    proposedPreference->setEnabled(loggedIn && !conversationsClearing && !proposalDecisionPending);
    routeObserveButton->setEnabled(loggedIn && !submissionPending);
    loginButton->setVisible(!loggedIn);
    loginButton->setEnabled(!busy);
    input->setEnabled(ready);
    const int inputLength = input->toPlainText().size();
    inputCounter->setText(QStringLiteral("Enter 发送 · Shift+Enter 换行    %1 / %2")
                          .arg(inputLength).arg(maximumInstructionLength));
    inputCounter->setStyleSheet(inputLength > maximumInstructionLength ? QStringLiteral("color: #e9b4a5;") : QString());
    sendButton->setEnabled(ready && !submissionPending && inputLength <= maximumInstructionLength &&
                           !input->toPlainText().trimmed().isEmpty());
    cancelButton->setEnabled(busy);
    resultsPanel->setVisible(!songs.isEmpty() || taskStatus == QStringLiteral("failed") ||
                             taskStatus == QStringLiteral("budget_exhausted"));
    preferencePanel->setVisible(!preferenceSummary.isEmpty());
    playButton->setEnabled(!busy && !conversationsClearing && !songs.isEmpty());
    rememberButton->setEnabled(ready && !songs.isEmpty());
    clearPreferenceButton->setEnabled(loggedIn && !busy && !preferenceSummary.isEmpty());
    editPreferenceButton->setEnabled(loggedIn && !busy);
    artistPanel->setVisible(!artists.isEmpty());
    artistCombo->setEnabled(ready);
    lookupArtistButton->setEnabled(ready && !artists.isEmpty());
    avoidArtistButton->setEnabled(ready && !selectedArtist().isEmpty());
    table->setEnabled(!busy);
    taskRestore->setEnabled(loggedIn);
    taskNew->setEnabled(loggedIn && !busy);
    taskPause->setEnabled(loggedIn && busy);
    const auto canResume = taskResume->property("backendCanResume");
    taskResume->setEnabled(loggedIn && !busy && (taskConnectionInterrupted || taskLocalResumeRequired ||
        ((!canResume.isValid() || canResume.toBool()) && QStringList{"paused", "failed", "budget_exhausted", "waiting_tools"}.contains(taskStatus))));
    taskLock->setEnabled(ready && !busy && !songs.isEmpty());
    taskUndo->setEnabled(ready && !busy && taskCanUndo);
    if (conversationMode) {
        taskPanel->setVisible(taskMode && !taskStatus.isEmpty());
        taskBudget->hide();
        taskPause->setVisible(busy);
        taskResume->setVisible(taskResume->isEnabled());
        taskRestore->setVisible(taskStatus.isEmpty() || taskConnectionInterrupted ||
            QStringList{"paused", "failed", "budget_exhausted"}.contains(taskStatus));
    } else {
        taskPanel->setVisible(taskMode);
        taskBudget->show();
        taskPause->show();
        taskResume->show();
        taskRestore->show();
    }
    if (taskMode)
        playButton->setEnabled(!busy && !conversationsClearing && !songs.isEmpty() &&
            (taskStatus == "preview" || taskStatus == "awaiting_apply" || taskStatus == "adopted"));
    if (conversationMode) {
        // 当前产品入口只有聊天与输入框。旧控件保留为信号适配，不再占据页面。
        for (auto *widget : QVector<QWidget *>{newConversationButton, loginButton, availability, quota,
                 taskPanel, preferencePanel, artistPanel, resultsPanel, preferenceProposalPanel,
                 progress, stage, cancelButton, routeObserveButton, inputCounter}) widget->hide();
        findChild<QLabel *>(QStringLiteral("label_RadioHeading"))->hide();
        findChild<QLabel *>(QStringLiteral("label_RadioDescription"))->hide();
        findChild<QWidget *>(QStringLiteral("panel_RadioHeader"))->hide();
        earlierMessages->hide();
        renderConversation(conversationText);
    }
}

void RadioDialog::submit(const QString &instruction)
{
    const QString text = instruction.trimmed();
    if (!loggedIn || !configured || conversationsClearing || (busy && !taskMode && !conversationMode) || submissionPending ||
        text.isEmpty() || instruction.size() > maximumInstructionLength) return;
    submissionPending = true;
    pendingInstruction = text;
    pendingDraftRevision = input->document()->revision();
    pendingAccountRevision = accountRevision;
    pendingSubmissionId = ++submissionSerial;
    updateControls();
    emit generateRequested(text);
}

void RadioDialog::setConversationMode(bool enabled)
{
    if (conversationMode != enabled) {
        conversationMode = enabled;
        auto *scroll = findChild<QScrollArea *>(QStringLiteral("scroll_RadioPage"));
        auto *outer = qobject_cast<QVBoxLayout *>(layout());
        auto *contentLayout = qobject_cast<QVBoxLayout *>(scroll->widget()->layout());
        if (enabled) {
            contentLayout->removeWidget(conversationView);
            outer->insertWidget(2, conversationView, 1);
            conversationView->setMinimumHeight(120);
            conversationView->show();
            scroll->hide();
        } else {
            outer->removeWidget(conversationView);
            contentLayout->insertWidget(3, conversationView, 1);
            conversationView->setMinimumHeight(240);
            scroll->show();
            findChild<QLabel *>(QStringLiteral("label_RadioHeading"))->show();
            findChild<QLabel *>(QStringLiteral("label_RadioDescription"))->show();
            findChild<QWidget *>(QStringLiteral("panel_RadioHeader"))->show();
            availability->show();
            inputCounter->show();
            renderConversation(conversationText);
        }
    }
    updateControls();
}

void RadioDialog::setConversationsClearing(bool clearing)
{
    conversationsClearing = clearing;
    updateControls();
}

void RadioDialog::setEarlierMessagesAvailable(bool available)
{
    earlierAvailable = available;
    earlierMessages->setVisible(available && !conversationMode);
    if (conversationMode) renderConversation(conversationText);
}

quint64 RadioDialog::currentSubmissionId() const
{
    return pendingSubmissionId;
}

void RadioDialog::submissionAccepted(const QString &instruction, quint64 submissionId)
{
    if (!submissionPending || instruction.trimmed() != pendingInstruction ||
        pendingAccountRevision != accountRevision ||
        (submissionId && submissionId != pendingSubmissionId)) return;
    const bool unchanged = input->document()->revision() == pendingDraftRevision;
    submissionPending = false;
    pendingInstruction.clear();
    pendingSubmissionId = 0;
    if (unchanged) input->clear();
    updateControls();
}

void RadioDialog::submissionFailed(const QString &instruction, quint64 submissionId)
{
    if (!submissionPending || instruction.trimmed() != pendingInstruction ||
        pendingAccountRevision != accountRevision ||
        (submissionId && submissionId != pendingSubmissionId)) return;
    submissionPending = false;
    pendingInstruction.clear();
    pendingSubmissionId = 0;
    updateControls();
}

void RadioDialog::renderConversation(const QString &text)
{
    conversationText = text;
    QString presentation = text;
    if (conversationMode) {
        const auto context = chatContext();
        if (!context.isEmpty()) presentation += (presentation.isEmpty() ? QString() : QStringLiteral("\n\n")) + context;
    }
    const auto replacePreservingPosition = [](QAbstractScrollArea *view, const QString &previousText,
                                             const QString &nextText, const std::function<void()> &replace) {
        auto *scroll = view->verticalScrollBar();
        const int previous = scroll->value();
        const int previousMaximum = scroll->maximum();
        const bool followLatest = previousMaximum - previous <= 24;
        const bool prepended = !previousText.isEmpty() && nextText.endsWith(previousText);
        replace();
        // 补载历史保留正在阅读的位置；仅当用户已接近底部时跟随新回复。
        const int preserved = prepended ? previous + scroll->maximum() - previousMaximum : previous;
        scroll->setValue(followLatest && !prepended ? scroll->maximum() : qBound(0, preserved, scroll->maximum()));
    };
    if (transcript->toPlainText() != presentation)
        replacePreservingPosition(transcript, transcript->toPlainText(), presentation,
            [this, presentation] { transcript->setPlainText(presentation); });
    const QString html = conversationHtml();
    if (renderedConversationHtml != html) {
        const QString previousText = conversationView->toPlainText();
        // 先通过临时纯文本转换判断补载位置，不使用隐藏诊断控件的滚动状态。
        QTextDocument next;
        next.setHtml(html);
        replacePreservingPosition(conversationView, previousText, next.toPlainText(),
            [this, html] { conversationView->setHtml(html); });
        renderedConversationHtml = html;
    }
    // 相同HTML也要刷新异步解码的像素；固定行槽位最多40首/5歌单，避免资源随轮次增长。
    for (auto it = conversationArtwork.cbegin(); it != conversationArtwork.cend(); ++it)
        conversationView->document()->addResource(QTextDocument::ImageResource, QUrl(it.key()), it.value());
    conversationView->document()->markContentsDirty(0, conversationView->document()->characterCount());
    conversationView->viewport()->update();
}

QString RadioDialog::artworkHtml(const QString &kind, int row, const QString &coverUrl)
{
    const QString slot = QStringLiteral("tango-artwork:/%1/%2").arg(kind).arg(row);
    QImage image;
    const auto safe = CoverArtCache::normalizedCoverUrl(coverUrl);
    // 空封面不按ID补查元数据，歌单ID也不能被当成歌曲ID。
    if (artworkCache && !safe.isEmpty()) image = artworkCache->image({}, 0, safe.toString());
    if (image.isNull()) {
        image = QImage(48, 48, QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(QStringLiteral("#24382b")));
        QPainter painter(&image);
        painter.setPen(QColor(QStringLiteral("#bfdcad")));
        QFont font = painter.font(); font.setPixelSize(25); painter.setFont(font);
        painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("♪"));
    } else image = image.scaled(48, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    conversationArtwork.insert(slot, image);
    return QStringLiteral("<img src=\"%1\" width=\"48\" height=\"48\"/>").arg(slot);
}

bool RadioDialog::canActivateTrack() const
{
    const bool currentPreview = !taskMode || QStringList{"preview", "awaiting_apply", "adopted"}.contains(taskStatus);
    return loggedIn && configured && !busy && !submissionPending && !conversationsClearing &&
        artists.isEmpty() && currentPreview && !songs.isEmpty();
}

QString RadioDialog::conversationHtml()
{
    conversationTracks.clear();
    conversationPlaylists.clear();
    conversationArtwork.clear();
    QString body;
    for (const auto &message : messages) {
        const bool user = message.first == QStringLiteral("你") || message.first == QStringLiteral("user");
        body += chatBubble(user ? QStringLiteral("你") : QStringLiteral("探歌"), htmlText(message.second), user);
    }
    if (conversationMode) {
        if (!playlistCandidates.isEmpty()) {
            QString cards;
            const bool enabled = loggedIn && configured && !busy && !submissionPending && !conversationsClearing && artists.isEmpty();
            for (int i = 0; i < playlistCandidates.size(); ++i) {
                const auto item = playlistCandidates.at(i).toObject();
                const auto link = QStringLiteral("tango-playlist://show/%1/%2/%3")
                    .arg(accountRevision).arg(recommendationRevision).arg(i);
                const auto text = QStringLiteral("%1. %2<br/><span style=\"font-size:12px;\">%3 首</span>")
                    .arg(i + 1).arg(htmlText(item.value("name").toString())).arg(item.value("trackCount").toInt());
                QString image = artworkHtml(QStringLiteral("playlist"), i, item.value("coverUrl").toString());
                QString content = text;
                if (enabled) {
                    conversationPlaylists.insert(link, item);
                    content = QStringLiteral("<a href=\"%1\" style=\"color:#c4e9bb;text-decoration:none;\">%2</a>").arg(link, text);
                    image = QStringLiteral("<a href=\"%1\">%2</a>").arg(link, image);
                }
                cards += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\"><tr><td width=\"60\">%1</td><td>%2</td></tr></table><br/>").arg(image, content);
            }
            cards += QStringLiteral("<span style=\"font-size:11px;color:#9cad9f;\">点击歌单查看歌曲；不会自动播放。</span>");
            body += chatBubble(QStringLiteral("探歌 · 找到的歌单"), cards, false);
        }
        const auto context = chatContext(false);
        if (!context.isEmpty()) {
            for (QString section : context.split(QStringLiteral("\n\n"), Qt::SkipEmptyParts)) {
                QString speaker = QStringLiteral("探歌");
                if (section.startsWith(QStringLiteral("探歌"))) {
                    const int newline = section.indexOf(QLatin1Char('\n'));
                    if (newline >= 0) { speaker = section.left(newline); section = section.mid(newline + 1); }
                }
                body += chatBubble(speaker, htmlText(section), false);
            }
        }
        const bool currentPreview = !taskMode || QStringList{"preview", "awaiting_apply", "adopted"}.contains(taskStatus);
        if (!songs.isEmpty() && currentPreview && artists.isEmpty()) {
            QString tracks = QStringLiteral("<b>%1</b><br/><span style=\"font-size:12px;color:#a5b6a8;\">%2</span><br/><br/>")
                .arg(htmlText(playlistTitle->text()), htmlText(playlistNote->text()));
            const bool enabled = canActivateTrack();
            for (int i = 0; i < songs.size(); ++i) {
                const auto &song = songs.at(i).song;
                QString metadata = bounded(song.artist, 100);
                if (!song.album.trimmed().isEmpty()) metadata += QStringLiteral(" · ") + bounded(song.album, 100);
                if (song.version == QStringLiteral("studio")) metadata += QStringLiteral(" · 录音室版");
                else if (song.version == QStringLiteral("live")) metadata += QStringLiteral(" · 现场版");
                if (song.durationMs > 0) metadata += QStringLiteral(" · %1:%2").arg(song.durationMs / 60000)
                    .arg((song.durationMs / 1000) % 60, 2, 10, QLatin1Char('0'));
                if (taskMode && taskLockedIds.contains(song.id)) metadata += QStringLiteral(" · 已锁定");
                const QString row = QStringLiteral("▶  %1. %2<br/><span style=\"font-size:12px;\">&nbsp;&nbsp;&nbsp;&nbsp;%3</span>")
                    .arg(i + 1).arg(htmlText(bounded(song.title.isEmpty() ? QStringLiteral("未命名歌曲") : song.title, 180)), htmlText(metadata));
                QString image = artworkHtml(QStringLiteral("song"), i, song.coverUrl);
                QString content;
                if (enabled) {
                    const QString link = QStringLiteral("tango-track://play/%1/%2/%3")
                        .arg(accountRevision).arg(recommendationRevision).arg(i);
                    conversationTracks.insert(link, song);
                    content = QStringLiteral("<a href=\"%1\" style=\"color:#c4e9bb;text-decoration:none;\">%2</a>").arg(link, row);
                    image = QStringLiteral("<a href=\"%1\">%2</a>").arg(link, image);
                } else content = QStringLiteral("<span style=\"color:#899b8e;\">%1</span>").arg(row);
                tracks += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\"><tr><td width=\"60\">%1</td><td>%2</td></tr></table><br/>").arg(image, content);
            }
            tracks += QStringLiteral("<span style=\"font-size:11px;color:#9cad9f;\">%1</span>")
                .arg(enabled ? QStringLiteral("点击歌曲播放单首 · 输入“采用歌单”替换待播放队列<br/>完整播放取决于你的网易云账号权益。")
                             : QStringLiteral("当前暂不可播放，请等待本轮完成或处理确认。"));
            body += chatBubble(QStringLiteral("探歌 · 歌曲推荐"), tracks, false);
        }
        const QString warning = constraintWarningText();
        if (!warning.isEmpty())
            body += chatBubble(QStringLiteral("探歌 · 条件核验"), htmlText(warning), false);
    }
    if (body.isEmpty()) body = QStringLiteral("<p align=\"center\" style=\"margin-top:64px;color:#c1d1c4;font-size:20px;\">"
        "此刻，想听什么？</p><p align=\"center\" style=\"color:#81978a;font-size:13px;\">"
        "说说心情，找一首歌，或聊聊你喜欢的音乐。</p>");
    return QStringLiteral("<html><body style=\"font-family:'PingFang SC','Microsoft YaHei',sans-serif;\">%1</body></html>").arg(body);
}

QString RadioDialog::chatContext(bool includeSongs) const
{
    QStringList sections;
    if (!proposalActionId.isEmpty()) {
        const bool clearing = proposalOperation == QStringLiteral("clear");
        sections.append(QStringLiteral("探歌 · 待你确认\n") + (clearing
            ? QStringLiteral("准备清除当前账号在本机保存的长期口味。输入“确认清除口味”才会清除；输入“取消口味”保留。")
            : QStringLiteral("准备保存的长期口味：%1\n输入“确认保存口味”才会写入；输入“取消口味”放弃。").arg(proposedPreference->text())));
    }
    if (!artists.isEmpty()) {
        QStringList choices;
        for (int i = 0; i < artists.size(); ++i) {
            QString detail = artists.at(i).name;
            if (!artists.at(i).aliases.isEmpty()) detail += QStringLiteral(" · ") + artists.at(i).aliases.join(QStringLiteral(" / "));
            choices.append(QStringLiteral("%1. %2").arg(i + 1).arg(detail));
        }
        sections.append(QStringLiteral("探歌 · 请确认歌手\n") + choices.join(QLatin1Char('\n')) +
            QStringLiteral("\n输入“选择歌手 1”等候选序号后，我再继续查找。"));
    }
    if (busy && !stage->text().isEmpty())
        sections.append(QStringLiteral("探歌\n") + stage->text() + QStringLiteral("\n可继续输入要求，或发送“暂停任务”“取消任务”。"));
    else if (taskMode && (taskConnectionInterrupted || taskLocalResumeRequired ||
             QStringList{"failed", "budget_exhausted", "paused", "needs_clarification"}.contains(taskStatus)) && !taskChecks->text().isEmpty()) {
        QStringList currentChecks;
        for (const auto &line : taskChecks->text().split(QLatin1Char('\n')))
            // 权威问题已同步进历史时不重复展示；同步前仍让用户看到需要补充什么。
            if (taskStatus != QStringLiteral("needs_clarification") || !conversationText.contains(line)) currentChecks.append(line);
        if (!currentChecks.isEmpty()) sections.append(QStringLiteral("探歌 · 当前任务\n") + currentChecks.join(QLatin1Char('\n'))
            .replace(QStringLiteral("点击“继续任务”"), QStringLiteral("发送“继续任务”"))
            .replace(QStringLiteral("点击继续后执行"), QStringLiteral("发送“继续任务”后执行")));
    }
    const bool currentPreview = !taskMode || QStringList{"preview", "awaiting_apply", "adopted"}.contains(taskStatus);
    if (includeSongs && !songs.isEmpty() && currentPreview && artists.isEmpty()) {
        QStringList tracks;
        int lockedCount = 0;
        for (int i = 0; i < songs.size(); ++i) {
            const auto &song = songs.at(i).song;
            QString line = QStringLiteral("%1. %2 · %3").arg(i + 1)
                .arg(bounded(song.title, 180), bounded(song.artist, 100));
            // 只展示曲库明确给出的规范版本；没标现场不能推断为录音室版。
            if (song.version == QStringLiteral("studio")) line += QStringLiteral(" · 录音室版");
            else if (song.version == QStringLiteral("live")) line += QStringLiteral(" · 现场版");
            // 同名同歌手可能属于不同发行，专辑与时长也保留供用户辨认。
            if (!song.album.trimmed().isEmpty()) line += QStringLiteral(" · 《%1》").arg(bounded(song.album, 100));
            if (song.durationMs > 0) line += QStringLiteral(" · %1:%2").arg(song.durationMs / 60000)
                .arg((song.durationMs / 1000) % 60, 2, 10, QLatin1Char('0'));
            // 锁定动作不会重算旧版 checks；仅用最新任务快照标记真实保留状态。
            if (taskMode && taskLockedIds.contains(song.id)) {
                line += QStringLiteral(" · 已锁定");
                ++lockedCount;
            }
            tracks.append(line);
        }
        const QString lockSummary = taskMode ? (lockedCount > 0
            ? QStringLiteral("\n已锁定 %1 首歌曲，后续调整会保留；输入“解除锁定”可取消。").arg(lockedCount)
            : QStringLiteral("\n当前未锁定歌曲。")) : QString();
        sections.append(QStringLiteral("探歌 · 歌曲预览\n") + playlistTitle->text() + QLatin1Char('\n') +
            playlistNote->text() + QLatin1Char('\n') + tracks.join(QLatin1Char('\n')) + lockSummary +
            QStringLiteral("\n输入“采用歌单”会替换待播放队列；输入“播放第1首”可播放单首。完整播放取决于你的网易云账号权益。"));
    } else if (includeSongs && !songs.isEmpty() && taskMode && !currentPreview && !busy) {
        sections.append(QStringLiteral("探歌\n已有歌单来自上一版本；本轮尚未完成，暂不提供采用或播放操作。"));
    }
    if (includeSongs && !constraintWarningText().isEmpty())
        sections.append(QStringLiteral("探歌 · 条件核验\n") + constraintWarningText());
    if (!routeObservationMessage.isEmpty())
        sections.append(QStringLiteral("探歌\n原请求结果尚未确认。输入“观察原请求”读取状态，不会重新发送付费请求。"));
    if (earlierAvailable) sections.append(QStringLiteral("输入“加载更早消息”可查看此前对话。"));
    if (!playback->text().isEmpty()) sections.append(QStringLiteral("探歌\n") + playback->text());
    if ((!loggedIn || !configured) && !conversationText.isEmpty()) sections.append(availability->text());
    return sections.join(QStringLiteral("\n\n"));
}

QString RadioDialog::constraintWarningText() const
{
    if (!taskMode || busy || !artists.isEmpty() || taskConstraintWarnings.isEmpty() ||
        !QStringList{"preview", "awaiting_apply", "adopted"}.contains(taskStatus)) return {};
    return QStringLiteral("这份预览仍有未满足或未核验的条件：\n") + taskConstraintWarnings.join(QLatin1Char('\n'));
}

bool RadioDialog::executeChatAction(const QString &instruction)
{
    if (!conversationMode || !loggedIn || conversationsClearing) return false;
    const QString text = instruction.trimmed();
    const auto artist = QRegularExpression(QStringLiteral("^选择歌手\\s*([1-5])$")).match(text);
    if (artist.hasMatch()) {
        const int index = artist.captured(1).toInt() - 1;
        if (!configured || busy || index < 0 || index >= artists.size()) return false;
        emit artistSelected(artists.at(index));
        return true;
    }
    if (!artists.isEmpty() && !busy && configured) {
        // 自然语言消歧只在当前候选中选唯一身份，不从模型或用户文本创造歌手ID。
        const auto choice = QRegularExpression(QStringLiteral("^(?:就是|我说的是|我要|我选|选择|选)(.+?)(?:那位|那个|的)?(?:[，,](?:不是|不要|不选).+)?[。！!]?$"))
            .match(text);
        if (choice.hasMatch()) {
            const auto positive = choice.captured(1).trimmed();
            const auto work = QRegularExpression(QStringLiteral("[《“\"]([^》”\"]{2,60})[》”\"]")).match(positive);
            QString identity = work.hasMatch() ? work.captured(1) : positive;
            identity.remove(QRegularExpression(QStringLiteral("^(?:做|唱|演唱|创作|发布|有作品|作品是|作品|是)")));
            identity.remove(QRegularExpression(QStringLiteral("(?:的|那位|那个)$")));
            const auto norm = [](QString value) {
                value = value.normalized(QString::NormalizationForm_KC).toCaseFolded();
                value.remove(QRegularExpression(QStringLiteral("[\\s\\p{P}\\p{S}]+"))); return value;
            };
            QVector<int> matches;
            const auto wanted = norm(identity);
            const bool unsafe = QRegularExpression(QStringLiteral("不是|不要|不选|如果|假如|假设|他说|她说|引用|或者|还是|也许|可能|忽略|执行|调用"))
                .match(positive).hasMatch();
            if (!unsafe && wanted.size() >= 2)
                for (int i = 0; i < artists.size(); ++i) {
                    bool same = norm(artists.at(i).name) == wanted;
                    for (const auto &alias : artists.at(i).aliases) same |= norm(alias).contains(wanted);
                    if (same) matches.append(i);
                }
            if (matches.size() == 1) { emit artistSelected(artists.at(matches.first())); return true; }
        }
    }
    if (text == QStringLiteral("采用歌单") || text == QStringLiteral("开始收听")) {
        if (!playButton->isEnabled() || !artists.isEmpty()) return false;
        emit playRequested(songs);
        return true;
    }
    if (text == QStringLiteral("保留前三首") || text == QStringLiteral("锁定前三首")) {
        if (!taskMode || !taskLock->isEnabled() || !artists.isEmpty()) return false;
        emit taskActionRequested(QStringLiteral("lock_first_three"));
        return true;
    }
    if (text == QStringLiteral("解除歌曲锁定") || text == QStringLiteral("解除锁定")) {
        if (!taskMode || !taskLock->isEnabled() || !artists.isEmpty()) return false;
        emit taskActionRequested(QStringLiteral("unlock"));
        return true;
    }
    const auto track = QRegularExpression(QStringLiteral("^播放第\\s*([0-9]{1,2}|[一二三四五六七八九十])\\s*首$")).match(text);
    if (track.hasMatch()) {
        const auto number = track.captured(1);
        const QString chinese = QStringLiteral("一二三四五六七八九十");
        const int index = number.front().isDigit() ? number.toInt() - 1 : chinese.indexOf(number);
        if (!playButton->isEnabled() || !artists.isEmpty() || index < 0 || index >= songs.size()) return false;
        emit trackActivated(songs.at(index).song);
        return true;
    }
    if (text == QStringLiteral("确认保存口味") || text == QStringLiteral("确认清除口味")) {
        const QString operation = text == QStringLiteral("确认清除口味") ? QStringLiteral("clear") : QStringLiteral("set");
        if (proposalOperation != operation || !confirmProposalButton->isEnabled()) return false;
        confirmProposalButton->click();
        return true;
    }
    const bool naturalMemoryWithdrawal = QRegularExpression(QStringLiteral("^(?:不记了|不保存了|别记了|不要记了|不保存这个偏好)(?:[，,](?:我)?撤回刚才(?:那条|的(?:口味|偏好|记忆|提议)))?(?:[，,](?:只(?:在)?这次聊聊|只用于这次|这次聊聊就好))?[。！!]?$"))
        .match(text).hasMatch();
    if (text == QStringLiteral("取消口味") || text == QStringLiteral("取消记忆") || naturalMemoryWithdrawal) {
        if (!cancelProposalButton->isEnabled()) return false;
        cancelProposalButton->click();
        return true;
    }
    if (text == QStringLiteral("新对话")) {
        if (!newConversationButton->isEnabled()) return false;
        emit taskActionRequested(QStringLiteral("new_conversation"));
        return true;
    }
    if (text == QStringLiteral("加载更早消息") && earlierAvailable) {
        emit earlierMessagesRequested();
        return true;
    }
    if (text == QStringLiteral("观察原请求") && !routeObservationMessage.isEmpty()) {
        emit routeObservationRequested(routeObservationMessage);
        return true;
    }
    return false;
}

QString RadioDialog::selectedArtist() const
{
    const int row = table->currentRow();
    return row >= 0 && row < songs.size() ? songs.at(row).song.artist.trimmed() : QString();
}

void RadioDialog::setPreferenceProposal(const QString &actionId, const QString &operation, const QString &proposal)
{
    proposalActionId = actionId;
    proposalOperation = operation;
    proposalDecisionPending = false;
    proposedPreference->setText(proposal.left(500));
    proposedPreference->setReadOnly(operation == QStringLiteral("clear"));
    proposalNote->setText(operation == QStringLiteral("clear")
        ? QStringLiteral("是否清除当前账号在本机保存的长期口味？取消不会影响独立的找歌请求。")
        : QStringLiteral("确认后才保存到当前账号的本机长期记忆；可以编辑。取消不会影响独立的找歌请求。"));
    confirmProposalButton->setText(operation == QStringLiteral("clear") ? QStringLiteral("确认清除") : QStringLiteral("确认保存"));
    preferenceProposalPanel->setVisible(!actionId.isEmpty());
    updateControls();
}
void RadioDialog::clearPreferenceProposal(const QString &actionId)
{
    if (!actionId.isEmpty() && actionId != proposalActionId) return;
    proposalActionId.clear(); proposalOperation.clear(); proposalDecisionPending = false;
    proposedPreference->clear(); preferenceProposalPanel->hide();
    updateControls();
}
void RadioDialog::setRouteObservationAvailable(const QString &messageId)
{
    routeObservationMessage = messageId;
    routeObserveButton->setVisible(!messageId.isEmpty() && !conversationMode);
    if (conversationMode) renderConversation(conversationText);
}
