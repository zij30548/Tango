#include <QtTest>
#include <QLabel>
#include <QComboBox>
#include <QInputDialog>
#include <QLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextDocument>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTimer>
#include <QScrollArea>
#include <QScrollBar>
#include <QInputMethodEvent>
#include <QTemporaryDir>
#include <QFile>
#include <QBuffer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <cstring>

#include "radiodialog.h"
#include "songlistdelegate.h"
#include "coverartcache.h"

namespace {
QString fixtureCoverUrl()
{
    // 保留生产白名单格式；该逻辑地址仅由下方内存夹具响应，绝不交给真实网络。
    return QStringLiteral("https://p1.music.126.net/tango-owned-offline-fixture.png");
}

QByteArray fixtureCoverPng()
{
    QImage image(QSize(64, 64), QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#6da88c")));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

class FixtureImageReply : public QNetworkReply
{
public:
    FixtureImageReply(const QNetworkRequest &request, bool accepted, int delayMs, QObject *parent)
        : QNetworkReply(parent), body(accepted ? fixtureCoverPng() : QByteArray())
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, accepted ? 200 : 403);
        setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/png"));
        if (!accepted) setError(QNetworkReply::ContentAccessDenied, QStringLiteral("测试禁止真实图片网络"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        // 与真实下载相同地异步发信号，不能靠同步返回图片绕过生产解码链。
        QTimer::singleShot(delayMs, this, [this] {
            if (isFinished()) return;
            available = true;
            emit downloadProgress(body.size(), body.size());
            emit readyRead();
            setFinished(true);
            emit finished();
        });
    }
    void abort() override
    {
        if (isFinished()) return;
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("夹具请求已取消"));
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override
    {
        return (available ? body.size() - position : 0) + QNetworkReply::bytesAvailable();
    }
    bool isSequential() const override { return true; }
protected:
    qint64 readData(char *target, qint64 maximum) override
    {
        if (!available || position >= body.size()) return -1;
        const auto count = qMin(maximum, qint64(body.size()) - position);
        std::memcpy(target, body.constData() + position, size_t(count));
        position += count;
        return count;
    }
private:
    QByteArray body;
    qint64 position = 0;
    bool available = false;
};

class FixtureImageNetwork : public QNetworkAccessManager
{
public:
    explicit FixtureImageNetwork(QObject *parent) : QNetworkAccessManager(parent) {}
    int requestCount = 0;
    int responseDelayMs = 0;
protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *) override
    {
        ++requestCount;
        const bool accepted = operation == GetOperation
            && request.url() == CoverArtCache::normalizedCoverUrl(fixtureCoverUrl());
        if (!accepted) QTest::qFail("封面夹具收到未登记请求；禁止回落到公网", __FILE__, __LINE__);
        // 无论输入是否预期，都不调用基类 createRequest，因而不会发生 DNS 或套接字访问。
        return new FixtureImageReply(request, accepted, responseDelayMs, this);
    }
};

class ArtworkFixture
{
public:
    ArtworkFixture() : imageNetwork(nullptr), artwork(&imageNetwork, {}) {}
    FixtureImageNetwork imageNetwork;
    CoverArtCache artwork;
};

// 先构造普通拥有者，再把同一缓存注入界面；只有RadioDialog继承QObject。
class IsolatedRadioDialog : private ArtworkFixture, public RadioDialog
{
public:
    IsolatedRadioDialog() : ArtworkFixture(), RadioDialog(nullptr, &artwork) {}
    using ArtworkFixture::imageNetwork;
    using ArtworkFixture::artwork;
};

QImage conversationImage(RadioDialog &dialog, const QUrl &resource)
{
    auto *browser = dialog.findChild<QTextBrowser *>(QStringLiteral("browser_RadioConversation"));
    return browser->document()->resource(QTextDocument::ImageResource, resource).value<QImage>();
}

QString imageAnchor(RadioDialog &dialog, const QUrl &resource, QPoint *point = nullptr)
{
    auto *browser = dialog.findChild<QTextBrowser *>(QStringLiteral("browser_RadioConversation"));
    for (auto block = browser->document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            const auto format = fragment.charFormat();
            if (!fragment.isValid() || !format.isImageFormat() || format.toImageFormat().name() != resource.toString()) continue;
            if (point) {
                QTextCursor cursor(browser->document()); cursor.setPosition(fragment.position());
                browser->setTextCursor(cursor); browser->ensureCursorVisible();
                *point = browser->cursorRect(cursor).topLeft() + QPoint(24, 24);
            }
            return format.anchorHref();
        }
    return {};
}

int matchingPixels(const QImage &image, const QColor &color)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y) == color) ++count;
    return count;
}

QJsonObject accountUsage(bool disabled = false)
{
    return {{"daily", QJsonObject{{"limit", 30}, {"used", 4}, {"remaining", 26}, {"resetAt", "2026-10-21T00:00:00.000Z"}}},
        {"total", QJsonObject{{"limit", 300}, {"used", 12}, {"remaining", 288}}}, {"disabled", disabled}};
}
RadioRecommendation recommendation(qint64 id = 42)
{
    RadioRecommendation value;
    value.song.id = id;
    value.song.title = QStringLiteral("<b>夜色与风</b>");
    value.song.artist = QStringLiteral("示例歌手");
    value.song.album = QStringLiteral("夜行");
    value.song.coverUrl = fixtureCoverUrl();
    value.song.durationMs = 190000;
    value.reason = QStringLiteral("来自适合夜晚散步的音乐收藏。");
    value.sourceName = QStringLiteral("夜间漫步");
    value.sourceTags = QStringList{QStringLiteral("夜晚"), QStringLiteral("流行")};
    return value;
}

QPushButton *push(RadioDialog &dialog, const char *name)
{
    auto *result = dialog.findChild<QPushButton *>(QString::fromLatin1(name));
    Q_ASSERT(result);
    return result;
}

QString firstTrackLink(RadioDialog &dialog)
{
    auto *browser = dialog.findChild<QTextBrowser *>(QStringLiteral("browser_RadioConversation"));
    for (auto block = browser->document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().anchorHref().startsWith(QStringLiteral("tango-track:")))
                return fragment.charFormat().anchorHref();
        }
    return {};
}

QJsonObject scenePreviewSnapshot(const QJsonArray &checks)
{
    // 保留真实诊断的八首与三条件形状；仅使用本地元数据，不触发封面下载。
    QJsonArray tracks;
    for (int index = 0; index < 8; ++index)
        tracks.append(QJsonObject{{"id", 100 + index}, {"title", QStringLiteral("歌曲%1").arg(index + 1)},
            {"artist", "G.E.M.邓紫棋"}, {"artists", QJsonArray{QJsonObject{{"id", 7763}, {"name", "G.E.M.邓紫棋"}}}},
            {"durationMs", 225000}});
    return {{"id", "warning-preview-task"}, {"revision", 2}, {"status", "preview"},
        {"brief", QJsonObject{{"scene", "散步"}, {"requiredArtist", "G.E.M.邓紫棋"},
            {"requiredArtistId", 7763}, {"artistOnly", true}, {"targetMinutes", 30}}},
        {"requirementPlan", QJsonObject{{"scene", "散步"}, {"artist", "邓紫棋"}, {"targetMinutes", 30}}},
        {"preview", QJsonObject{{"version", 1}, {"title", "邓紫棋 散步歌单"},
            {"note", "基于真实曲库候选与来源主题编排。"}, {"totalDurationMs", 1800000},
            {"tracks", tracks}, {"checks", checks}}}};
}
}

class RadioDialogTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void availabilityProtectsEveryGenerationEntry();
    void accountQuotaShowsVerifiedCountsWithoutChangingTheDraft();
    void accountQuotaRejectsUnknownOrMalformedCounts();
    void accountQuotaDisabledAndResetPreserveHistoryAndPlayback();
    void composerKeyboardImeAndLengthBoundary();
    void draftReceiptsDoNotLoseEditsOrCrossAccounts();
    void journalIsNotOverwrittenAndReadingPositionIsPreserved();
    void newConversationIsExplicitAndDisabledForPendingSubmission();
    void busyCancellationAndRecovery();
    void recommendationsPreserveMetadataAndRequireExplicitPlayback();
    void recommendationArtworkUsesLocalAsyncFixture();
    void conversationSongArtworkIsDecodedAndItsImageRemainsClickable();
    void conversationPlaylistArtworkIsDecodedWithoutUsingPlaylistIdAsSongId();
    void conversationArtworkRejectsMessageAndExternalResourceInjection();
    void conversationArtworkCannotReappearAfterAccountOrCandidateChanges();
    void recommendationsAndConversationAreBoundedPlainText();
    void feedbackAndPreferenceSignalsAreExplicit();
    void closingCancelsWorkWithoutStartingMusic();
    void accountResetClearsAllPrivatePresentation();
    void compactWindowFitsItsMinimumSize();
    void assistantExamplesRouteThroughTheSameChat();
    void artistChoicesAreBoundedAndRequireExplicitSelection();
    void preferenceReviewIsEditableBoundedAndExplicit();
    void stalePreferenceEditorCannotSaveIntoAnotherAccount();
    void conversationModeShowsOnlyChatAndComposer();
    void chatCommandsRequireCurrentCandidatesAndExplicitConfirmation();
    void clarificationCannotOfferPreviousPreviewAsCurrentSuccess();
    void chatPreviewShowsCurrentLocksAndClearsStaleAccountState();
    void chatPreviewShowsSceneEvidenceGapInVisibleConversation();
    void chatPreviewShowsDurationAndArtistFailuresAndClearsOldWarnings();
    void chatPreviewWarningsEscapeMarkupAndExternalLinks();
    void chatPreviewDisplaysOnlyExplicitVersion_data();
    void chatPreviewDisplaysOnlyExplicitVersion();
    void clearHistoryIsExplicitAndDisablesLocalActionsDuringDelete();
    void staleSongAnchorsDoNotPlayAcrossSnapshotsOrAccounts();
};

