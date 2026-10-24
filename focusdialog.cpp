#include "focusdialog.h"
#include "dialogstyle.h"

#include <QCheckBox>
#include <QHideEvent>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

void drawChevron(QPainter &painter, const QPointF &center, bool down)
{
    const qreal direction = down ? 1.0 : -1.0;
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#8b9890")), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF points[] = {center + QPointF(-3, -1.5 * direction),
                              center + QPointF(0, 1.5 * direction),
                              center + QPointF(3, -1.5 * direction)};
    painter.drawPolyline(points, 3);
}

class FocusComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QComboBox::paintEvent(event);
        QPainter painter(this);
        drawChevron(painter, QPointF(width() - 14, height() / 2.0), true);
    }
};

class FocusMinutesSpinBox : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QSpinBox::paintEvent(event);
        QPainter painter(this);
        drawChevron(painter, QPointF(width() - 9, height() / 4.0), false);
        drawChevron(painter, QPointF(width() - 9, height() * 3.0 / 4.0), true);
    }
};

QLabel *label(const QString &text, const char *name, QWidget *parent,
              const char *role = nullptr)
{
    auto *result = new QLabel(text, parent);
    result->setObjectName(QString::fromLatin1(name));
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    if (role) result->setProperty("role", role);
    return result;
}

QPushButton *button(const QString &text, const char *name, QWidget *parent)
{
    auto *result = new QPushButton(text, parent);
    result->setObjectName(QString::fromLatin1(name));
    result->setAutoDefault(false);
    return result;
}

QString durationText(qint64 milliseconds)
{
    if (milliseconds <= 0) return QStringLiteral("时长未知");
    const qint64 seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString instrumentalText(int value)
{
    if (value == 1) return QStringLiteral("纯音乐");
    if (value == 0) return QStringLiteral("有人声");
    return QStringLiteral("未标注");
}

QString liveText(int value)
{
    if (value == 1) return QStringLiteral("现场版");
    if (value == 0) return QStringLiteral("录音室版");
    return QStringLiteral("未标注");
}

QScrollArea *scrollPage(QWidget *content, QWidget *parent)
{
    auto *scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    scroll->setWidget(content);
    return scroll;
}

QTableWidgetItem *cell(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setToolTip(text);
    return item;
}

bool sameTracks(const QVector<FocusTrack> &left, const QVector<FocusTrack> &right)
{
    if (left.size() != right.size()) return false;
    for (int i = 0; i < left.size(); ++i) {
        const auto &a = left.at(i);
        const auto &b = right.at(i);
        if (a.key != b.key || a.title != b.title || a.artist != b.artist || a.url != b.url ||
            a.songId != b.songId || a.durationMs != b.durationMs || a.favorite != b.favorite ||
            a.instrumental != b.instrumental || a.live != b.live) return false;
    }
    return true;
}

bool sameRules(const QVector<FocusRule> &left, const QVector<FocusRule> &right)
{
    if (left.size() != right.size()) return false;
    for (int i = 0; i < left.size(); ++i) {
        const auto &a = left.at(i);
        const auto &b = right.at(i);
        if (a.id != b.id || a.scene != b.scene || a.kind != b.kind || a.value != b.value) return false;
    }
    return true;
}

} // namespace

