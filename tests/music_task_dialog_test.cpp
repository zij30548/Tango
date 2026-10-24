#include <QtTest>
#include <QComboBox>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QMainWindow>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QTemporaryDir>
#include "playerwindowview.h"
#include "radiodialog.h"
namespace {
QPushButton *button(RadioDialog &d,const char *name){return d.findChild<QPushButton*>(name);}
QJsonObject preview() {
    QJsonArray tracks;
    for(int i=0;i<18;++i)tracks.append(QJsonObject{{"id",100+i},{"title",QStringLiteral("真实曲目%1").arg(i)},
        {"artist","Artist"},{"album","Album"},{"durationMs",180000},{"sourceName","来源"},{"sourceTags",QJsonArray{"夜晚"}},
        {"reason","来源主题符合本次要求"}});
    return {{"version",2},{"title","微醺歌单"},{"note","已核验真实来源"},{"tracks",tracks},
        {"lockedIds",QJsonArray{100,101,102}},{"totalDurationMs",3240000},
        {"checks",QJsonArray{QJsonObject{{"key","locks"},{"status","satisfied"},{"text","保留3首锁定歌曲及顺序"}},
            QJsonObject{{"key","style"},{"status","unverified"},{"text","风格尚未进行音频核验"}}}}};
}
QJsonObject snapshot(const QString &status="preview") {
    return {{"id","task-test"},{"revision",3},{"stateVersion",10},{"status",status},{"statusText","正在核验真实曲库"},
        {"briefSummary","喝酒 · 华语 R&B · 约40分钟 · 保留3首锁定歌曲"},{"preview",preview()},
        {"canUndo",true},{"lockedIds",QJsonArray{100,101,102}},
        {"messages",QJsonArray{QJsonObject{{"role","user"},{"content","保留前三首，后半段更轻快"}},
            QJsonObject{{"role","assistant"},{"content","已保留，并展示未核验项"}}}},
        {"budget",QJsonObject{{"runCalls",4},{"totalCalls",9}}}};
}
}
class MusicTaskDialogTest:public QObject{
    Q_OBJECT
    QTemporaryDir temporary;
private slots:
    void initTestCase() {
        QVERIFY(temporary.isValid());
        qputenv("MUSICPLAYER_DATA_DIR",temporary.path().toUtf8());
        qputenv("MUSICPLAYER_NETEASE_PORT","1");
    }
    void taskSnapshotBudgetArtistChoiceAndActionAvailability() {
        RadioDialog dialog;dialog.setAvailability(true,true,{});
        auto value=snapshot("needs_clarification");value.insert("providedArtists",QJsonArray{QJsonObject{{"id",7763},{"name","G.E.M.邓紫棋"},{"description","歌手"}}});
        value.insert("canUndo",false);dialog.setTaskSnapshot(value);
        QVERIFY(dialog.findChild<QLabel*>("label_TaskBudget")->text().contains("4"));
        QCOMPARE(dialog.findChild<QComboBox*>("combo_RadioArtistChoices")->count(),1);
        QVERIFY(!button(dialog,"button_TaskUndo")->isEnabled());
        QVERIFY(!button(dialog,"button_TaskResume")->isEnabled());
        QVERIFY(button(dialog,"button_TaskLock")->text().contains("解除"));
        value.insert("status","paused");value.insert("canUndo",true);dialog.setTaskSnapshot(value);
        QVERIFY(button(dialog,"button_TaskResume")->isEnabled());
    }
    void catalogArtistChoiceEmitsTheExactSelectedIdentityWithoutChatSubmission() {
        RadioDialog dialog;dialog.setAvailability(true,true,{});
        auto value=snapshot("needs_clarification");value.insert("preview",QJsonValue(QJsonValue::Null));
        value.insert("providedArtists",QJsonArray{QJsonObject{{"id",1},{"name","同名用户"}},
            QJsonObject{{"id",7763},{"name","G.E.M.邓紫棋"}}});
        dialog.setTaskSnapshot(value);
        auto *combo=dialog.findChild<QComboBox*>("combo_RadioArtistChoices");QCOMPARE(combo->count(),2);
        QSignalSpy selected(&dialog,&RadioDialog::artistSelected),generated(&dialog,&RadioDialog::generateRequested);
        QCOMPARE(selected.size(),0);combo->setCurrentIndex(1);QCOMPARE(selected.size(),0);
        button(dialog,"button_RadioLookupArtist")->click();QCOMPARE(selected.size(),1);QCOMPARE(generated.size(),0);
        const auto artist=qvariant_cast<AssistantArtist>(selected.first().first());
        QCOMPARE(artist.id,qint64(7763));QCOMPARE(artist.name,QString("G.E.M.邓紫棋"));
    }
    void interruptedRecoveryReasonIsVisibleBeforeExplicitResume() {
        RadioDialog dialog;dialog.setAvailability(true,true,{});
        auto value=snapshot("paused");
        value.insert("recoveryReason","服务器重启，结果未知的模型调用不会自动重放；继续可能产生新的调用费用。");
        dialog.setTaskSnapshot(value);
        const auto text=dialog.findChild<QLabel*>("label_TaskChecks")->text();
        QVERIFY(text.contains("不会自动重放"));QVERIFY(text.contains("调用费用"));
        QVERIFY(button(dialog,"button_TaskResume")->isEnabled());
    }
    void lostObservationStopsProgressAndOffersRecoveryWithoutFakingServerPause() {
        RadioDialog dialog;dialog.setAvailability(true,true,{});
        auto value=snapshot("planning");dialog.setTaskSnapshot(value);
        auto *progress=dialog.findChild<QProgressBar*>("progress_RadioGeneration");QVERIFY(progress);QVERIFY(!progress->isHidden());
        value.insert("clientConnection",QJsonObject{{"status","interrupted"},
            {"message","与后台断开，后台任务状态尚未确认。请点击找回任务或继续任务。"}});
        dialog.setTaskSnapshot(value);QVERIFY(progress->isHidden());
        const auto checks=dialog.findChild<QLabel*>("label_TaskChecks")->text();
        QVERIFY(checks.contains("状态尚未确认"));QVERIFY(!checks.contains("任务已暂停"));
        QVERIFY(button(dialog,"button_TaskRestore")->isEnabled());QVERIFY(button(dialog,"button_TaskResume")->isEnabled());
        QVERIFY(button(dialog,"button_TaskNew")->isEnabled());
        QVERIFY(button(dialog,"button_TaskResume")->toolTip().contains("不会自动重发"));
        value.remove("clientConnection");dialog.setTaskSnapshot(value);QVERIFY(!progress->isHidden());
        QVERIFY(!button(dialog,"button_TaskResume")->isEnabled());
    }
    void restoredToolsRequireAnExplicitLocalContinue_data() {
        QTest::addColumn<QString>("status"); QTest::addColumn<bool>("observationOnly");
        QTest::addColumn<bool>("hasPendingTools"); QTest::addColumn<bool>("localContinue");
        QTest::newRow("restored-tools") << QString("waiting_tools") << true << true << true;
        QTest::newRow("active-tools") << QString("waiting_tools") << false << true << false;
        QTest::newRow("no-tools") << QString("waiting_tools") << true << false << false;
        QTest::newRow("planning") << QString("planning") << true << true << false;
        QTest::newRow("researching") << QString("researching") << true << true << false;
        QTest::newRow("composing") << QString("composing") << true << true << false;
    }
    void restoredToolsRequireAnExplicitLocalContinue() {
        QFETCH(QString, status); QFETCH(bool, observationOnly);
        QFETCH(bool, hasPendingTools); QFETCH(bool, localContinue);
        RadioDialog dialog; dialog.setConversationMode(true); dialog.setAvailability(true, true, {});
        auto value = snapshot(status);
        value.insert("clientObservationOnly", observationOnly);
        value.insert("pendingSteps", hasPendingTools ? QJsonArray{QJsonObject{{"id", "local-step"}, {"skill", "scene.research"}}} : QJsonArray{});
        value.insert("recovery", QJsonObject{{"canResume", false}});
        dialog.setTaskSnapshot(value);
        QVERIFY(dialog.findChild<QProgressBar*>("progress_RadioGeneration")->isHidden());
        QCOMPARE(button(dialog, "button_TaskResume")->isEnabled(), localContinue);
        const auto stage = dialog.findChild<QLabel*>("label_RadioStage")->text();
        QCOMPARE(stage.contains(QStringLiteral("点击继续后执行本机曲库查询")), localContinue);
        if (localContinue) {
            QVERIFY(button(dialog, "button_TaskResume")->toolTip().contains(QStringLiteral("本机曲库查询")));
            QSignalSpy actions(&dialog, &RadioDialog::taskActionRequested);
            button(dialog, "button_TaskResume")->click();
            QCOMPARE(actions.size(), 1); QCOMPARE(actions.first().first().toString(), QString("resume"));
            value.insert("clientObservationOnly", false); dialog.setTaskSnapshot(value);
            QVERIFY(dialog.findChild<QProgressBar*>("progress_RadioGeneration")->isHidden());
            QVERIFY(!button(dialog, "button_TaskResume")->isEnabled());
        }
    }
    void actualProgressStageReplacesGenericStatusWithoutInventingAPercentage() {
        RadioDialog dialog; dialog.setAvailability(true, true, {});
        auto value = snapshot("researching");
        value.insert("progress", QJsonObject{{"taskId", "task-test"}, {"revision", 3}, {"stateVersion", 11},
            {"stage", "checking"}, {"message", "正在核对候选歌曲与要求"}});
        auto budget = value.value("budget").toObject(); budget.insert("cacheHits", 2); value.insert("budget", budget);
        dialog.setTaskSnapshot(value);
        auto *stage = dialog.findChild<QLabel*>("label_RadioStage");
        QCOMPARE(stage->text(), QString("正在核对候选歌曲与要求"));
        QVERIFY(!stage->text().contains('%'));
        auto *progress = dialog.findChild<QProgressBar*>("progress_RadioGeneration");
        QCOMPARE(progress->minimum(), 0); QCOMPARE(progress->maximum(), 0);
        QVERIFY(dialog.findChild<QLabel*>("label_TaskBudget")->text().contains("复用近期曲库结果 2 次"));
        const auto screenshot = qEnvironmentVariable("TANGO_PROGRESS_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            dialog.resize(1100, 820); dialog.show(); QTest::qWait(80);
            QVERIFY(dialog.grab().save(screenshot));
        }
        auto old = value.value("progress").toObject(); old.insert("stateVersion", 9);
        old.insert("message", "旧阶段不能覆盖当前状态"); value.insert("progress", old); dialog.setTaskSnapshot(value);
        QCOMPARE(stage->text(), value.value("statusText").toString());
    }
    void taskCancelWaitsForBackendConfirmationBeforeClaimingCancellation() {
        RadioDialog dialog;dialog.setAvailability(true,true,{});dialog.setTaskSnapshot(snapshot("planning"));
        QSignalSpy cancelled(&dialog,&RadioDialog::cancelRequested);
        button(dialog,"button_RadioCancel")->click();QCOMPARE(cancelled.size(),1);
        auto *stage=dialog.findChild<QLabel*>("label_RadioStage");QVERIFY(stage);
        QVERIFY(stage->text().contains("等待后台确认"));QVERIFY(!stage->text().contains("已取消"));
    }
    void failedTaskShowsReasonAndRetryGuidanceWithoutPresentingOldSuccess_data() {
        QTest::addColumn<bool>("hasPreviousPreview");
        QTest::newRow("first-request-failed") << false;
        QTest::newRow("amendment-failed-with-old-preview") << true;
    }
    void failedTaskShowsReasonAndRetryGuidanceWithoutPresentingOldSuccess() {
        QFETCH(bool, hasPreviousPreview);
        RadioDialog dialog;dialog.setAvailability(true,true,{});
        auto value=snapshot("failed");
        value.insert("statusText","任务未完成，可修改要求");
        value.insert("briefSummary","上一份已经完成的旧需求");
        value.insert("messages",QJsonArray{QJsonObject{{"role","user"},{"content","读书时听陶喆，约40分钟"}}});
        value.insert("error",QJsonObject{{"code","INVALID_TASK_MODEL"},{"message","AI 返回格式无效，未能完成音乐任务。"}});
        value.insert("budget",QJsonObject{{"runCalls",1},{"totalCalls",1}});
        if(!hasPreviousPreview)value.insert("preview",QJsonValue(QJsonValue::Null));
        dialog.setTaskSnapshot(value);
        const auto *checks=dialog.findChild<QLabel*>("label_TaskChecks");
        QVERIFY(checks->text().startsWith("失败原因（INVALID_TASK_MODEL）：AI 返回格式无效"));
        QVERIFY(checks->text().contains("修改要求"));QVERIFY(checks->text().contains("继续任务"));
        QVERIFY(checks->text().contains("调用费用"));QCOMPARE(checks->textFormat(),Qt::PlainText);
        const auto summary=dialog.findChild<QLabel*>("label_TaskSummary")->text();
        QVERIFY(summary.startsWith("本次安排未完成"));QVERIFY(summary.contains("读书时听陶喆"));
        QVERIFY(!summary.contains("上一份已经完成"));
        QVERIFY(dialog.findChild<QLabel*>("label_TaskBudget")->text().contains("已保存失败进度"));
        QVERIFY(!button(dialog,"button_RadioPlay")->isEnabled());QVERIFY(button(dialog,"button_TaskResume")->isEnabled());
        QVERIFY(dialog.findChild<QPlainTextEdit*>("edit_RadioInstruction")->isEnabled());
        if(hasPreviousPreview) {
            QCOMPARE(dialog.recommendations().size(),18);
            QVERIFY(dialog.findChild<QLabel*>("label_RadioPlaylistTitle")->text().startsWith("上次成功歌单"));
            QVERIFY(dialog.findChild<QLabel*>("label_RadioPlaylistNote")->text().contains("本次修改失败"));
            QVERIFY(checks->text().contains("上一版"));
        } else {
            QVERIFY(dialog.recommendations().isEmpty());
            QCOMPARE(dialog.findChild<QLabel*>("label_RadioPlaylistTitle")->text(),QString("尚未生成歌单"));
        }
        value.insert("status","preview");value.insert("preview",preview());dialog.setTaskSnapshot(value);
        QVERIFY(!checks->text().contains("INVALID_TASK_MODEL"));QVERIFY(checks->styleSheet().isEmpty());
        QVERIFY(!dialog.findChild<QLabel*>("label_RadioPlaylistTitle")->text().startsWith("上次成功歌单"));
    }
    void accountResetCannotRevealPreviousFailureWhenTaskModeReturns() {
        RadioDialog dialog;dialog.setAvailability(true,true,{});
        auto value=snapshot("failed");
        value.insert("error",QJsonObject{{"code","INVALID_TASK_MODEL"},{"message","上个账号的失败原因"}});
        dialog.setTaskSnapshot(value);dialog.resetForAccount();
        dialog.setAvailability(true,true,{});dialog.setTaskMode(true);
        QVERIFY(dialog.findChild<QLabel*>("label_TaskChecks")->text().isEmpty());
        QVERIFY(dialog.findChild<QLabel*>("label_TaskChecks")->toolTip().isEmpty());
        QVERIFY(dialog.findChild<QLabel*>("label_TaskChecks")->styleSheet().isEmpty());
        QVERIFY(dialog.findChild<QLabel*>("label_TaskBudget")->text().isEmpty());
        QVERIFY(!dialog.findChild<QLabel*>("label_TaskSummary")->text().contains("保留前三首"));
        QVERIFY(!button(dialog,"button_TaskUndo")->isEnabled());
    }
    void exhaustedBudgetDisablesNoOpContinueAndShowsTheCorrectNextAction_data() {
        QTest::addColumn<QString>("action");QTest::addColumn<QString>("message");
        QTest::newRow("run-limit") << QString("new_turn") << QString("本轮调用次数已用完，请修改要求后重新发送。");
        QTest::newRow("task-limit") << QString("new_task") << QString("此任务总额度已用完，请新建任务，每日额度不会重置。");
        QTest::newRow("daily-limit") << QString("retry_later") << QString("今日额度已用完，请等待额度恢复后继续。");
    }
    void exhaustedBudgetDisablesNoOpContinueAndShowsTheCorrectNextAction() {
        QFETCH(QString,action);QFETCH(QString,message);
        RadioDialog dialog;dialog.setAvailability(true,true,{});
        auto value=snapshot("budget_exhausted");
        value.insert("error",QJsonObject{{"code","TASK_BUDGET"},{"message","达到调用上限，已保存进度。"}});
        value.insert("recovery",QJsonObject{{"canResume",false},{"action",action},{"message",message}});
        dialog.setTaskSnapshot(value);
        QVERIFY(!button(dialog,"button_TaskResume")->isEnabled());
        QCOMPARE(button(dialog,"button_TaskResume")->toolTip(),message);
        QVERIFY(dialog.findChild<QLabel*>("label_TaskChecks")->text().contains(message));
        QVERIFY(button(dialog,"button_TaskNew")->isEnabled());
        QVERIFY(dialog.findChild<QPlainTextEdit*>("edit_RadioInstruction")->isEnabled());
        value.insert("recovery",QJsonObject{{"canResume",true},{"action","resume"},{"message",""}});
        value.insert("status","paused");dialog.setTaskSnapshot(value);
        QVERIFY(button(dialog,"button_TaskResume")->isEnabled());
    }
    void embeddedTaskGeometry_data() {
        QTest::addColumn<QSize>("requested");
        QTest::newRow("compact")<<QSize(960,720);
        QTest::newRow("large")<<QSize(1280,860);
    }
    void embeddedTaskGeometry() {
        QFETCH(QSize,requested);
        QMainWindow window;PlayerWindowView view;view.setupUi(&window);
        auto *dialog=new RadioDialog(&window);dialog->setEmbedded(true);dialog->setWindowFlags(Qt::Widget);
        dialog->setConversationMode(true);
        dialog->setAvailability(true,true,{});dialog->setPreference("喜欢华语 R&B；少推荐现场版；偏好晚间放松");
        auto value=snapshot();value.insert("providedArtists",QJsonArray{QJsonObject{{"id",7763},{"name","G.E.M.邓紫棋"}}});
        dialog->setTaskSnapshot(value);dialog->setConversation(value.value("messages").toArray());
        view.setPage(dialog);window.resize(requested);window.show();
        QTest::qWait(20);
        // 原生 macOS 会压回超出屏幕的窗口；桌面容不下时验收可见边界与最小尺寸。
        const auto area = window.screen()->availableGeometry().size();
        const auto frame = window.frameGeometry().size() - window.size();
        const QSize expected(qMax(1000,requested.width()),qMax(700,requested.height()));
        if (expected.height() + frame.height() <= area.height()) QCOMPARE(window.size(), expected);
        else {
            QCOMPARE(window.width(), expected.width());
            QVERIFY(window.height() >= 700);
            QVERIFY(window.frameGeometry().height() <= area.height());
        }
        auto *scroll=dialog->findChild<QScrollArea*>("scroll_RadioPage");QVERIFY(scroll);
        QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
        auto *transcript = dialog->findChild<QPlainTextEdit*>("text_RadioConversation"); QVERIFY(transcript);
        QVERIFY(scroll->isHidden());
        const QRect conversationRect(transcript->mapTo(dialog, QPoint()), transcript->size());
        const int visibleConversationHeight = conversationRect.intersected(dialog->rect()).height();
        QVERIFY2(visibleConversationHeight >= 180,
            qPrintable(QStringLiteral("Only %1px of the main conversation are visible").arg(visibleConversationHeight)));
        QVERIFY(window.grab().save(QStringLiteral("/tmp/tango-task-ui-%1-top.png").arg(expected.width())));
        for(const char *name:{"button_RadioPlay","button_TaskUndo","table_RadioRecommendations"}) {
            auto *widget=dialog->findChild<QWidget*>(name);QVERIFY(widget);
            QVERIFY2(!widget->isVisible(),name);
        }
        auto *composer = dialog->findChild<QWidget*>("panel_RadioComposer"); QVERIFY(composer);
        const auto composerBefore = composer->mapTo(dialog, QPoint());
        transcript->verticalScrollBar()->setValue(0); QCoreApplication::processEvents();
        transcript->verticalScrollBar()->setValue(transcript->verticalScrollBar()->maximum()); QCoreApplication::processEvents();
        QCOMPARE(composer->mapTo(dialog, QPoint()), composerBefore);
        QVERIFY(dialog->rect().contains(QRect(composerBefore, composer->size())));
        QVERIFY(window.grab().save(QStringLiteral("/tmp/tango-task-ui-%1-bottom.png").arg(expected.width())));
        qInfo()<<"task shell requested"<<requested<<"actual"<<window.size()<<"page"<<dialog->size()<<"scroll content"<<scroll->widget()->size();
    }