void RadioDialogTest::initTestCase()
{
    // 测试只使用临时账号目录和禁用端口，不访问真实曲库或用户资料。
    static QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    qputenv("MUSICPLAYER_DATA_DIR", temporary.path().toUtf8());
    qputenv("MUSICPLAYER_NETEASE_PORT", "1");
}

void RadioDialogTest::composerKeyboardImeAndLengthBoundary()
{
    IsolatedRadioDialog dialog;
    dialog.setAvailability(true, true, {});
    auto *input = dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction");
    QSignalSpy generated(&dialog, &RadioDialog::generateRequested);
    input->setPlainText(QStringLiteral("第一行"));
    input->moveCursor(QTextCursor::End);
    QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
    QVERIFY(input->toPlainText().contains(QLatin1Char('\n')));
    QCOMPARE(generated.size(), 0);
    QInputMethodEvent preedit(QStringLiteral("yin"), {});
    QApplication::sendEvent(input, &preedit);
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(generated.size(), 0);
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("音乐"));
    QApplication::sendEvent(input, &commit);
    input->setPlainText(QString(1001, QChar(0x97F3)));
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(generated.size(), 0);
    QCOMPARE(input->toPlainText().size(), 1001);
    QVERIFY(!push(dialog, "button_RadioGenerate")->isEnabled());
    input->setPlainText(QString(1000, QChar(0x97F3)));
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(generated.size(), 1);
    QCOMPARE(generated.first().first().toString().size(), 1000);
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(generated.size(), 1);
    dialog.submissionAccepted(generated.first().first().toString(), dialog.currentSubmissionId());
    input->setPlainText(QString::fromUtf8("🎵").repeated(500));
    QCOMPARE(input->toPlainText().size(), 1000);
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(generated.size(), 2);
    dialog.submissionFailed(generated.last().first().toString(), dialog.currentSubmissionId());
    input->insertPlainText(QString::fromUtf8("🎵"));
    QCOMPARE(input->toPlainText().size(), 1002);
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(generated.size(), 2);
}

void RadioDialogTest::draftReceiptsDoNotLoseEditsOrCrossAccounts()
{
    IsolatedRadioDialog dialog;
    dialog.setAvailability(true, true, {});
    auto *input = dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction");
    QSignalSpy generated(&dialog, &RadioDialog::generateRequested);
    input->setPlainText(QStringLiteral("第一条"));
    push(dialog, "button_RadioGenerate")->click();
    const auto first = dialog.currentSubmissionId();
    QCOMPARE(generated.size(), 1);
    QCOMPARE(input->toPlainText(), QStringLiteral("第一条"));
    push(dialog, "button_RadioGenerate")->click();
    QCOMPARE(generated.size(), 1);
    input->setPlainText(QStringLiteral("下一条草稿"));
    dialog.submissionAccepted(QStringLiteral("第一条"), first);
    QCOMPARE(input->toPlainText(), QStringLiteral("下一条草稿"));
    push(dialog, "button_RadioGenerate")->click();
    const auto failed = dialog.currentSubmissionId();
    dialog.submissionFailed(QStringLiteral("下一条草稿"), failed);
    QCOMPARE(input->toPlainText(), QStringLiteral("下一条草稿"));
    QVERIFY(push(dialog, "button_RadioGenerate")->isEnabled());
    input->setPlainText(QStringLiteral("同文"));
    push(dialog, "button_RadioGenerate")->click();
    const auto stale = dialog.currentSubmissionId();
    QInputMethodEvent composing(QStringLiteral("zhong"), {});
    QApplication::sendEvent(input, &composing);
    dialog.resetForAccount();
    dialog.setAvailability(true, true, {});
    input->setPlainText(QStringLiteral("同文"));
    QTest::keyClick(input, Qt::Key_Return);
    const auto current = dialog.currentSubmissionId();
    QVERIFY(current != stale);
    dialog.submissionAccepted(QStringLiteral("同文"), stale);
    QCOMPARE(input->toPlainText(), QStringLiteral("同文"));
    QCOMPARE(dialog.currentSubmissionId(), current);
    dialog.submissionAccepted(QStringLiteral("同文"), current);
    QVERIFY(input->toPlainText().isEmpty());
}

void RadioDialogTest::journalIsNotOverwrittenAndReadingPositionIsPreserved()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    dialog.resize(900, 740);
    dialog.show();
    QJsonArray journal;
    for (int i = 0; i < 60; ++i) journal.append(QJsonObject{{"id", QString::number(i)},
        {"sequence", i + 1}, {"role", "assistant"}, {"content", QStringLiteral("已确认的会话消息 %1").arg(i)}});
    dialog.setConversation(journal);
    QCoreApplication::processEvents();
    auto *transcript = dialog.findChild<QPlainTextEdit *>("text_RadioConversation");
    QVERIFY(transcript->toPlainText().contains(QStringLiteral("会话消息 0")));
    auto *scroll = transcript->verticalScrollBar();
    QVERIFY(scroll->maximum() > 24);
    scroll->setValue(0);
    journal.append(QJsonObject{{"id", "61"}, {"role", "assistant"}, {"content", "new live message"}});
    dialog.setConversation(journal);
    QCOMPARE(scroll->value(), 0);
    const int oldMaximum = scroll->maximum();
    journal.insert(0, QJsonObject{{"id", "older-page"}, {"role", "assistant"}, {"content", "older restored message"}});
    dialog.setConversation(journal);
    QCOMPARE(scroll->value(), scroll->maximum() - oldMaximum);
    QVERIFY(scroll->value() < scroll->maximum());
    dialog.setTaskSnapshot(QJsonObject{{"status", "preview"}, {"messages", QJsonArray{
        QJsonObject{{"role", "assistant"}, {"content", "task-only obsolete text"}}}}});
    QVERIFY(transcript->toPlainText().contains("new live message"));
    QVERIFY(!transcript->toPlainText().contains("task-only obsolete"));
    dialog.setTaskSnapshot({});
    QVERIFY(transcript->toPlainText().contains("new live message"));
    scroll->setValue(scroll->maximum());
    journal.append(QJsonObject{{"role", "assistant"}, {"content", "follow latest"}});
    dialog.setConversation(journal);
    QCOMPARE(scroll->value(), scroll->maximum());
    dialog.setEarlierMessagesAvailable(true);
    QSignalSpy earlier(&dialog, &RadioDialog::earlierMessagesRequested);
    push(dialog, "button_RadioEarlierMessages")->click();
    QCOMPARE(earlier.size(), 1);
    dialog.resetForAccount();
    QVERIFY(transcript->toPlainText().isEmpty());
    QVERIFY(push(dialog, "button_RadioEarlierMessages")->isHidden());
}

void RadioDialogTest::newConversationIsExplicitAndDisabledForPendingSubmission()
{
    IsolatedRadioDialog dialog;
    auto *newConversation = push(dialog, "button_ConversationNew");
    QVERIFY(newConversation->isHidden());
    dialog.setConversationMode(true);
    QVERIFY(newConversation->isHidden());
    QVERIFY(!newConversation->isEnabled());
    dialog.setAvailability(true, true, {});
    QSignalSpy actions(&dialog, &RadioDialog::taskActionRequested);
    QVERIFY(dialog.executeChatAction(QStringLiteral("新对话")));
    QCOMPARE(actions.size(), 1);
    QCOMPARE(actions.first().first().toString(), QStringLiteral("new_conversation"));
    auto *input = dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction");
    input->setPlainText(QStringLiteral("进行中"));
    push(dialog, "button_RadioGenerate")->click();
    QVERIFY(!newConversation->isEnabled());
    newConversation->click();
    QCOMPARE(actions.size(), 1);
    dialog.submissionFailed(QStringLiteral("进行中"), dialog.currentSubmissionId());
    QVERIFY(newConversation->isEnabled());
    dialog.setBusy(true);
    QVERIFY(!newConversation->isEnabled());
}

void RadioDialogTest::availabilityProtectsEveryGenerationEntry()
{
    IsolatedRadioDialog dialog;
    auto *input = dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction");
    auto *availability = dialog.findChild<QLabel *>("label_RadioAvailability");
    QSignalSpy generated(&dialog, &RadioDialog::generateRequested);
    QSignalSpy login(&dialog, &RadioDialog::loginRequested);
    input->setPlainText(QStringLiteral("想听中文歌"));
    QVERIFY(!input->isEnabled());
    QVERIFY(!push(dialog, "button_RadioGenerate")->isEnabled());
    QVERIFY(availability->text().contains(QStringLiteral("登录探歌")));
    push(dialog, "button_RadioGenerate")->click();
    QCOMPARE(generated.size(), 0);
    push(dialog, "button_RadioLogin")->click();
    QCOMPARE(login.size(), 1);

    dialog.setAvailability(true, false, QStringLiteral("后台暂未配置模型"));
    QCOMPARE(availability->text(), QStringLiteral("后台暂未配置模型"));
    QVERIFY(!input->isEnabled());
    push(dialog, "button_RadioGenerate")->click();
    QCOMPARE(generated.size(), 0);
    dialog.setAvailability(true, true, QStringLiteral("AI 已就绪"));
    QVERIFY(input->isEnabled());
    QVERIFY(push(dialog, "button_RadioGenerate")->isEnabled());
    push(dialog, "button_RadioGenerate")->click();
    QCOMPARE(generated.size(), 1);
    QCOMPARE(generated.takeFirst().first().toString(), QStringLiteral("想听中文歌"));
    QCOMPARE(input->toPlainText(), QStringLiteral("想听中文歌"));
    QVERIFY(!push(dialog, "button_RadioGenerate")->isEnabled());
    dialog.submissionAccepted(QStringLiteral("想听中文歌"), dialog.currentSubmissionId());
    QVERIFY(input->toPlainText().isEmpty());
    input->setPlainText(QStringLiteral("   "));
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(generated.size(), 0);
}

