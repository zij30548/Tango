#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>

#include "focusdialog.h"

namespace {

QVector<FocusTrack> tracks()
{
    FocusTrack known;
    known.key = QStringLiteral("file:known");
    known.title = QStringLiteral("<b>曲目一</b>");
    known.artist = QStringLiteral("歌手一");
    known.durationMs = 185000;
    FocusTrack unknown;
    unknown.key = QStringLiteral("file:unknown");
    unknown.title = QStringLiteral("曲目二");
    unknown.artist = QStringLiteral("歌手二");
    return {known, unknown};
}

FocusPlan preview(FocusDialog &dialog)
{
    FocusPlan plan;
    plan.request = dialog.request();
    plan.tracks = tracks();
    plan.notes = QStringList{QStringLiteral("一首曲目时长未知，将按计划计时。")};
    return plan;
}

} // namespace

class FocusDialogTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void editInvalidatesPreviewAndBusyPreventsStart();
    void modelParsingHasGenerationAndCandidateInvalidation();
    void taggingWorksWithoutPlanAndRulesAreExplicit();
    void sessionControlsRemainIndependentFromDialog();
};

void FocusDialogTest::initTestCase()
{
    qRegisterMetaType<FocusRequest>();
    qRegisterMetaType<FocusPlan>();
}

void FocusDialogTest::editInvalidatesPreviewAndBusyPreventsStart()
{
    FocusDialog dialog(tracks(), {}, {});
    auto *start = dialog.findChild<QPushButton *>("button_FocusStart");
    auto *generate = dialog.findChild<QPushButton *>("button_FocusPreview");
    auto *duration = dialog.findChild<QSpinBox *>("spin_FocusMinutes");
    auto *live = dialog.findChild<QCheckBox *>("check_FocusExcludeLive");
    auto *table = dialog.findChild<QTableWidget *>("table_FocusPlan");
    auto *summary = dialog.findChild<QLabel *>("label_FocusPlanSummary");
    QVERIFY(start && generate && duration && live && table && summary);
    QVERIFY(!start->isEnabled());
    QSignalSpy generated(&dialog, &FocusDialog::previewRequested);
    QSignalSpy started(&dialog, &FocusDialog::startRequested);
    generate->click();
    QCOMPARE(generated.count(), 1);
    dialog.setPlan(preview(dialog));
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, 2)->text(), QStringLiteral("3:05"));
    QCOMPARE(table->item(1, 2)->text(), QStringLiteral("时长未知"));
    QVERIFY(summary->text().contains(QStringLiteral("1 首时长未知")));
    QVERIFY(start->isEnabled());
    dialog.setBusy(true);
    QVERIFY(!start->isEnabled());
    QVERIFY(!generate->isEnabled());
    start->click();
    QCOMPARE(started.count(), 0);
    dialog.setBusy(false);
    start->click();
    QCOMPARE(started.count(), 1);
    QCOMPARE(qvariant_cast<FocusPlan>(started.first().first()).tracks.first().key, QStringLiteral("file:known"));
    duration->setValue(50);
    QVERIFY(!start->isEnabled());
    QCOMPARE(table->rowCount(), 0);
    start->click();
    QCOMPARE(started.count(), 1);
    dialog.setPlan(preview(dialog));
    live->setChecked(true);
    QVERIFY(!start->isEnabled());
    dialog.setPlan(preview(dialog));
    FocusPlan errorPlan = preview(dialog);
    errorPlan.error = QStringLiteral("没有符合条件的音乐");
    dialog.setPlan(errorPlan);
    QVERIFY(!start->isEnabled());
    auto *status = dialog.findChild<QLabel *>("label_FocusStatus");
    QVERIFY(status->property("error").toBool());
    QCOMPARE(status->textFormat(), Qt::PlainText);
}

void FocusDialogTest::modelParsingHasGenerationAndCandidateInvalidation()
{
    FocusDialog dialog(tracks(), {}, {});
    auto *text = dialog.findChild<QLineEdit *>("edit_FocusInstruction");
    auto *parse = dialog.findChild<QPushButton *>("button_FocusParse");
    auto *mode = dialog.findChild<QLabel *>("label_FocusParserMode");
    auto *start = dialog.findChild<QPushButton *>("button_FocusStart");
    QVERIFY(text && parse && mode && start);
    QVERIFY(!parse->isEnabled());
    QCOMPARE(mode->text(), QStringLiteral("本地规则解析"));
    dialog.setModelAvailable(true);
    QCOMPARE(mode->text(), QStringLiteral("AI 解析"));
    QSignalSpy parsing(&dialog, &FocusDialog::parseRequested);
    text->setText(QStringLiteral("工作50分钟，不要现场版"));
    parse->click();
    QCOMPARE(parsing.count(), 1);
    QCOMPARE(parsing.first().at(0).toString(), text->text());
    const quint64 captured = dialog.generation();
    QVERIFY(dialog.isCurrentGeneration(captured));
    dialog.setBusy(true);
    text->setText(QStringLiteral("学习25分钟"));
    QVERIFY(!dialog.isCurrentGeneration(captured));
    dialog.setBusy(false);
    FocusRequest parsed;
    parsed.scene = QStringLiteral("study");
    parsed.source = QStringLiteral("favorites");
    parsed.minutes = 42;
    parsed.excludeLive = true;
    parsed.excludedArtists = QStringList{QStringLiteral("歌手甲"), QStringLiteral("歌手乙")};
    dialog.setRequest(parsed);
    QCOMPARE(dialog.request().scene, QStringLiteral("study"));
    QCOMPARE(dialog.request().minutes, 42);
    QCOMPARE(dialog.request().source, QStringLiteral("favorites"));
    QCOMPARE(dialog.request().excludedArtists, parsed.excludedArtists);
    dialog.setPlan(preview(dialog));
    QVERIFY(start->isEnabled());
    const quint64 beforeCandidates = dialog.generation();
    dialog.setCandidates({}, tracks());
    QVERIFY(!dialog.isCurrentGeneration(beforeCandidates));
    QVERIFY(!start->isEnabled());
}