    void taskHidesAndRejectsWithoutCancellingBackgroundTask() {
        QStackedWidget stack;stack.resize(900,700);
        auto *dialog=new RadioDialog;dialog->setEmbedded(true);dialog->setWindowFlags(Qt::Widget);
        dialog->setAvailability(true,true,{});dialog->setTaskSnapshot(snapshot("researching"));
        stack.addWidget(dialog);auto *other=new QWidget;stack.addWidget(other);stack.show();
        QSignalSpy cancel(dialog,&RadioDialog::cancelRequested);
        stack.setCurrentWidget(other);QCoreApplication::processEvents();QCOMPARE(cancel.size(),0);
        stack.setCurrentWidget(dialog);dialog->reject();QCOMPARE(cancel.size(),0);
    }
    void taskInputAcceptsAmendmentsDuringResearchWithoutPlayback() {
        RadioDialog dialog;dialog.setAvailability(true,true,{});dialog.setTaskSnapshot(snapshot("waiting_tools"));
        auto *input=dialog.findChild<QPlainTextEdit*>("edit_RadioInstruction");
        QVERIFY(input->isEnabled());input->setPlainText("前三首保留，后半段轻快一点");
        QSignalSpy generated(&dialog,&RadioDialog::generateRequested),played(&dialog,&RadioDialog::playRequested);
        button(dialog,"button_RadioGenerate")->click();QCOMPARE(generated.size(),1);QCOMPARE(played.size(),0);
        QVERIFY(!button(dialog,"button_RadioPlay")->isEnabled());
    }
    void restoredPreviewRendersConversationChecksAndAllSongs() {
        RadioDialog dialog;dialog.setAvailability(true,true,{});dialog.setTaskSnapshot(snapshot());
        QCOMPARE(dialog.recommendations().size(),18);
        QCOMPARE(dialog.findChild<QTableWidget*>("table_RadioRecommendations")->rowCount(),18);
        QVERIFY(dialog.findChild<QLabel*>("label_TaskSummary")->text().contains("保留3首"));
        const auto checks=dialog.findChild<QLabel*>("label_TaskChecks")->text();
        QVERIFY(checks.contains("✓"));QVERIFY(checks.contains("待核验"));
        QVERIFY(dialog.findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().contains("保留前三首"));
        QSignalSpy actions(&dialog,&RadioDialog::taskActionRequested);
        button(dialog,"button_TaskUndo")->click();QCOMPARE(actions.size(),1);QCOMPARE(actions.first().first().toString(),QString("undo"));
    }
    void resetAccountClearsPrivateTaskPresentation() {
        RadioDialog dialog;dialog.setAvailability(true,true,{});dialog.setPreference("喜欢爵士");dialog.setTaskSnapshot(snapshot());
        dialog.resetForAccount();QVERIFY(dialog.recommendations().isEmpty());
        QVERIFY(dialog.findChild<QPlainTextEdit*>("text_RadioConversation")->toPlainText().isEmpty());
        QVERIFY(!dialog.findChild<QLabel*>("label_RadioPreference")->text().contains("喜欢爵士"));
        QVERIFY(!dialog.findChild<QPlainTextEdit*>("edit_RadioInstruction")->isEnabled());
        QVERIFY(dialog.findChild<QWidget*>("panel_MusicTask")->isHidden());
    }
    void preferenceItemRemovalPreservesOtherEntriesAndRequiresSave() {
        RadioDialog dialog;dialog.setAvailability(true,true,{});dialog.setPreference("喜欢华语；偏爱爵士；少推摇滚");
        QSignalSpy saved(&dialog,&RadioDialog::preferenceEdited);
        bool edited=false;
        QTimer::singleShot(0,&dialog,[&]{
            auto *editor=dialog.findChild<QInputDialog*>("dialog_RadioPreferenceEditor");
            QVERIFY(editor);auto *items=editor->findChild<QListWidget*>("list_PreferenceItems");
            QVERIFY(items);QCOMPARE(items->count(),3);items->setCurrentRow(1);
            auto *remove=editor->findChild<QPushButton*>("button_RemovePreferenceItem");QVERIFY(remove);remove->click();
            QCOMPARE(editor->textValue(),QString("喜欢华语；少推摇滚"));QCOMPARE(saved.size(),0);
            edited=true;editor->accept();
        });
        button(dialog,"button_RadioEditPreference")->click();QVERIFY(edited);QCOMPARE(saved.size(),1);
        QCOMPARE(saved.first().first().toString(),QString("喜欢华语；少推摇滚"));
    }
};
QTEST_MAIN(MusicTaskDialogTest)
#include "music_task_dialog_test.moc"