void RadioDialogTest::accountQuotaShowsVerifiedCountsWithoutChangingTheDraft()
{
    IsolatedRadioDialog dialog; dialog.setAvailability(true, true, QStringLiteral("AI 已就绪"));
    auto *input = dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction");
    auto *quota = dialog.findChild<QLabel *>("label_RadioQuota"); QVERIFY(quota);
    input->setPlainText(QStringLiteral("今晚想听爵士"));
    dialog.setAccountUsage(accountUsage());
    QCOMPARE(quota->text(), QStringLiteral("今日剩余 26/30 次 · 内测剩余 288 次"));
    QVERIFY(!quota->isHidden());
    QVERIFY(quota->toolTip().contains(QStringLiteral("模型尝试")));
    QVERIFY(quota->toolTip().contains(QStringLiteral("失败或结果不确定")));
    QVERIFY(quota->toolTip().contains(QStringLiteral("不是歌曲数量或聊天条数")));
    QVERIFY(quota->toolTip().contains(QStringLiteral("累计额度不随日期重置")));
    QCOMPARE(input->toPlainText(), QStringLiteral("今晚想听爵士"));
    QCOMPARE(dialog.findChild<QLabel *>("label_RadioAvailability")->text(), QStringLiteral("AI 已就绪"));
}

void RadioDialogTest::accountQuotaRejectsUnknownOrMalformedCounts()
{
    IsolatedRadioDialog dialog; dialog.setAvailability(true, true, {});
    auto *quota = dialog.findChild<QLabel *>("label_RadioQuota");
    QJsonArray malformed{QJsonObject(), QJsonObject{{"daily", QJsonObject()}, {"total", QJsonObject()}, {"disabled", false}}};
    for (const auto &key : QStringList{"limit", "used", "remaining", "resetAt"}) {
        auto value = accountUsage(); auto daily = value.value("daily").toObject();
        daily.insert(key, key == "resetAt" ? QJsonValue("unknown") : QJsonValue(-1)); value.insert("daily", daily); malformed.append(value);
    }
    auto inconsistent = accountUsage(); auto daily = inconsistent.value("daily").toObject();
    daily.insert("remaining", 25); inconsistent.insert("daily", daily); malformed.append(inconsistent);
    auto fractional = accountUsage(); daily = fractional.value("daily").toObject();
    daily.insert("used", 4.5); fractional.insert("daily", daily); malformed.append(fractional);
    auto wrongDisabled = accountUsage(); wrongDisabled.insert("disabled", "false"); malformed.append(wrongDisabled);
    for (const auto &value : malformed) {
        dialog.setAccountUsage(accountUsage()); QVERIFY(!quota->isHidden());
        dialog.setAccountUsage(value.toObject()); QVERIFY(quota->isHidden()); QVERIFY(quota->text().isEmpty());
        QVERIFY(quota->toolTip().isEmpty());
    }
    auto zero = accountUsage(); daily = zero.value("daily").toObject(); daily.insert("limit", 0); daily.insert("remaining", 0); zero.insert("daily", daily);
    dialog.setAccountUsage(zero); QVERIFY(quota->text().contains(QStringLiteral("今日剩余 0/0 次")));
    dialog.setAccountUsage({}); QVERIFY(quota->isHidden());
    QVERIFY(dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction")->isEnabled());
}

void RadioDialogTest::accountQuotaDisabledAndResetPreserveHistoryAndPlayback()
{
    IsolatedRadioDialog dialog; dialog.setAvailability(true, true, {});
    dialog.appendMessage(QStringLiteral("你"), QStringLiteral("保留聊天历史"));
    dialog.setRecommendations(QStringLiteral("保留播放歌单"), {}, {recommendation()});
    auto *quota = dialog.findChild<QLabel *>("label_RadioQuota");
    dialog.setAccountUsage(accountUsage(true));
    QCOMPARE(quota->text(), QStringLiteral("AI 已停用"));
    QVERIFY(dialog.findChild<QPlainTextEdit *>("text_RadioConversation")->toPlainText().contains(QStringLiteral("保留聊天历史")));
    QVERIFY(push(dialog, "button_RadioPlay")->isEnabled()); QCOMPARE(dialog.recommendations().size(), 1);
    dialog.resetForAccount(); QVERIFY(quota->isHidden()); QVERIFY(quota->text().isEmpty()); QVERIFY(quota->toolTip().isEmpty());
    dialog.setAccountUsage(accountUsage()); QVERIFY(quota->isHidden());
    dialog.setAvailability(true, true, {}); dialog.setAccountUsage(accountUsage()); QVERIFY(!quota->isHidden());
    dialog.setAvailability(false, false, {}); QVERIFY(quota->isHidden()); QVERIFY(quota->text().isEmpty());
}

void RadioDialogTest::busyCancellationAndRecovery()
{
    IsolatedRadioDialog dialog;
    dialog.setAvailability(true, true, {});
    dialog.setRecommendations(QStringLiteral("夜间电台"), {}, {recommendation()});
    auto *input = dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction");
    input->setPlainText(QStringLiteral("换一些轻快的"));
    QSignalSpy generated(&dialog, &RadioDialog::generateRequested);
    QSignalSpy canceled(&dialog, &RadioDialog::cancelRequested);
    QSignalSpy played(&dialog, &RadioDialog::playRequested);
    dialog.setBusy(true, QStringLiteral("正在寻找真实曲目…"));
    QCOMPARE(dialog.findChild<QLabel *>("label_RadioStage")->text(), QStringLiteral("正在寻找真实曲目…"));
    QVERIFY(!input->isEnabled());
    QVERIFY(!push(dialog, "button_RadioPlay")->isEnabled());
    QVERIFY(!push(dialog, "button_RadioRemember")->isEnabled());
    push(dialog, "button_RadioGenerate")->click();
    push(dialog, "button_RadioPlay")->click();
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(generated.size(), 0);
    QCOMPARE(played.size(), 0);
    push(dialog, "button_RadioCancel")->click();
    QCOMPARE(canceled.size(), 1);
    QVERIFY(input->isEnabled());
    QVERIFY(push(dialog, "button_RadioGenerate")->isEnabled());
    QVERIFY(push(dialog, "button_RadioPlay")->isEnabled());
    push(dialog, "button_RadioCancel")->click();
    QCOMPARE(canceled.size(), 1);
    dialog.setBusy(true);
    dialog.setBusy(false, QStringLiteral("网络暂不可用，请重试。"));
    QVERIFY(push(dialog, "button_RadioGenerate")->isEnabled());
    QVERIFY(dialog.findChild<QLabel *>("label_RadioStage")->text().contains(QStringLiteral("重试")));
}

void RadioDialogTest::recommendationsPreserveMetadataAndRequireExplicitPlayback()
{
    IsolatedRadioDialog dialog;
    dialog.setAvailability(true, true, {});
    QSignalSpy played(&dialog, &RadioDialog::playRequested);
    QSignalSpy activated(&dialog, &RadioDialog::trackActivated);
    dialog.setRecommendations(QStringLiteral("晚风电台"), QStringLiteral("听一组适合散步的歌曲。"), {recommendation()});
    QCOMPARE(played.size(), 0);
    QCOMPARE(activated.size(), 0);
    auto *table = dialog.findChild<QTableWidget *>("table_RadioRecommendations");
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->columnCount(), 2);
    QVERIFY(qobject_cast<SongListDelegate *>(table->itemDelegate()));
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("<b>夜色与风</b>"));
    QCOMPARE(table->item(0, 0)->data(SongListDelegate::ArtistRole).toString(), recommendation().song.artist);
    QCOMPARE(table->item(0, 0)->data(SongListDelegate::CoverUrlRole).toString(), recommendation().song.coverUrl);
    QCOMPARE(table->item(0, 0)->data(SongListDelegate::SongIdRole).toLongLong(), qint64(42));
    QVERIFY(table->item(0, 1)->toolTip().contains(QStringLiteral("来源：夜间漫步")));
    QVERIFY(!table->cellWidget(0, 0));
    QVERIFY(push(dialog, "button_RadioPlay")->toolTip().contains(QStringLiteral("替换当前播放队列")));
    push(dialog, "button_RadioPlay")->click();
    QCOMPARE(played.size(), 1);
    const auto playlist = qvariant_cast<QVector<RadioRecommendation>>(played.takeFirst().first());
    QCOMPARE(playlist.size(), 1);
    QCOMPARE(playlist.first().song.coverUrl, recommendation().song.coverUrl);
    QCOMPARE(playlist.first().sourceTags, recommendation().sourceTags);
    QVERIFY(QMetaObject::invokeMethod(table, "cellDoubleClicked", Q_ARG(int, 0), Q_ARG(int, 0)));
    QCOMPARE(activated.size(), 1);
    QCOMPARE(qvariant_cast<OnlineSong>(activated.first().first()).id, qint64(42));
}