FocusDialog::FocusDialog(const QVector<FocusTrack> &queue,
                         const QVector<FocusTrack> &favorites,
                         const QVector<FocusRule> &rules,
                         QWidget *parent)
    : QDialog(parent), queueTracks(queue), favoriteTracks(favorites), savedRules(rules)
{
    setObjectName(QStringLiteral("focusDialog"));
    setWindowTitle(QStringLiteral("专注听歌"));
    setMinimumSize(660, 580);
    resize(740, 690);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(12);
    layout->addWidget(label(QStringLiteral("把这段时间，留给自己"), "label_FocusHeading", this, "heading"));
    layout->addWidget(label(QStringLiteral("从你的音乐中安排一段听歌时间，按你明确设置的偏好挑选。"),
                            "label_FocusSubtitle", this, "subtitle"));

    sessionPanel = new QWidget(this);
    sessionPanel->setObjectName(QStringLiteral("panel_FocusSession"));
    auto *sessionLayout = new QVBoxLayout(sessionPanel);
    sessionLayout->setContentsMargins(12, 10, 12, 10);
    sessionLayout->setSpacing(8);
    auto *sessionMainRow = new QHBoxLayout;
    sessionLabel = label(QString(), "label_FocusSession", sessionPanel);
    sessionMainRow->addWidget(sessionLabel, 1);
    pauseButton = button(QStringLiteral("暂停"), "button_FocusPause", sessionPanel);
    endButton = button(QStringLiteral("结束"), "button_FocusEnd", sessionPanel);
    sessionMainRow->addWidget(pauseButton);
    sessionMainRow->addWidget(endButton);
    sessionLayout->addLayout(sessionMainRow);
    avoidCurrentButton = button(QStringLiteral("本场景避开这首"), "button_FocusAvoidCurrent", sessionPanel);
    avoidCurrentButton->setToolTip(QStringLiteral("保存偏好，并从剩余计划中跳过当前歌曲。"));
    sessionLayout->addWidget(avoidCurrentButton, 0, Qt::AlignRight);
    layout->addWidget(sessionPanel);

    tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("tabs_Focus"));
    auto *planPage = new QWidget(tabs);
    auto *planLayout = new QVBoxLayout(planPage);
    planLayout->setContentsMargins(4, 16, 16, 12);
    planLayout->setSpacing(12);

    auto *options = new QGridLayout;
    options->setHorizontalSpacing(12);
    options->setVerticalSpacing(8);
    options->addWidget(label(QStringLiteral("此刻要做什么"), "label_FocusScene", planPage, "subtitle"), 0, 0);
    options->addWidget(label(QStringLiteral("留多长时间"), "label_FocusDuration", planPage, "subtitle"), 0, 1);
    options->addWidget(label(QStringLiteral("从哪里挑选"), "label_FocusSource", planPage, "subtitle"), 0, 2);
    scene = new FocusComboBox(planPage);
    scene->setObjectName(QStringLiteral("combo_FocusScene"));
    scene->addItem(QStringLiteral("工作"), QStringLiteral("work"));
    scene->addItem(QStringLiteral("学习"), QStringLiteral("study"));
    scene->addItem(QStringLiteral("放松"), QStringLiteral("rest"));
    options->addWidget(scene, 1, 0);
    auto *durationLayout = new QHBoxLayout;
    durationLayout->setSpacing(4);
    auto *shortButton = button(QStringLiteral("25"), "button_Focus25", planPage);
    auto *longButton = button(QStringLiteral("50"), "button_Focus50", planPage);
    shortButton->setToolTip(QStringLiteral("25 分钟"));
    longButton->setToolTip(QStringLiteral("50 分钟"));
    shortButton->setFixedWidth(42);
    longButton->setFixedWidth(42);
    durationLayout->addWidget(shortButton);
    durationLayout->addWidget(longButton);
    minutes = new FocusMinutesSpinBox(planPage);
    minutes->setObjectName(QStringLiteral("spin_FocusMinutes"));
    minutes->setRange(1, 180);
    minutes->setValue(25);
    minutes->setSuffix(QStringLiteral(" 分钟"));
    minutes->setMaximumWidth(110);
    durationLayout->addWidget(minutes, 1);
    options->addLayout(durationLayout, 1, 1);
    source = new FocusComboBox(planPage);
    source->setObjectName(QStringLiteral("combo_FocusSource"));
    source->addItem(QStringLiteral("当前播放队列"), QStringLiteral("queue"));
    source->addItem(QStringLiteral("喜欢的音乐"), QStringLiteral("favorites"));
    options->addWidget(source, 1, 2);
    options->setColumnStretch(0, 1);
    options->setColumnStretch(1, 1);
    options->setColumnStretch(2, 1);
    planLayout->addLayout(options);

    auto *naturalHeading = new QHBoxLayout;
    naturalHeading->addWidget(label(QStringLiteral("也可以说一句话"), "label_FocusPrompt", planPage, "subtitle"));
    naturalHeading->addStretch();
    parserMode = label(QStringLiteral("本地规则解析"), "label_FocusParserMode", planPage, "subtitle");
    naturalHeading->addWidget(parserMode);
    planLayout->addLayout(naturalHeading);
    auto *instructionLayout = new QHBoxLayout;
    instruction = new QLineEdit(planPage);
    instruction->setObjectName(QStringLiteral("edit_FocusInstruction"));
    instruction->setPlaceholderText(QStringLiteral("例如：工作 50 分钟，不要现场版"));
    instruction->setMaxLength(1000);
    parseButton = button(QStringLiteral("理解并预览"), "button_FocusParse", planPage);
    instructionLayout->addWidget(instruction, 1);
    instructionLayout->addWidget(parseButton);
    planLayout->addLayout(instructionLayout);

    auto *filters = new QHBoxLayout;
    excludeLive = new QCheckBox(QStringLiteral("避开现场版"), planPage);
    excludeLive->setObjectName(QStringLiteral("check_FocusExcludeLive"));
    excludeLive->setToolTip(QStringLiteral("根据版本信息和你保存的标注筛选；未确认的版本会在预览中说明。"));
    instrumentalOnly = new QCheckBox(QStringLiteral("仅已标注的纯音乐"), planPage);
    instrumentalOnly->setObjectName(QStringLiteral("check_FocusInstrumentalOnly"));
    instrumentalOnly->setToolTip(QStringLiteral("只选择你明确标注为纯音乐的曲目，可在“曲目标注”中添加。"));
    filters->addWidget(excludeLive);
    filters->addWidget(instrumentalOnly);
    filters->addStretch();
    planLayout->addLayout(filters);
    excludedArtists = new QLineEdit(planPage);
    excludedArtists->setObjectName(QStringLiteral("edit_FocusExcludedArtists"));
    excludedArtists->setPlaceholderText(QStringLiteral("避开的歌手，多个名字用逗号分隔"));
    excludedArtists->setMaxLength(1000);
    planLayout->addWidget(excludedArtists);
    auto *previewRow = new QHBoxLayout;
    planSummary = label(QStringLiteral("设置条件后，先预览这次安排。"), "label_FocusPlanSummary", planPage);
    previewRow->addWidget(planSummary, 1);
    previewButton = button(QStringLiteral("生成预览"), "button_FocusPreview", planPage);
    previewRow->addWidget(previewButton);
    planLayout->addLayout(previewRow);
    planTable = new QTableWidget(0, 3, planPage);
    planTable->setObjectName(QStringLiteral("table_FocusPlan"));
    planTable->setHorizontalHeaderLabels({QStringLiteral("曲目"), QStringLiteral("歌手"), QStringLiteral("时长")});
    planTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    planTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    planTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    planTable->setMinimumHeight(164);
    planLayout->addWidget(planTable, 1);
    planNotes = label(QStringLiteral("歌曲播放权限会在播放时检查。"), "label_FocusPlanNotes", planPage, "subtitle");
    planLayout->addWidget(planNotes);
    tabs->addTab(scrollPage(planPage, tabs), QStringLiteral("安排计划"));

    auto *libraryPage = new QWidget(tabs);
    auto *libraryLayout = new QVBoxLayout(libraryPage);
    libraryLayout->setContentsMargins(4, 16, 16, 12);
    libraryLayout->setSpacing(12);
    libraryLayout->addWidget(label(QStringLiteral("选一首歌，告诉探歌你确定的信息。标注会用于之后的安排。"),
                                   "label_FocusLibraryHint", libraryPage, "subtitle"));
    libraryTable = new QTableWidget(0, 5, libraryPage);
    libraryTable->setObjectName(QStringLiteral("table_FocusLibrary"));
    libraryTable->setHorizontalHeaderLabels({QStringLiteral("曲目"), QStringLiteral("歌手"), QStringLiteral("时长"),
                                             QStringLiteral("人声"), QStringLiteral("版本")});
    libraryTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    libraryTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int column = 2; column < 5; ++column)
        libraryTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    libraryTable->setMinimumHeight(240);
    libraryLayout->addWidget(libraryTable, 1);
    auto *tagsLayout = new QHBoxLayout;
    instrumentalTag = new FocusComboBox(libraryPage);
    instrumentalTag->setObjectName(QStringLiteral("combo_FocusInstrumentalTag"));
    instrumentalTag->addItem(QStringLiteral("人声：未标注"), -1);
    instrumentalTag->addItem(QStringLiteral("纯音乐"), 1);
    instrumentalTag->addItem(QStringLiteral("有人声"), 0);
    liveTag = new FocusComboBox(libraryPage);
    liveTag->setObjectName(QStringLiteral("combo_FocusLiveTag"));
    liveTag->addItem(QStringLiteral("版本：未标注"), -1);
    liveTag->addItem(QStringLiteral("现场版"), 1);
    liveTag->addItem(QStringLiteral("录音室版"), 0);
    saveTagsButton = button(QStringLiteral("保存标注"), "button_FocusSaveTags", libraryPage);
    tagsLayout->addWidget(instrumentalTag, 1);
    tagsLayout->addWidget(liveTag, 1);
    tagsLayout->addWidget(saveTagsButton);
    libraryLayout->addLayout(tagsLayout);
    libraryLayout->addWidget(label(QStringLiteral("此处显示“安排计划”中所选来源的音乐；无需先生成计划。"),
                                   "label_FocusLibrarySource", libraryPage, "subtitle"));
    tabs->addTab(scrollPage(libraryPage, tabs), QStringLiteral("曲目标注"));

    auto *rulesPage = new QWidget(tabs);
    auto *rulesLayout = new QVBoxLayout(rulesPage);
    rulesLayout->setContentsMargins(4, 16, 16, 12);
    rulesLayout->setSpacing(12);
    rulesHeading = label(QString(), "label_FocusRulesHeading", rulesPage);
    rulesLayout->addWidget(rulesHeading);
    rulesLayout->addWidget(label(QStringLiteral("只记住你主动保存的条件。切歌不会自动变成“不喜欢”。"),
                                 "label_FocusMemoryHint", rulesPage, "subtitle"));
    rulesList = new QListWidget(rulesPage);
    rulesList->setObjectName(QStringLiteral("list_FocusRules"));
    rulesList->setMinimumHeight(220);
    rulesLayout->addWidget(rulesList, 1);
    auto *rulesButtons = new QHBoxLayout;
    rememberButton = button(QStringLiteral("记住当前条件"), "button_FocusRemember", rulesPage);
    deleteRuleButton = button(QStringLiteral("删除选中偏好"), "button_FocusDeleteRule", rulesPage);
    rulesButtons->addWidget(rememberButton);
    rulesButtons->addStretch();
    rulesButtons->addWidget(deleteRuleButton);
    rulesLayout->addLayout(rulesButtons);
    rulesLayout->addWidget(label(QStringLiteral("时长和音乐来源只用于本次计划；保存的筛选偏好按工作、学习、放松分别使用。"),
                                 "label_FocusRulesScope", rulesPage, "subtitle"));
    tabs->addTab(scrollPage(rulesPage, tabs), QStringLiteral("我的偏好"));
    layout->addWidget(tabs, 1);

    status = label(QStringLiteral("选择音乐来源和条件，或用一句话描述。"), "label_FocusStatus", this, "subtitle");
    status->setMinimumHeight(32);
    layout->addWidget(status);
    auto *footer = new QHBoxLayout;
    auto *closeButton = button(QStringLiteral("关闭"), "button_FocusClose", this);
    footer->addWidget(closeButton);
    footer->addStretch();
    startButton = button(QStringLiteral("开始这段时间"), "button_FocusStart", this);
    startButton->setProperty("role", "primary");
    footer->addWidget(startButton);
    layout->addLayout(footer);

    connect(shortButton, &QPushButton::clicked, this, [this] { minutes->setValue(25); });
    connect(longButton, &QPushButton::clicked, this, [this] { minutes->setValue(50); });
    connect(scene, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (applyingRequest) return;
        invalidatePreview();
        updateRules();
    });
    connect(source, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (applyingRequest) return;
        invalidatePreview();
        updateLibrary();
    });
    connect(minutes, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] { invalidatePreview(); });
    connect(excludeLive, &QCheckBox::toggled, this, [this] { invalidatePreview(); });
    connect(instrumentalOnly, &QCheckBox::toggled, this, [this] { invalidatePreview(); });
    connect(excludedArtists, &QLineEdit::textChanged, this, [this] { invalidatePreview(); });
    connect(instruction, &QLineEdit::textChanged, this, [this] { invalidatePreview(); });
    connect(previewButton, &QPushButton::clicked, this, &FocusDialog::requestPreview);
    connect(parseButton, &QPushButton::clicked, this, [this] {
        if (busy || instruction->text().trimmed().isEmpty()) return;
        invalidatePreview();
        emit parseRequested(instruction->text().trimmed(), request());
    });
    connect(instruction, &QLineEdit::returnPressed, parseButton, &QPushButton::click);
    connect(startButton, &QPushButton::clicked, this, [this] {
        if (!previewValid || busy || sessionActive || displayedPlan.tracks.isEmpty()) return;
        emit startRequested(displayedPlan);
    });
    connect(rememberButton, &QPushButton::clicked, this, [this] { emit rememberRequested(request()); });
    connect(rulesList, &QListWidget::itemSelectionChanged, this, &FocusDialog::updateControls);
    connect(deleteRuleButton, &QPushButton::clicked, this, [this] {
        auto *selected = rulesList->currentItem();
        if (selected && selected->isSelected())
            emit deleteRuleRequested(selected->data(Qt::UserRole).toLongLong());
    });
    connect(libraryTable, &QTableWidget::itemSelectionChanged, this, &FocusDialog::updateTagControls);
    connect(saveTagsButton, &QPushButton::clicked, this, [this] {
        const int row = libraryTable->currentRow();
        if (!libraryTable->selectionModel()->hasSelection() || row < 0 || row >= currentCandidates().size()) return;
        const QString key = currentCandidates().at(row).key;
        if (key.isEmpty()) return;
        emit setTrackTags(key, instrumentalTag->currentData().toInt(), liveTag->currentData().toInt());
    });
    connect(pauseButton, &QPushButton::clicked, this, &FocusDialog::pauseResumeRequested);
    connect(endButton, &QPushButton::clicked, this, &FocusDialog::endSessionRequested);
    connect(avoidCurrentButton, &QPushButton::clicked, this, &FocusDialog::avoidCurrentTrackRequested);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);

    applyDialogStyle(this);
    // Apply the extra controls directly after the shared dialog theme. Qt 5's
    // native macOS controls otherwise retain their initial platform styling.
    tabs->setStyleSheet(QStringLiteral(R"(
        QTabWidget::pane { border: 0; background: #121614; }
        QTabBar::tab { border: 0; background: #121614; color: #8b9890; padding: 9px 15px;
            border-bottom: 2px solid #121614; }
        QTabBar::tab:selected { color: #b4d9a0; border-bottom-color: #b4d9a0; }
    )"));
    const QString inputStyle = QStringLiteral(R"(
        QComboBox, QSpinBox { color: #ecf0ea; background: #191e1b;
            border: 1px solid #354038; border-radius: 8px; min-height: 24px; padding: 6px 9px; }
        QComboBox:focus, QSpinBox:focus { border-color: #b4d9a0; }
        QComboBox::drop-down { border: none; width: 22px; }
        QSpinBox::up-button, QSpinBox::down-button { width: 18px; border: none; background: #252d27; }
        QSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; border-top-right-radius: 8px; }
        QSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; border-bottom-right-radius: 8px; }
        QComboBox QAbstractItemView { color: #ecf0ea; background: #191e1b;
            selection-background-color: #344b37; selection-color: #ecf0ea; }
    )");
    for (auto *combo : findChildren<QComboBox *>()) combo->setStyleSheet(inputStyle);
    minutes->setStyleSheet(inputStyle);
    const QString checkStyle = QStringLiteral(R"(
        QCheckBox { color: #ecf0ea; spacing: 8px; padding: 5px 0; }
        QCheckBox::indicator { width: 15px; height: 15px; border-radius: 4px;
            background: #191e1b; border: 1px solid #526350; }
        QCheckBox::indicator:checked { background: #b4d9a0; border-color: #b4d9a0; }
    )");
    excludeLive->setStyleSheet(checkStyle);
    instrumentalOnly->setStyleSheet(checkStyle);
    rulesList->setStyleSheet(QStringLiteral(R"(
        QListWidget { color: #ecf0ea; background: #191e1b; border: 1px solid #2c352e;
            border-radius: 10px; outline: none; }
        QListWidget::item { padding: 12px; border-bottom: 1px solid #252d27; }
        QListWidget::item:selected { background: #344b37; color: #ecf0ea; }
    )"));
    for (auto *scroll : findChildren<QScrollArea *>()) {
        scroll->setStyleSheet(QStringLiteral("QScrollArea { border: 0; background: #121614; }"
                                             "QScrollArea > QWidget > QWidget { background: #121614; }"));
        scroll->widget()->setObjectName(QStringLiteral("focusPage"));
        scroll->widget()->setAttribute(Qt::WA_StyledBackground, true);
        scroll->widget()->setStyleSheet(QStringLiteral("QWidget#focusPage { background: #121614; }"));
    }
    sessionPanel->setStyleSheet(QStringLiteral("QWidget#panel_FocusSession { background: #202c21; border-radius: 10px; }"));
    shortButton->setStyleSheet(QStringLiteral("padding: 8px 4px;"));
    longButton->setStyleSheet(QStringLiteral("padding: 8px 4px;"));
    startButton->setStyleSheet(QStringLiteral("QPushButton:disabled { background: #1a201c; color: #526057; border-color: #252d27; }"));
    setSessionStatus(QString(), 0, false, false);
    updateLibrary();
    updateRules();
    updateControls();
}

void FocusDialog::setEmbedded(bool value)
{
    embedded = value;
    setProperty("embedded", value);
    if (auto *back = findChild<QPushButton *>(QStringLiteral("button_FocusClose")))
        back->setText(value ? QStringLiteral("返回曲库") : QStringLiteral("关闭"));
    if (value) {
        setMinimumSize(0, 0);
        if (layout()) layout()->setContentsMargins(24, 18, 24, 18);
    }
}

void FocusDialog::resetForAccount()
{
    ++revision;
    setBusy(false);
    setRequest(FocusRequest());
    setSessionStatus(QString(), 0, false, false);
    setStatus(QStringLiteral("选择音乐来源和条件，或用一句话描述。"));
}

void FocusDialog::reject()
{
    if (busy) {
        ++revision;
        setBusy(false);
        emit cancelRequested();
    }
    QDialog::reject();
}

void FocusDialog::hideEvent(QHideEvent *event)
{
    if (embedded && busy) {
        ++revision;
        setBusy(false);
        emit cancelRequested();
    }
    QDialog::hideEvent(event);
}

FocusRequest FocusDialog::request() const
{
    FocusRequest result;
    result.scene = scene->currentData().toString();
    result.minutes = minutes->value();
    result.source = source->currentData().toString();
    result.excludeLive = excludeLive->isChecked();
    result.instrumentalOnly = instrumentalOnly->isChecked();
    const QStringList names = excludedArtists->text().split(QRegularExpression(QStringLiteral("[,，;；\\n]")), Qt::SkipEmptyParts);
    for (const QString &name : names) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty() && !result.excludedArtists.contains(trimmed, Qt::CaseInsensitive))
            result.excludedArtists.append(trimmed);
    }
    result.originalText = instruction->text().trimmed();
    return result;
}

void FocusDialog::setCandidates(const QVector<FocusTrack> &queue,
                                const QVector<FocusTrack> &favorites)
{
    if (sameTracks(queueTracks, queue) && sameTracks(favoriteTracks, favorites)) return;
    queueTracks = queue;
    favoriteTracks = favorites;
    invalidatePreview();
    updateLibrary();
    updateRules();
}

void FocusDialog::setRules(const QVector<FocusRule> &rules)
{
    if (sameRules(savedRules, rules)) return;
    savedRules = rules;
    invalidatePreview();
    updateRules();
}

void FocusDialog::setRequest(const FocusRequest &value)
{
    applyingRequest = true;
    scene->setCurrentIndex(qMax(0, scene->findData(value.scene)));
    source->setCurrentIndex(qMax(0, source->findData(value.source)));
    minutes->setValue(qBound(1, value.minutes, 180));
    excludeLive->setChecked(value.excludeLive);
    instrumentalOnly->setChecked(value.instrumentalOnly);
    excludedArtists->setText(value.excludedArtists.join(QStringLiteral("，")));
    instruction->setText(value.originalText);
    applyingRequest = false;
    invalidatePreview();
    updateLibrary();
    updateRules();
}

void FocusDialog::setPlan(const FocusPlan &plan)
{
    displayedPlan = plan;
    planTable->setRowCount(plan.tracks.size());
    qint64 duration = 0;
    int unknown = 0;
    for (int row = 0; row < plan.tracks.size(); ++row) {
        const FocusTrack &track = plan.tracks.at(row);
        planTable->setItem(row, 0, cell(track.title.isEmpty() ? QStringLiteral("未命名曲目") : track.title));
        planTable->setItem(row, 1, cell(track.artist.isEmpty() ? QStringLiteral("未知歌手") : track.artist));
        planTable->setItem(row, 2, cell(durationText(track.durationMs)));
        if (track.durationMs > 0) duration += track.durationMs;
        else ++unknown;
    }
    previewValid = plan.error.isEmpty() && !plan.tracks.isEmpty();
    if (!plan.error.isEmpty()) {
        planSummary->setText(QStringLiteral("暂时无法安排这段时间"));
        setStatus(plan.error, true);
    } else if (plan.tracks.isEmpty()) {
        planSummary->setText(QStringLiteral("没有符合条件的音乐"));
        setStatus(QStringLiteral("试着调整筛选条件，或在“曲目标注”中补充信息。"));
    } else {
        QString summary = QStringLiteral("%1 首 · 已知时长 %2").arg(plan.tracks.size()).arg(durationText(duration));
        if (unknown > 0) summary += QStringLiteral(" · %1 首时长未知").arg(unknown);
        planSummary->setText(summary);
        setStatus(QStringLiteral("已生成预览。开始后切换页面，计划会继续运行。"));
    }
    planNotes->setText(plan.notes.isEmpty() ? QStringLiteral("歌曲播放权限会在播放时检查。")
                                          : plan.notes.join(QLatin1Char('\n')));
    updateControls();
}

void FocusDialog::setStatus(const QString &message, bool error)
{
    status->setText(message);
    status->setProperty("error", error);
    status->setStyleSheet(error ? QStringLiteral("color: #e8aba3;") : QString());
}

void FocusDialog::setModelAvailable(bool available)
{
    parserMode->setText(available ? QStringLiteral("AI 解析") : QStringLiteral("本地规则解析"));
}

void FocusDialog::setParserStatus(const QString &text)
{
    parserMode->setText(text);
}

void FocusDialog::setBusy(bool value)
{
    busy = value;
    parseButton->setText(busy ? QStringLiteral("正在理解…") : QStringLiteral("理解并预览"));
    updateControls();
}

void FocusDialog::setSessionStatus(const QString &state, int remainingSeconds,
                                   bool active, bool paused)
{
    sessionActive = active;
    sessionPanel->setVisible(active);
    const int seconds = qMax(0, remainingSeconds);
    sessionLabel->setText(QStringLiteral("%1 · 剩余 %2:%3")
                              .arg(state.isEmpty() ? (paused ? QStringLiteral("已暂停") : QStringLiteral("专注中")) : state)
                              .arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0')));
    pauseButton->setText(paused ? QStringLiteral("继续") : QStringLiteral("暂停"));
    startButton->setText(active ? QStringLiteral("计划进行中") : QStringLiteral("开始这段时间"));
    updateControls();
}

const QVector<FocusTrack> &FocusDialog::currentCandidates() const
{
    return source->currentData().toString() == QStringLiteral("favorites") ? favoriteTracks : queueTracks;
}

void FocusDialog::invalidatePreview()
{
    if (applyingRequest) return;
    ++revision;
    previewValid = false;
    displayedPlan = FocusPlan();
    planTable->setRowCount(0);
    planSummary->setText(QStringLiteral("条件已更新，请重新生成预览。"));
    planNotes->setText(QStringLiteral("歌曲播放权限会在播放时检查。"));
    updateControls();
}

void FocusDialog::updateControls()
{
    startButton->setEnabled(previewValid && !busy && !sessionActive);
    previewButton->setEnabled(!busy);
    parseButton->setEnabled(!busy && !instruction->text().trimmed().isEmpty());
    const bool hasConditions = excludeLive->isChecked() || instrumentalOnly->isChecked()
                               || !excludedArtists->text().trimmed().isEmpty();
    rememberButton->setEnabled(!busy && hasConditions);
    deleteRuleButton->setEnabled(!busy && rulesList->currentItem()
                                 && rulesList->currentItem()->isSelected());
    pauseButton->setEnabled(sessionActive);
    endButton->setEnabled(sessionActive);
    avoidCurrentButton->setEnabled(sessionActive);
}

void FocusDialog::updateLibrary()
{
    const QVector<FocusTrack> &tracks = currentCandidates();
    QString selectedKey;
    const int oldRow = libraryTable->currentRow();
    if (oldRow >= 0 && libraryTable->item(oldRow, 0))
        selectedKey = libraryTable->item(oldRow, 0)->data(Qt::UserRole).toString();
    const QSignalBlocker blocker(libraryTable);
    libraryTable->setRowCount(tracks.size());
    int selectRow = -1;
    for (int row = 0; row < tracks.size(); ++row) {
        const FocusTrack &track = tracks.at(row);
        auto *title = cell(track.title.isEmpty() ? QStringLiteral("未命名曲目") : track.title);
        title->setData(Qt::UserRole, track.key);
        libraryTable->setItem(row, 0, title);
        libraryTable->setItem(row, 1, cell(track.artist.isEmpty() ? QStringLiteral("未知歌手") : track.artist));
        libraryTable->setItem(row, 2, cell(durationText(track.durationMs)));
        libraryTable->setItem(row, 3, cell(instrumentalText(track.instrumental)));
        libraryTable->setItem(row, 4, cell(liveText(track.live)));
        if (!selectedKey.isEmpty() && selectedKey == track.key) selectRow = row;
    }
    if (selectRow < 0 && !tracks.isEmpty()) selectRow = 0;
    if (selectRow >= 0) libraryTable->selectRow(selectRow);
    updateTagControls();
}

void FocusDialog::updateRules()
{
    rulesHeading->setText(QStringLiteral("%1时使用的偏好").arg(sceneLabel(scene->currentData().toString())));
    rulesList->clear();
    for (const FocusRule &rule : savedRules) {
        if (rule.scene != scene->currentData().toString()) continue;
        QString description;
        if (rule.kind == QStringLiteral("exclude_live")) description = QStringLiteral("避开现场版");
        else if (rule.kind == QStringLiteral("instrumental_only")) description = QStringLiteral("只听已标注的纯音乐");
        else if (rule.kind == QStringLiteral("exclude_artist")) description = QStringLiteral("避开歌手：%1").arg(rule.value);
        else if (rule.kind == QStringLiteral("exclude_track")) {
            QString title;
            for (const auto &tracks : {queueTracks, favoriteTracks}) {
                for (const FocusTrack &track : tracks) {
                    if (track.key == rule.value && !track.title.isEmpty()) {
                        title = track.title;
                        break;
                    }
                }
                if (!title.isEmpty()) break;
            }
            description = title.isEmpty() ? QStringLiteral("避开某首已移除的歌曲")
                                           : QStringLiteral("避开曲目：%1").arg(title);
        }
        else continue;
        auto *item = new QListWidgetItem(description, rulesList);
        item->setData(Qt::UserRole, rule.id);
        if (rule.kind == QStringLiteral("exclude_track")) item->setToolTip(rule.value);
    }
    if (rulesList->count() == 0) {
        auto *empty = new QListWidgetItem(QStringLiteral("还没有保存偏好，按你的习惯慢慢添加。"), rulesList);
        empty->setFlags(Qt::NoItemFlags);
    }
    updateControls();
}

void FocusDialog::updateTagControls()
{
    const int row = libraryTable->currentRow();
    const bool selected = libraryTable->selectionModel()->hasSelection()
                          && row >= 0 && row < currentCandidates().size();
    instrumentalTag->setEnabled(selected);
    liveTag->setEnabled(selected);
    saveTagsButton->setEnabled(selected && !currentCandidates().at(row).key.isEmpty());
    if (!selected) return;
    const FocusTrack &track = currentCandidates().at(row);
    instrumentalTag->setCurrentIndex(qMax(0, instrumentalTag->findData(track.instrumental)));
    liveTag->setCurrentIndex(qMax(0, liveTag->findData(track.live)));
}

void FocusDialog::requestPreview()
{
    if (busy) return;
    invalidatePreview();
    emit previewRequested(request());
}

QString FocusDialog::sceneLabel(const QString &value) const
{
    if (value == QStringLiteral("study")) return QStringLiteral("学习");
    if (value == QStringLiteral("rest")) return QStringLiteral("放松");
    return QStringLiteral("工作");
}