void FocusDialogTest::taggingWorksWithoutPlanAndRulesAreExplicit()
{
    QVector<FocusRule> rules{
        {4, QStringLiteral("work"), QStringLiteral("exclude_live"), QString()},
        {6, QStringLiteral("work"), QStringLiteral("exclude_track"), QStringLiteral("file:known")},
        {7, QStringLiteral("work"), QStringLiteral("exclude_track"), QStringLiteral("https://example.invalid/very/long/removed/song")},
        {5, QStringLiteral("study"), QStringLiteral("exclude_artist"), QStringLiteral("歌手甲")}
    };
    FocusDialog dialog(tracks(), {}, rules);
    auto *table = dialog.findChild<QTableWidget *>("table_FocusLibrary");
    auto *save = dialog.findChild<QPushButton *>("button_FocusSaveTags");
    auto *instrumental = dialog.findChild<QComboBox *>("combo_FocusInstrumentalTag");
    auto *live = dialog.findChild<QComboBox *>("combo_FocusLiveTag");
    auto *ruleList = dialog.findChild<QListWidget *>("list_FocusRules");
    auto *remember = dialog.findChild<QPushButton *>("button_FocusRemember");
    auto *remove = dialog.findChild<QPushButton *>("button_FocusDeleteRule");
    auto *scene = dialog.findChild<QComboBox *>("combo_FocusScene");
    auto *exclude = dialog.findChild<QCheckBox *>("check_FocusExcludeLive");
    QVERIFY(table && save && instrumental && live && ruleList && remember && remove && scene && exclude);
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(ruleList->count(), 3);
    QVERIFY(ruleList->item(1)->text().contains(QStringLiteral("<b>曲目一</b>")));
    QVERIFY(!ruleList->item(2)->text().contains(QStringLiteral("https://")));
    QCOMPARE(ruleList->item(2)->toolTip(), QStringLiteral("https://example.invalid/very/long/removed/song"));
    QVERIFY(!remember->isEnabled());
    QVERIFY(!remove->isEnabled());
    QSignalSpy tags(&dialog, &FocusDialog::setTrackTags);
    QSignalSpy memories(&dialog, &FocusDialog::rememberRequested);
    QSignalSpy deletions(&dialog, &FocusDialog::deleteRuleRequested);
    table->selectRow(1);
    instrumental->setCurrentIndex(instrumental->findData(1));
    live->setCurrentIndex(live->findData(0));
    save->click();
    QCOMPARE(tags.count(), 1);
    QCOMPARE(tags.first().at(0).toString(), QStringLiteral("file:unknown"));
    QCOMPARE(tags.first().at(1).toInt(), 1);
    QCOMPARE(tags.first().at(2).toInt(), 0);
    table->clearSelection();
    QVERIFY(!save->isEnabled());
    save->click();
    QCOMPARE(tags.count(), 1);
    exclude->setChecked(true);
    QVERIFY(remember->isEnabled());
    remember->click();
    QCOMPARE(memories.count(), 1);
    QVERIFY(qvariant_cast<FocusRequest>(memories.first().first()).excludeLive);
    ruleList->setCurrentRow(0);
    remove->click();
    QCOMPARE(deletions.count(), 1);
    QCOMPARE(deletions.first().first().toLongLong(), qint64(4));
    scene->setCurrentIndex(scene->findData(QStringLiteral("study")));
    QCOMPARE(ruleList->count(), 1);
    QVERIFY(ruleList->item(0)->text().contains(QStringLiteral("歌手甲")));
}

void FocusDialogTest::sessionControlsRemainIndependentFromDialog()
{
    FocusDialog dialog(tracks(), {}, {});
    auto *pause = dialog.findChild<QPushButton *>("button_FocusPause");
    auto *end = dialog.findChild<QPushButton *>("button_FocusEnd");
    auto *close = dialog.findChild<QPushButton *>("button_FocusClose");
    auto *avoid = dialog.findChild<QPushButton *>("button_FocusAvoidCurrent");
    auto *remaining = dialog.findChild<QLabel *>("label_FocusSession");
    auto *start = dialog.findChild<QPushButton *>("button_FocusStart");
    QVERIFY(pause && end && close && avoid && remaining && start);
    QSignalSpy pauses(&dialog, &FocusDialog::pauseResumeRequested);
    QSignalSpy endings(&dialog, &FocusDialog::endSessionRequested);
    QSignalSpy avoided(&dialog, &FocusDialog::avoidCurrentTrackRequested);
    dialog.setPlan(preview(dialog));
    dialog.setSessionStatus(QStringLiteral("工作中"), 305, true, false);
    QCOMPARE(remaining->text(), QStringLiteral("工作中 · 剩余 5:05"));
    QVERIFY(!start->isEnabled());
    pause->click();
    QCOMPARE(pauses.count(), 1);
    dialog.setSessionStatus(QString(), 305, true, true);
    QCOMPARE(pause->text(), QStringLiteral("继续"));
    end->click();
    QCOMPARE(endings.count(), 1);
    avoid->click();
    QCOMPARE(avoided.count(), 1);
    close->click();
    // Closing the editor emits neither a pause nor an end command.
    QCOMPARE(pauses.count(), 1);
    QCOMPARE(endings.count(), 1);
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
}

QTEST_MAIN(FocusDialogTest)
#include "focusdialog_test.moc"