void RadioDialogTest::recommendationArtworkUsesLocalAsyncFixture()
{
    IsolatedRadioDialog dialog;
    const auto value = recommendation();
    QSignalSpy ready(&dialog.artwork, &CoverArtCache::imageReady);
    QVERIFY(dialog.artwork.image({}, value.song.id, value.song.coverUrl).isNull());
    QCOMPARE(dialog.imageNetwork.requestCount, 1);
    QCOMPARE(ready.size(), 0);
    QTRY_COMPARE(ready.size(), 1);
    const auto image = dialog.artwork.image({}, value.song.id, value.song.coverUrl);
    QCOMPARE(image.size(), QSize(64, 64));
    QCOMPARE(image.pixelColor(32, 32), QColor(QStringLiteral("#6da88c")));
    dialog.setRecommendations(QStringLiteral("本机封面夹具"), {}, {value});
    auto *table = dialog.findChild<QTableWidget *>("table_RadioRecommendations");
    QImage painted(QSize(600, 72), QImage::Format_ARGB32);
    painted.fill(Qt::black);
    QPainter painter(&painted);
    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, 600, 72);
    option.state = QStyle::State_Enabled;
    table->itemDelegate()->paint(&painter, option, table->model()->index(0, 0));
    painter.end();
    // 验证真实行委托画出了成功解码的封面，而不是下载失败后的占位图。
    QCOMPARE(painted.pixelColor(35, 35), QColor(QStringLiteral("#6da88c")));
    QCOMPARE(dialog.imageNetwork.requestCount, 1);
}

void RadioDialogTest::conversationSongArtworkIsDecodedAndItsImageRemainsClickable()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    dialog.resize(900, 740);
    dialog.show();
    auto *browser = dialog.findChild<QTextBrowser *>(QStringLiteral("browser_RadioConversation"));
    QVERIFY(browser);
    QSignalSpy decoded(&dialog.artwork, &CoverArtCache::imageReady);
    QSignalSpy played(&dialog, &RadioDialog::trackActivated);
    dialog.setRecommendations(QStringLiteral("聊天中的真实解码封面"), {}, {recommendation(42)});
    const QUrl resource(QStringLiteral("tango-artwork:/song/0"));
    QCOMPARE(decoded.size(), 0);
    QCOMPARE(dialog.imageNetwork.requestCount, 1);
    QVERIFY(browser->toHtml().contains(QStringLiteral("<img src=\"tango-artwork:/song/0\"")));
    QTRY_COMPARE(decoded.size(), 1);
    QTRY_COMPARE(conversationImage(dialog, resource).size(), QSize(48, 48));
    QTRY_COMPARE(conversationImage(dialog, resource).pixelColor(24, 24), QColor(QStringLiteral("#6da88c")));
    QPoint point;
    const QString link = imageAnchor(dialog, resource, &point);
    QVERIFY(link.startsWith(QStringLiteral("tango-track://play/")));
    QCOMPARE(link, firstTrackLink(dialog));
    QTRY_COMPARE(browser->anchorAt(point), link);
    // 除了缓存对象，实际可见聊天视口也必须画出图片的解码像素。
    QImage painted(browser->viewport()->size(), QImage::Format_ARGB32);
    painted.fill(Qt::transparent);
    browser->viewport()->render(&painted);
    QVERIFY(matchingPixels(painted, QColor(QStringLiteral("#6da88c"))) >= 100);
    QTest::mouseClick(browser->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(played.size(), 1);
    QCOMPARE(qvariant_cast<OnlineSong>(played.first().first()).id, qint64(42));
    QCOMPARE(dialog.imageNetwork.requestCount, 1);
    const auto screenshot = qEnvironmentVariable("TANGO_ARTWORK_SCREENSHOT");
    if (!screenshot.isEmpty()) QVERIFY(dialog.grab().save(screenshot + QStringLiteral("-song.png")));
}

void RadioDialogTest::conversationPlaylistArtworkIsDecodedWithoutUsingPlaylistIdAsSongId()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    dialog.resize(900, 740);
    dialog.show();
    auto *browser = dialog.findChild<QTextBrowser *>(QStringLiteral("browser_RadioConversation"));
    QSignalSpy decoded(&dialog.artwork, &CoverArtCache::imageReady);
    QSignalSpy selected(&dialog, &RadioDialog::playlistActivated);
    dialog.setPlaylistCandidates(QJsonArray{QJsonObject{{"id", 90042}, {"name", "散步时刻"},
        {"trackCount", 10}, {"coverUrl", fixtureCoverUrl()}}});
    const QUrl resource(QStringLiteral("tango-artwork:/playlist/0"));
    QCOMPARE(decoded.size(), 0);
    QCOMPARE(dialog.imageNetwork.requestCount, 1);
    QTRY_COMPARE(decoded.size(), 1);
    QTRY_COMPARE(conversationImage(dialog, resource).size(), QSize(48, 48));
    QTRY_COMPARE(conversationImage(dialog, resource).pixelColor(24, 24), QColor(QStringLiteral("#6da88c")));
    QVERIFY(browser->toHtml().contains(QStringLiteral("<img src=\"tango-artwork:/playlist/0\"")));
    QPoint point;
    const auto link = imageAnchor(dialog, resource, &point);
    QVERIFY(link.startsWith(QStringLiteral("tango-playlist://show/")));
    QTRY_COMPARE(browser->anchorAt(point), link);
    QTest::mouseClick(browser->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(selected.size(), 1);
    QCOMPARE(selected.first().at(0).toLongLong(), qint64(90042));
    QCOMPARE(selected.first().at(1).toString(), QStringLiteral("散步时刻"));
    const auto screenshot = qEnvironmentVariable("TANGO_ARTWORK_SCREENSHOT");
    if (!screenshot.isEmpty()) QVERIFY(dialog.grab().save(screenshot + QStringLiteral("-playlist.png")));

    // 同一个歌单ID失去封面时不能冒充歌曲ID，继承旧图或触发song/detail补查。
    dialog.setPlaylistCandidates(QJsonArray{QJsonObject{{"id", 90042}, {"name", "无封面的歌单"}, {"trackCount", 5}}});
    QCOMPARE(conversationImage(dialog, resource).size(), QSize(48, 48));
    QVERIFY(conversationImage(dialog, resource).pixelColor(24, 24) != QColor(QStringLiteral("#6da88c")));
    QCOMPARE(dialog.artwork.activeRequestCount(), 0);
    QCOMPARE(dialog.artwork.queuedRequestCount(), 0);
    QCOMPARE(dialog.imageNetwork.requestCount, 1);
}

void RadioDialogTest::conversationArtworkRejectsMessageAndExternalResourceInjection()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    auto *browser = dialog.findChild<QTextBrowser *>(QStringLiteral("browser_RadioConversation"));
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QFile imageFile(temporary.filePath(QStringLiteral("private-image.png")));
    QVERIFY(imageFile.open(QIODevice::WriteOnly));
    imageFile.write(fixtureCoverPng()); imageFile.close();
    const QUrl local = QUrl::fromLocalFile(imageFile.fileName());
    const QUrl inlineImage(QStringLiteral("data:image/png;base64,") + QString::fromLatin1(fixtureCoverPng().toBase64()));
    const QString attack = QStringLiteral("<img src='%1'><img src='%2'><img src='%3'>"
        "<a href='tango-track://play/0/0/0'>假播放</a><a href='https://example.invalid/attack'>外链</a>")
        .arg(fixtureCoverUrl(), local.toString(), inlineImage.toString());
    dialog.appendMessage(QStringLiteral("探歌"), attack);
    QVERIFY(browser->toPlainText().contains(QStringLiteral("<img src=")));
    QVERIFY(!browser->toHtml().contains(QStringLiteral("<img ")));
    for (auto block = browser->document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto format = it.fragment().charFormat();
            QVERIFY(!format.isImageFormat());
            QVERIFY(!format.isAnchor());
        }
    // 直接询问资源加载器也应拒绝，不依赖图片路径不存在来碰巧通过。
    for (const auto &url : QList<QUrl>{QUrl(fixtureCoverUrl()), local, inlineImage})
        QVERIFY2(browser->document()->resource(QTextDocument::ImageResource, url).value<QImage>().isNull(), qPrintable(url.scheme()));
    QSignalSpy played(&dialog, &RadioDialog::trackActivated);
    QVERIFY(QMetaObject::invokeMethod(browser, "anchorClicked", Q_ARG(QUrl, QUrl(QStringLiteral("tango-track://play/0/0/0")))));
    QCOMPARE(played.size(), 0);
    QCOMPARE(dialog.imageNetwork.requestCount, 0);
    QCOMPARE(dialog.artwork.activeRequestCount(), 0);
    QCOMPARE(dialog.artwork.queuedRequestCount(), 0);
}

void RadioDialogTest::conversationArtworkCannotReappearAfterAccountOrCandidateChanges()
{
    IsolatedRadioDialog dialog;
    dialog.imageNetwork.responseDelayMs = 150;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    QSignalSpy decoded(&dialog.artwork, &CoverArtCache::imageReady);
    dialog.setRecommendations(QStringLiteral("旧账号正在下载"), {}, {recommendation(42)});
    QCOMPARE(dialog.imageNetwork.requestCount, 1);
    QCOMPARE(decoded.size(), 0);
    dialog.resetForAccount();
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    auto current = recommendation(42);
    current.song.coverUrl.clear();
    dialog.setRecommendations(QStringLiteral("新账号无封面"), {}, {current});
    const QUrl resource(QStringLiteral("tango-artwork:/song/0"));
    QTRY_COMPARE(decoded.size(), 1);
    QCOMPARE(conversationImage(dialog, resource).size(), QSize(48, 48));
    QVERIFY(conversationImage(dialog, resource).pixelColor(24, 24) != QColor(QStringLiteral("#6da88c")));
    QCOMPARE(dialog.artwork.activeRequestCount(), 0);
    QCOMPARE(dialog.artwork.queuedRequestCount(), 0);
    QCOMPARE(dialog.imageNetwork.requestCount, 1);

    current.song.id = 43;
    current.song.coverUrl = QStringLiteral("file:///not-a-program-artwork.png");
    dialog.setRecommendations(QStringLiteral("新候选的无效封面"), {}, {current});
    QVERIFY(conversationImage(dialog, resource).pixelColor(24, 24) != QColor(QStringLiteral("#6da88c")));
    QCOMPARE(dialog.artwork.activeRequestCount(), 0);
    QCOMPARE(dialog.artwork.queuedRequestCount(), 0);
    QCOMPARE(dialog.imageNetwork.requestCount, 1);
    dialog.setRecommendations({}, {}, {});
    QVERIFY(!dialog.findChild<QTextBrowser *>(QStringLiteral("browser_RadioConversation"))->toHtml().contains(QStringLiteral("tango-artwork:")));
}

void RadioDialogTest::recommendationsAndConversationAreBoundedPlainText()
{
    IsolatedRadioDialog dialog;
    QVector<RadioRecommendation> values;
    values.append(recommendation(0));
    for (int i = 0; i < 20; ++i) values.append(recommendation(i + 1));
    values[1].song.title = QString(2000, QLatin1Char('x'));
    dialog.setRecommendations(QString(1000, QLatin1Char('t')), QString(1000, QLatin1Char('n')), values);
    QCOMPARE(dialog.recommendations().size(), 12);
    auto *table = dialog.findChild<QTableWidget *>("table_RadioRecommendations");
    QCOMPARE(table->rowCount(), 12);
    QVERIFY(table->item(0, 0)->text().size() <= 300);
    auto *conversation = dialog.findChild<QPlainTextEdit *>("text_RadioConversation");
    dialog.appendMessage(QStringLiteral("你"), QStringLiteral("  "));
    QVERIFY(conversation->toPlainText().isEmpty());
    dialog.appendMessage(QStringLiteral("用户"), QStringLiteral("<img src='https://example.test/a'> & <script>文字</script>"));
    QVERIFY(conversation->toPlainText().contains(QStringLiteral("<img src=")));
    for (int i = 0; i < 24; ++i)
        dialog.appendMessage(QStringLiteral("AI"), QStringLiteral("message-%1 ").arg(i) + QString(5000, QLatin1Char('z')));
    QVERIFY(!conversation->toPlainText().contains(QStringLiteral("<img src=")));
    QVERIFY(conversation->toPlainText().contains(QStringLiteral("message-0 ")));
    QVERIFY(conversation->toPlainText().contains(QStringLiteral("message-23 ")));
    QVERIFY(conversation->toPlainText().size() < 39000);
    dialog.appendMessage(QStringLiteral("AI"), QStringLiteral("最新消息"));
    QVERIFY(!conversation->toPlainText().contains(QStringLiteral("message-0 ")));
    QVERIFY(conversation->toPlainText().contains(QStringLiteral("最新消息")));
    QVERIFY(conversation->isReadOnly());
    dialog.setRecommendations({}, {}, {});
    QVERIFY(!push(dialog, "button_RadioPlay")->isEnabled());
}

void RadioDialogTest::feedbackAndPreferenceSignalsAreExplicit()
{
    IsolatedRadioDialog dialog;
    dialog.setAvailability(true, true, {});
    QSignalSpy generated(&dialog, &RadioDialog::generateRequested);
    QSignalSpy remembered(&dialog, &RadioDialog::rememberRequested);
    QSignalSpy cleared(&dialog, &RadioDialog::clearPreferenceRequested);
    QSignalSpy avoided(&dialog, &RadioDialog::avoidArtistRequested);
    QVERIFY(!push(dialog, "button_RadioRemember")->isEnabled());
    auto *input = dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction");
    input->setPlainText(QStringLiteral("下班路上，想听轻快的中文歌"));
    push(dialog, "button_RadioGenerate")->click();
    QCOMPARE(generated.size(), 1);
    QVERIFY(generated.first().first().toString().contains(QStringLiteral("下班路上")));
    dialog.submissionAccepted(generated.first().first().toString(), dialog.currentSubmissionId());
    dialog.setRecommendations({}, {}, {recommendation()});
    QCOMPARE(remembered.size(), 0);
    input->setPlainText(QStringLiteral("更轻快一点"));
    push(dialog, "button_RadioGenerate")->click();
    QCOMPARE(generated.size(), 2);
    QCOMPARE(generated.last().first().toString(), QStringLiteral("更轻快一点"));
    dialog.submissionAccepted(generated.last().first().toString(), dialog.currentSubmissionId());
    push(dialog, "button_RadioRemember")->click();
    QCOMPARE(remembered.size(), 1);
    push(dialog, "button_RadioAvoidArtist")->click();
    QCOMPARE(avoided.size(), 1);
    QCOMPARE(avoided.first().first().toString(), recommendation().song.artist);
    dialog.setPreference(QStringLiteral("轻快的中文流行，少一点现场版"));
    QVERIFY(dialog.findChild<QLabel *>("label_RadioPreference")->text().contains(QStringLiteral("中文流行")));
    QCOMPARE(cleared.size(), 0);
    push(dialog, "button_RadioClearPreference")->click();
    QCOMPARE(cleared.size(), 1);
}

void RadioDialogTest::closingCancelsWorkWithoutStartingMusic()
{
    IsolatedRadioDialog dialog;
    dialog.setAvailability(true, true, {});
    dialog.setRecommendations({}, {}, {recommendation()});
    dialog.setPlaybackStatus(QStringLiteral("私人电台正在播放 · 关闭窗口继续收听"));
    dialog.setBusy(true);
    QSignalSpy canceled(&dialog, &RadioDialog::cancelRequested);
    QSignalSpy played(&dialog, &RadioDialog::playRequested);
    QSignalSpy activated(&dialog, &RadioDialog::trackActivated);
    dialog.close();
    QCOMPARE(canceled.size(), 1);
    QCOMPARE(played.size(), 0);
    QCOMPARE(activated.size(), 0);
    QCOMPARE(dialog.recommendations().size(), 1);
    QVERIFY(dialog.findChild<QLabel *>("label_RadioPlayback")->text().contains(QStringLiteral("继续收听")));
}

void RadioDialogTest::accountResetClearsAllPrivatePresentation()
{
    IsolatedRadioDialog dialog;
    dialog.setAvailability(true, true, {});
    dialog.appendMessage(QStringLiteral("你"), QStringLiteral("只属于账号一的偏好"));
    dialog.setRecommendations(QStringLiteral("账号一的电台"), {}, {recommendation()});
    dialog.setPreference(QStringLiteral("账号一的口味"));
    dialog.setPlaybackStatus(QStringLiteral("账号一正在播放"));
    dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction")->setPlainText(QStringLiteral("尚未发送的私密输入"));
    dialog.setBusy(true);
    QSignalSpy remembered(&dialog, &RadioDialog::rememberRequested);
    QSignalSpy played(&dialog, &RadioDialog::playRequested);
    dialog.resetForAccount();
    QVERIFY(dialog.recommendations().isEmpty());
    QVERIFY(dialog.findChild<QPlainTextEdit *>("text_RadioConversation")->toPlainText().isEmpty());
    QVERIFY(dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction")->toPlainText().isEmpty());
    QVERIFY(!dialog.findChild<QLabel *>("label_RadioPreference")->text().contains(QStringLiteral("账号一")));
    QVERIFY(dialog.findChild<QLabel *>("label_RadioPlayback")->text().isEmpty());
    QVERIFY(!push(dialog, "button_RadioGenerate")->isEnabled());
    QVERIFY(!push(dialog, "button_RadioPlay")->isEnabled());
    QCOMPARE(remembered.size(), 0);
    QCOMPARE(played.size(), 0);
    dialog.setAvailability(true, true, {});
    QVERIFY(dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction")->isEnabled());
    QVERIFY(!push(dialog, "button_RadioPlay")->isEnabled());
}

void RadioDialogTest::compactWindowFitsItsMinimumSize()
{
    IsolatedRadioDialog dialog;
    dialog.setAvailability(true, true, {});
    dialog.setAccountUsage(accountUsage());
    dialog.setPreference(QStringLiteral("轻快中文歌，少一些现场版"));
    dialog.setRecommendations(QStringLiteral("下班后的风"), QStringLiteral("给今天留一点轻松的节奏。"), {recommendation()});
    dialog.setPlaybackStatus(QStringLiteral("正在收听，关闭电台窗口后继续播放。"));
    dialog.setBusy(true, QStringLiteral("正在寻找音乐…"));
    dialog.setArtistChoices({{42, QStringLiteral("示例歌手"), {QStringLiteral("Singer")}, {}}});
    dialog.ensurePolished();
    dialog.layout()->activate();
    QVERIFY2(dialog.layout()->minimumSize().height() <= 620,
             qPrintable(QStringLiteral("Minimum layout height: %1").arg(dialog.layout()->minimumSize().height())));
    QVERIFY(dialog.layout()->minimumSize().width() <= 760);
}

void RadioDialogTest::assistantExamplesRouteThroughTheSameChat()
{
    IsolatedRadioDialog dialog;
    dialog.setAvailability(true, true, {});
    QVERIFY(dialog.windowTitle().contains(QStringLiteral("音乐助手")));
    QVERIFY(!dialog.findChild<QPushButton *>("button_RadioExample0"));
    QVERIFY(!dialog.findChild<QPushButton *>("button_RadioFeedback0"));
    auto *input = dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction");
    QSignalSpy generated(&dialog, &RadioDialog::generateRequested);
    const QStringList requests{QStringLiteral("我现在在喝酒，推荐一些适合的歌"),
                               QStringLiteral("邓紫棋最近有没有发新歌"),
                               QStringLiteral("记住我喜欢华语 R&B，少推荐摇滚")};
    for (int i = 0; i < requests.size(); ++i) {
        input->setPlainText(requests.at(i));
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(generated.size(), i + 1);
        QCOMPARE(generated.last().first().toString(), requests.at(i));
        dialog.submissionAccepted(requests.at(i), dialog.currentSubmissionId());
    }
}

void RadioDialogTest::artistChoicesAreBoundedAndRequireExplicitSelection()
{
    IsolatedRadioDialog dialog;
    dialog.setAvailability(true, true, {});
    QSignalSpy selected(&dialog, &RadioDialog::artistSelected);
    QVector<AssistantArtist> choices{{0, QStringLiteral("invalid"), {}, {}},
                                     {1, QStringLiteral("<b>同名歌手</b>"), {QStringLiteral("Singer")}, {}}};
    choices.append(choices.last());
    for (int id = 2; id <= 10; ++id)
        choices.append({id, QStringLiteral("歌手 %1").arg(id), {}, {}});
    dialog.setArtistChoices(choices);
    auto *combo = dialog.findChild<QComboBox *>("combo_RadioArtistChoices");
    QCOMPARE(combo->count(), 5);
    QVERIFY(combo->itemText(0).contains(QStringLiteral("<b>同名歌手</b>")));
    QCOMPARE(selected.size(), 0);
    combo->setCurrentIndex(2);
    QCOMPARE(selected.size(), 0);
    push(dialog, "button_RadioLookupArtist")->click();
    QCOMPARE(selected.size(), 1);
    QCOMPARE(qvariant_cast<AssistantArtist>(selected.first().first()).id, qint64(3));
    dialog.setBusy(true);
    QVERIFY(!combo->isEnabled());
    push(dialog, "button_RadioLookupArtist")->click();
    QCOMPARE(selected.size(), 1);
    dialog.setBusy(false);
    dialog.clearArtistChoices();
    QCOMPARE(combo->count(), 0);
    QVERIFY(!push(dialog, "button_RadioLookupArtist")->isEnabled());
    dialog.setArtistChoices(choices);
    dialog.resetForAccount();
    QCOMPARE(combo->count(), 0);
}

void RadioDialogTest::preferenceReviewIsEditableBoundedAndExplicit()
{
    IsolatedRadioDialog dialog;
    // Editing local taste does not depend on the model being online.
    dialog.setAvailability(true, false, {});
    QSignalSpy saved(&dialog, &RadioDialog::preferenceEdited);
    QSignalSpy cleared(&dialog, &RadioDialog::clearPreferenceRequested);
    dialog.setPreference(QStringLiteral("喜欢轻快中文歌"));
    QVERIFY(push(dialog, "button_RadioEditPreference")->isEnabled());
    QVERIFY(push(dialog, "button_RadioClearPreference")->isEnabled());
    push(dialog, "button_RadioClearPreference")->click();
    QCOMPARE(cleared.size(), 1);
    QTimer::singleShot(0, &dialog, [&dialog] {
        auto *editor = dialog.findChild<QInputDialog *>("dialog_RadioPreferenceEditor");
        Q_ASSERT(editor);
        auto *field = editor->findChild<QLineEdit *>("edit_RadioPreference");
        Q_ASSERT(field);
        QCOMPARE(field->maxLength(), 500);
        QCOMPARE(field->text(), QStringLiteral("喜欢轻快中文歌"));
        field->setText(QString(700, QChar(0x97F3)));
        QCOMPARE(field->text().size(), 500);
        editor->accept();
    });
    push(dialog, "button_RadioEditPreference")->click();
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.first().first().toString().size(), 500);
    QTimer::singleShot(0, &dialog, [&dialog] {
        auto *editor = dialog.findChild<QInputDialog *>("dialog_RadioPreferenceEditor");
        Q_ASSERT(editor);
        QCOMPARE(editor->textValue(), QStringLiteral("华语 R&B，少推荐摇滚"));
        editor->reject();
    });
    dialog.reviewPreference(QStringLiteral("华语 R&B，少推荐摇滚"));
    QCOMPARE(saved.size(), 1);
    QTimer::singleShot(0, &dialog, [&dialog] {
        auto *editor = dialog.findChild<QInputDialog *>("dialog_RadioPreferenceEditor");
        Q_ASSERT(editor);
        QVERIFY(editor->labelText().contains(QStringLiteral("留空并保存可清除")));
        editor->accept();
    });
    dialog.reviewPreference({});
    QCOMPARE(saved.size(), 2);
    QVERIFY(saved.last().first().toString().isEmpty());
    dialog.setBusy(true);
    dialog.reviewPreference(QStringLiteral("不会弹出"));
    QCOMPARE(saved.size(), 2);
}

void RadioDialogTest::stalePreferenceEditorCannotSaveIntoAnotherAccount()
{
    IsolatedRadioDialog dialog;
    dialog.setAvailability(true, true, {});
    QSignalSpy saved(&dialog, &RadioDialog::preferenceEdited);
    QTimer::singleShot(0, &dialog, [&dialog] {
        auto *editor = dialog.findChild<QInputDialog *>("dialog_RadioPreferenceEditor");
        Q_ASSERT(editor);
        dialog.resetForAccount();
        dialog.setAvailability(true, true, {});
        editor->accept();
    });
    dialog.reviewPreference(QStringLiteral("旧账号的口味"));
    QCOMPARE(saved.size(), 0);
}

void RadioDialogTest::conversationModeShowsOnlyChatAndComposer()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    dialog.setAccountUsage(accountUsage());
    dialog.setPreference(QStringLiteral("偏好粤语"));
    dialog.setRecommendations(QStringLiteral("当前歌曲"), {}, {recommendation()});
    dialog.setArtistChoices({{7763, QStringLiteral("G.E.M.邓紫棋"), {}, {}}});
    dialog.setPreferenceProposal(QStringLiteral("turn:memory"), QStringLiteral("set"), QStringLiteral("喜欢粤语"));
    dialog.setBusy(true, QStringLiteral("正在查找音乐"));
    dialog.show();
    QCoreApplication::processEvents();
    auto *input = dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction");
    auto *transcript = dialog.findChild<QPlainTextEdit *>("text_RadioConversation");
    auto *conversation = dialog.findChild<QTextBrowser *>("browser_RadioConversation");
    QVERIFY(input->isVisible());
    QVERIFY(transcript->isHidden());
    QVERIFY(conversation && conversation->isVisible());
    QVERIFY(conversation->height() > 300);
    for (auto *button : dialog.findChildren<QPushButton *>())
        QCOMPARE(button->isVisible(), button->objectName() == QStringLiteral("button_RadioGenerate") ||
            button->objectName() == QStringLiteral("button_ConversationClearAll"));
    for (const char *name : {"scroll_RadioPage", "label_RadioHeading", "label_RadioDescription",
             "label_RadioAvailability", "label_RadioQuota", "panel_MusicTask", "panel_RadioArtistChoices",
             "panel_RadioPreview", "panel_RadioPreferenceProposal", "label_RadioInputCounter"}) {
        auto *widget = dialog.findChild<QWidget *>(name);
        QVERIFY(widget);
        QVERIFY2(!widget->isVisible(), name);
    }
    QVERIFY(transcript->toPlainText().contains(QStringLiteral("确认保存口味")));
    QVERIFY(transcript->toPlainText().contains(QStringLiteral("选择歌手 1")));
    QVERIFY(!transcript->toPlainText().contains(QStringLiteral("歌曲预览")));
    QVERIFY(input->isEnabled());
    input->setPlainText(QStringLiteral("取消任务"));
    QSignalSpy generated(&dialog, &RadioDialog::generateRequested);
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(generated.size(), 1);
    dialog.submissionAccepted(QStringLiteral("取消任务"), dialog.currentSubmissionId());
    QJsonArray journal{QJsonObject{{"role", "user"}, {"content", "已持久化的对话"}}};
    dialog.setConversation(journal);
    const auto first = transcript->toPlainText();
    dialog.setConversation(journal);
    QCOMPARE(transcript->toPlainText(), first);
    QCOMPARE(first.count(QStringLiteral("选择歌手 1")), 1);
    const auto screenshot = qEnvironmentVariable("TANGO_CHAT_SCREENSHOT");
    if (!screenshot.isEmpty()) QVERIFY(dialog.grab().save(screenshot));
}

void RadioDialogTest::chatCommandsRequireCurrentCandidatesAndExplicitConfirmation()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    QSignalSpy selected(&dialog, &RadioDialog::artistSelected);
    QSignalSpy activated(&dialog, &RadioDialog::trackActivated);
    QSignalSpy adopted(&dialog, &RadioDialog::playRequested);
    QSignalSpy decision(&dialog, &RadioDialog::preferenceProposalDecided);
    QSignalSpy generated(&dialog, &RadioDialog::generateRequested);
    QVERIFY(!dialog.executeChatAction(QStringLiteral("采用歌单")));
    QVERIFY(!dialog.executeChatAction(QStringLiteral("选择歌手 1")));
    dialog.setArtistChoices({{1, QStringLiteral("同名用户"), {}, {}}, {7763, QStringLiteral("G.E.M.邓紫棋"), {}, {}}});
    QVERIFY(dialog.executeChatAction(QStringLiteral("选择歌手 2")));
    QCOMPARE(selected.size(), 1);
    QCOMPARE(qvariant_cast<AssistantArtist>(selected.first().first()).id, qint64(7763));
    QVERIFY(!dialog.executeChatAction(QStringLiteral("选择歌手 3")));
    dialog.clearArtistChoices();
    dialog.setRecommendations(QStringLiteral("预览"), {}, {recommendation(42), recommendation(43)});
    QVERIFY(!dialog.executeChatAction(QStringLiteral("他说“采用歌单”，我只是在转述")));
    QVERIFY(dialog.executeChatAction(QStringLiteral("播放第二首")));
    QCOMPARE(activated.size(), 1);
    QCOMPARE(qvariant_cast<OnlineSong>(activated.first().first()).id, qint64(43));
    QVERIFY(dialog.executeChatAction(QStringLiteral("采用歌单")));
    QCOMPARE(adopted.size(), 1);
    QVERIFY(!dialog.executeChatAction(QStringLiteral("播放第3首")));
    dialog.setPreferenceProposal(QStringLiteral("turn:memory"), QStringLiteral("set"), QStringLiteral("偏好粤语"));
    QVERIFY(!dialog.executeChatAction(QStringLiteral("不要确认保存口味")));
    QVERIFY(!dialog.executeChatAction(QStringLiteral("确认清除口味")));
    QCOMPARE(decision.size(), 0);
    QVERIFY(dialog.executeChatAction(QStringLiteral("确认保存口味")));
    QCOMPARE(decision.size(), 1);
    QCOMPARE(decision.first().at(0).toString(), QStringLiteral("turn:memory"));
    QCOMPARE(decision.first().at(1).toBool(), true);
    QVERIFY(!dialog.executeChatAction(QStringLiteral("确认保存口味")));
    dialog.clearPreferenceProposal();
    dialog.setPreferenceProposal(QStringLiteral("turn:clear"), QStringLiteral("clear"), {});
    QVERIFY(dialog.executeChatAction(QStringLiteral("取消口味")));
    QCOMPARE(decision.size(), 2);
    QCOMPARE(decision.last().at(1).toBool(), false);
    QCOMPARE(generated.size(), 0);
    dialog.resetForAccount();
    dialog.setAvailability(true, true, {});
    QVERIFY(!dialog.executeChatAction(QStringLiteral("采用歌单")));
    QVERIFY(!dialog.executeChatAction(QStringLiteral("确认保存口味")));
    QVERIFY(!dialog.executeChatAction(QStringLiteral("选择歌手 2")));
}

void RadioDialogTest::clarificationCannotOfferPreviousPreviewAsCurrentSuccess()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    const QJsonObject oldPreview{{"version", 1}, {"title", "上一版旧歌单"}, {"tracks", QJsonArray{
        QJsonObject{{"id", 42}, {"title", "旧歌曲"}, {"artist", "旧歌手"}}}}};
    dialog.setTaskSnapshot(QJsonObject{{"status", "needs_clarification"}, {"preview", oldPreview},
        {"providedArtists", QJsonArray{QJsonObject{{"id", 7763}, {"name", "G.E.M.邓紫棋"}}}}});
    const auto text = dialog.findChild<QPlainTextEdit *>("text_RadioConversation")->toPlainText();
    QVERIFY(text.contains(QStringLiteral("请确认歌手")));
    QVERIFY(text.contains(QStringLiteral("上一版本")));
    QVERIFY(!text.contains(QStringLiteral("歌曲预览")));
    QVERIFY(!dialog.executeChatAction(QStringLiteral("采用歌单")));
    QVERIFY(!dialog.executeChatAction(QStringLiteral("播放第1首")));
}

void RadioDialogTest::chatPreviewShowsCurrentLocksAndClearsStaleAccountState()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    auto *transcript = dialog.findChild<QPlainTextEdit *>("text_RadioConversation");
    QVERIFY(transcript);
    QJsonArray songs;
    for (int i = 0; i < 4; ++i) songs.append(QJsonObject{{"id", 42 + i},
        {"title", QStringLiteral("歌曲%1").arg(i + 1)}, {"artist", "示例歌手"}});
    const QJsonObject oldLockCheck{{"key", "locks"}, {"status", "satisfied"}, {"detail", "保留 0 首锁定歌曲"}};
    const QJsonObject initialPreview{{"title", "已保存歌单"}, {"tracks", songs},
        {"checks", QJsonArray{oldLockCheck}}};
    QJsonObject snapshot{{"status", "preview"}, {"preview", initialPreview}};
    dialog.setTaskSnapshot(snapshot);
    QVERIFY(transcript->toPlainText().contains(QStringLiteral("当前未锁定歌曲")));
    snapshot.insert("lockedIds", QJsonArray{42, 43, 44});
    dialog.setTaskSnapshot(snapshot);
    QString text = transcript->toPlainText();
    QVERIFY(text.contains(QStringLiteral("已锁定 3 首歌曲")));
    QVERIFY(!text.contains(QStringLiteral("保留 0 首")));
    QCOMPARE(text.count(QStringLiteral(" · 已锁定")), 3);
    QVERIFY(text.contains(QStringLiteral("1. 歌曲1 · 示例歌手 · 已锁定")));
    QVERIFY(!text.contains(QStringLiteral("4. 歌曲4 · 示例歌手 · 已锁定")));
    snapshot.insert("status", "paused");
    dialog.setTaskSnapshot(snapshot);
    text = transcript->toPlainText();
    QVERIFY(text.contains(QStringLiteral("当前锁定 3 首歌曲，后续调整须保留")));
    QVERIFY(!text.contains(QStringLiteral("保留 0 首")));
    QVERIFY(!text.contains(QStringLiteral("✓ 当前锁定")));
    // 只替换旧成功数；未满足检查仍应保留，不能用当前配置掩盖历史失败。
    auto unmetPreview = initialPreview;
    unmetPreview.insert("checks", QJsonArray{QJsonObject{{"key", "locks"}, {"status", "unmet"},
        {"detail", "缺少锁定歌曲42"}}});
    auto unmetSnapshot = snapshot;
    unmetSnapshot.insert("preview", unmetPreview);
    dialog.setTaskSnapshot(unmetSnapshot);
    QVERIFY(transcript->toPlainText().contains(QStringLiteral("未满足 · 缺少锁定歌曲42")));
    snapshot.insert("status", "preview");
    dialog.setTaskSnapshot(snapshot);
    // 新版预览应保留最新快照的锁定标识，不能依靠上一次的展示文本。
    auto preview = snapshot.value("preview").toObject();
    preview.insert("title", "调整后的歌单");
    snapshot.insert("preview", preview);
    dialog.setTaskSnapshot(snapshot);
    QVERIFY(transcript->toPlainText().contains(QStringLiteral("已锁定 3 首歌曲")));
    snapshot.insert("lockedIds", QJsonArray{});
    dialog.setTaskSnapshot(snapshot);
    QVERIFY(!transcript->toPlainText().contains(QStringLiteral(" · 已锁定")));
    QVERIFY(transcript->toPlainText().contains(QStringLiteral("当前未锁定歌曲")));
    snapshot.insert("lockedIds", QJsonArray{42});
    dialog.setTaskSnapshot(snapshot);
    dialog.setTaskMode(false);
    dialog.setRecommendations("单曲查找", {}, {recommendation(42)});
    QVERIFY(!transcript->toPlainText().contains(QStringLiteral("锁定")));
    dialog.setTaskSnapshot(snapshot);
    dialog.resetForAccount();
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    dialog.setRecommendations("另一个账号的搜索", {}, {recommendation(42)});
    QVERIFY(!transcript->toPlainText().contains(QStringLiteral("锁定")));
}

void RadioDialogTest::chatPreviewShowsSceneEvidenceGapInVisibleConversation()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    dialog.resize(900, 740);
    dialog.show();
    QJsonArray checks;
    for (int index = 0; index < 5; ++index)
        checks.append(QJsonObject{{"key", QStringLiteral("verified_%1").arg(index)},
            {"status", "satisfied"}, {"detail", QStringLiteral("已完成核对%1").arg(index)}});
    const QString detail = QStringLiteral("8首中0首命中「散步」来源名称或标签；其余仅核验歌手与时长，未分析音频。");
    checks.append(QJsonObject{{"key", "scene"}, {"status", "unverified"}, {"detail", detail}});
    dialog.setTaskSnapshot(scenePreviewSnapshot(checks));
    auto *browser = dialog.findChild<QTextBrowser *>("browser_RadioConversation");
    QVERIFY(browser && browser->isVisible());
    QVERIFY(dialog.findChild<QWidget *>("panel_MusicTask")->isHidden());
    const auto visible = browser->toPlainText();
    // 必须进入用户实际使用的聊天文档，不能只存在于隐藏面板、标题或泛化说明中。
    QVERIFY(visible.contains(detail));
    QVERIFY(visible.contains(QStringLiteral("待核验")) || visible.contains(QStringLiteral("未核验")) ||
        visible.contains(QStringLiteral("证据不足")));
    QVERIFY(visible.contains(QStringLiteral("邓紫棋 散步歌单")));
    const auto screenshot = qEnvironmentVariable("TANGO_WARNING_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        const auto cursor = browser->document()->find(detail);
        QVERIFY(!cursor.isNull());
        browser->setTextCursor(cursor);
        browser->ensureCursorVisible();
        QVERIFY(dialog.grab().save(screenshot));
    }
    QCOMPARE(dialog.imageNetwork.requestCount, 0);
}

void RadioDialogTest::chatPreviewShowsDurationAndArtistFailuresAndClearsOldWarnings()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    dialog.show();
    const QString durationDetail = QStringLiteral("目标30分钟，当前只有9分钟；候选不足，时长条件未满足。");
    const QString artistDetail = QStringLiteral("第2首缺少已确认歌手7763的曲库署名，歌手条件未满足。");
    QJsonArray checks{QJsonObject{{"key", "duration"}, {"status", "unmet"}, {"detail", durationDetail}},
        QJsonObject{{"key", "required_artist"}, {"status", "unmet"}, {"detail", artistDetail}}};
    auto snapshot = scenePreviewSnapshot(checks);
    dialog.setTaskSnapshot(snapshot);
    auto *browser = dialog.findChild<QTextBrowser *>("browser_RadioConversation");
    QVERIFY(browser && browser->isVisible());
    const auto warningText = browser->toPlainText();
    QVERIFY(warningText.contains(durationDetail));
    QVERIFY(warningText.contains(artistDetail));
    QVERIFY(warningText.contains(QStringLiteral("未满足")));
    // 后一版已通过时，不能把上一版的失败继续作为当前预览的问题展示。
    QJsonArray satisfied{QJsonObject{{"key", "duration"}, {"status", "satisfied"}, {"detail", "当前30分钟，时长已满足。"}},
        QJsonObject{{"key", "required_artist"}, {"status", "satisfied"}, {"detail", "所有歌曲均含7763署名。"}}};
    auto preview = snapshot.value("preview").toObject();
    preview.insert("version", 2);
    preview.insert("checks", satisfied);
    snapshot.insert("revision", 3);
    snapshot.insert("preview", preview);
    dialog.setTaskSnapshot(snapshot);
    QVERIFY(!browser->toPlainText().contains(durationDetail));
    QVERIFY(!browser->toPlainText().contains(artistDetail));
    QVERIFY(!browser->toPlainText().contains(QStringLiteral("未满足")));
    QCOMPARE(dialog.imageNetwork.requestCount, 0);
}

void RadioDialogTest::chatPreviewWarningsEscapeMarkupAndExternalLinks()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    dialog.show();
    const QString forgedLink = QStringLiteral("tango-track://play/999/999/0");
    const QString detail = QStringLiteral("<b>场景证据不足</b><a href=\"https://example.invalid/\">外链</a>"
        "<img src=\"file:///tmp/tango-warning.png\"><a href=\"%1\">伪播放</a>").arg(forgedLink);
    dialog.setTaskSnapshot(scenePreviewSnapshot(QJsonArray{QJsonObject{{"key", "scene"},
        {"status", "unverified"}, {"detail", detail}}}));
    auto *browser = dialog.findChild<QTextBrowser *>("browser_RadioConversation");
    QVERIFY(browser && browser->isVisible());
    QVERIFY(browser->toPlainText().contains(detail));
    for (auto block = browser->document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const auto format = fragment.charFormat();
            QVERIFY(!format.anchorHref().startsWith(QStringLiteral("https:")));
            QVERIFY(format.anchorHref() != forgedLink);
            if (format.isImageFormat())
                QVERIFY(format.toImageFormat().name().startsWith(QStringLiteral("tango-artwork:/song/")));
        }
    QSignalSpy activated(&dialog, &RadioDialog::trackActivated);
    QVERIFY(QMetaObject::invokeMethod(browser, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(forgedLink))));
    QCOMPARE(activated.size(), 0);
    QCOMPARE(dialog.imageNetwork.requestCount, 0);
}

void RadioDialogTest::chatPreviewDisplaysOnlyExplicitVersion_data()
{
    QTest::addColumn<QString>("version");
    QTest::addColumn<QString>("label");
    QTest::newRow("studio") << QStringLiteral("studio") << QStringLiteral("录音室版");
    QTest::newRow("live") << QStringLiteral("live") << QStringLiteral("现场版");
    QTest::newRow("missing") << QString() << QString();
    QTest::newRow("unknown") << QStringLiteral("unverified") << QString();
}

void RadioDialogTest::chatPreviewDisplaysOnlyExplicitVersion()
{
    QFETCH(QString, version);
    QFETCH(QString, label);
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    auto row = recommendation(42);
    row.song.title = QStringLiteral("合成作品");
    row.song.album = QStringLiteral("合成作品集");
    row.song.version = version;
    QSignalSpy played(&dialog, &RadioDialog::playRequested);
    dialog.setRecommendations(QStringLiteral("单曲查找"), {}, {row});
    const auto text = dialog.findChild<QPlainTextEdit *>("text_RadioConversation")->toPlainText();
    if (label.isEmpty()) {
        QVERIFY(!text.contains(QStringLiteral("录音室版")));
        QVERIFY(!text.contains(QStringLiteral("现场版")));
    } else QVERIFY(text.contains(label));
    QCOMPARE(played.count(), 0);
}

void RadioDialogTest::clearHistoryIsExplicitAndDisablesLocalActionsDuringDelete()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    dialog.appendMessage(QStringLiteral("你"), QStringLiteral("原会话暂留，后台确认删除后才清屏"));
    dialog.setRecommendations(QStringLiteral("预览"), {}, {recommendation(42)});
    QSignalSpy cleared(&dialog, &RadioDialog::clearConversationsRequested);
    QSignalSpy played(&dialog, &RadioDialog::trackActivated);
    QSignalSpy generated(&dialog, &RadioDialog::generateRequested);
    auto *clear = push(dialog, "button_ConversationClearAll");
    dialog.setBusy(true, QStringLiteral("查找尚未返回"));
    QVERIFY(clear->isEnabled());
    clear->click();
    QCOMPARE(cleared.size(), 1);
    QVERIFY(dialog.findChild<QPlainTextEdit *>("text_RadioConversation")->toPlainText().contains(QStringLiteral("原会话暂留")));
    dialog.setConversationsClearing(true);
    clear->click();
    QCOMPARE(cleared.size(), 1);
    QVERIFY(!clear->isEnabled());
    QVERIFY(clear->text().contains(QStringLiteral("正在清除")));
    auto *input = dialog.findChild<QPlainTextEdit *>("edit_RadioInstruction");
    QVERIFY(!input->isEnabled());
    QVERIFY(!dialog.executeChatAction(QStringLiteral("播放第1首")));
    QVERIFY(firstTrackLink(dialog).isEmpty());
    QCOMPARE(played.size(), 0);
    QCOMPARE(generated.size(), 0);
    dialog.setBusy(false);
    dialog.setConversationsClearing(false);
    QVERIFY(clear->isEnabled());
    QVERIFY(input->isEnabled());
    dialog.setAvailability(false, false, {});
    clear->click();
    QCOMPARE(cleared.size(), 1);
}

void RadioDialogTest::staleSongAnchorsDoNotPlayAcrossSnapshotsOrAccounts()
{
    IsolatedRadioDialog dialog;
    dialog.setConversationMode(true);
    dialog.setAvailability(true, true, {});
    auto *browser = dialog.findChild<QTextBrowser *>("browser_RadioConversation");
    QSignalSpy played(&dialog, &RadioDialog::trackActivated);
    dialog.setRecommendations(QStringLiteral("第一组"), {}, {recommendation(42)});
    const QString first = firstTrackLink(dialog);
    QVERIFY(!first.isEmpty());
    dialog.setRecommendations(QStringLiteral("第二组"), {}, {recommendation(43)});
    const QString second = firstTrackLink(dialog);
    QVERIFY(!second.isEmpty());
    QVERIFY(first != second);
    // 模拟排队的旧点击事件，不能因列表位置仍是第一首就播放新曲或旧曲。
    QVERIFY(QMetaObject::invokeMethod(browser, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(first))));
    QCOMPARE(played.size(), 0);
    QVERIFY(QMetaObject::invokeMethod(browser, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(second))));
    QCOMPARE(played.size(), 1);
    QCOMPARE(qvariant_cast<OnlineSong>(played.first().first()).id, qint64(43));
    dialog.setArtistChoices({{999, QStringLiteral("尚未确认的歌手"), {}, {}}});
    QVERIFY(QMetaObject::invokeMethod(browser, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(second))));
    QCOMPARE(played.size(), 1);
    dialog.resetForAccount();
    dialog.setAvailability(true, true, {});
    dialog.setRecommendations(QStringLiteral("新账号"), {}, {recommendation(43)});
    QVERIFY(QMetaObject::invokeMethod(browser, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(second))));
    QCOMPARE(played.size(), 1);
    QVERIFY(QMetaObject::invokeMethod(browser, "anchorClicked", Qt::DirectConnection,
        Q_ARG(QUrl, QUrl(QStringLiteral("https://example.invalid/fake-play")))));
    QCOMPARE(played.size(), 1);
}

QTEST_MAIN(RadioDialogTest)
#include "radiodialog_test.moc"
