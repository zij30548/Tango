#ifndef LOOKUPCONTEXT_H
#define LOOKUPCONTEXT_H

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <cmath>

// 查找续接只表达当前批候选的只读选择，不能携带播放或偏好写入权限。
namespace LookupContext {
inline bool validId(const QString &value) {
    return QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{8,128}$")).match(value).hasMatch();
}
inline bool text(const QString &value, int maximum, bool empty = false) {
    if ((!empty && value.trimmed().isEmpty()) || value.size() > maximum ||
        QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")).match(value).hasMatch()) return false;
    for (int i = 0; i < value.size(); ++i) {
        if (value.at(i).isHighSurrogate()) {
            if (++i >= value.size() || !value.at(i).isLowSurrogate()) return false;
        } else if (value.at(i).isLowSurrogate()) return false;
    }
    return true;
}
inline qint64 integer(const QJsonValue &value, qint64 maximum = 9007199254740991LL) {
    const auto number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= 1 && number <= double(maximum) &&
        std::floor(number) == number ? qint64(number) : -1;
}
inline bool valid(const QJsonObject &context) {
    if (QJsonDocument(context).toJson(QJsonDocument::Compact).size() > 12 * 1024 || context.size() != 5 || !validId(context.value("candidateSetId").toString()) ||
        !validId(context.value("originMessageId").toString()) || context.value("originMessageId").toString().size() > 100 || !validId(context.value("originActionId").toString()) ||
        !context.value("rows").isArray() || !context.value("visibleIds").isArray()) return false;
    const auto rows = context.value("rows").toArray(), visible = context.value("visibleIds").toArray();
    if (rows.isEmpty() || rows.size() > 20 || visible.size() > 12) return false;
    QSet<qint64> ids, shown;
    for (const auto &value : rows) {
        const auto row = value.toObject(); const auto id = integer(row.value("id"));
        const auto duration = row.value("durationMs").toDouble(-1);
        if (!value.isObject() || row.size() != 6 || id < 1 || ids.contains(id) ||
            !row.value("title").isString() || !text(row.value("title").toString(), 200) ||
            !row.value("artist").isString() || !text(row.value("artist").toString(), 200, true) ||
            !row.value("album").isString() || !text(row.value("album").toString(), 200, true) ||
            !QStringList{"studio", "live", "unknown"}.contains(row.value("version").toString()) ||
            !row.value("durationMs").isDouble() || !std::isfinite(duration) || duration < 0 || duration > 86400000 || std::floor(duration) != duration) return false;
        ids.insert(id);
    }
    for (const auto &value : visible) {
        const auto id = integer(value);
        if (id < 1 || !ids.contains(id) || shown.contains(id)) return false;
        shown.insert(id);
    }
    return true;
}
inline int ordinal(QString value) {
    bool ok = false; const int number = value.toInt(&ok);
    if (ok) return number >= 1 && number <= 20 ? number : -1;
    const QString digits = QStringLiteral("一二三四五六七八九");
    if (value == QStringLiteral("十")) return 10;
    if (value == QStringLiteral("两")) return 2;
    if (value == QStringLiteral("二十")) return 20;
    if (value.size() == 1 && digits.contains(value)) return digits.indexOf(value) + 1;
    if (value.size() == 2 && value.startsWith(QStringLiteral("十")) && digits.contains(value.right(1))) return 11 + digits.indexOf(value.right(1));
    return -1;
}
inline QJsonObject requestedSelection(QString instruction, const QJsonObject &context = {}) {
    instruction = instruction.normalized(QString::NormalizationForm_KC);
    instruction.remove(QRegularExpression(QStringLiteral("\\s+")));
    const auto normalizedOriginal = instruction;
    instruction.remove(QRegularExpression(QStringLiteral("[。！!？?]+$")));
    if (instruction == QStringLiteral("不是这个歌手,换另一个歌手") || instruction == QStringLiteral("不是这个歌手,换成另一个歌手"))
        return {{"operation", "other_artist"}};
    const auto clauses = instruction.split(QRegularExpression(QStringLiteral("[，,；;。\\n]+")), Qt::SkipEmptyParts);
    if (clauses.isEmpty() || clauses.size() > 3) return {};
    for (const auto &tail : clauses.mid(1))
        if (!QRegularExpression(QStringLiteral("^(?:(?:先|只|暂时)?(?:别|不要|不用|无需)(?:自动)?(?:播放|放|播)(?:了)?|先让我看看|只(?:给我|看|查看)(?:资料|信息))$")).match(tail).hasMatch()) return {};
    const auto text = clauses.first();
    if (QRegularExpression(QStringLiteral("他说|她说|别人说|朋友说|如果|假如|假设|除非|引用|转述|女声|男声|[‘’“”\"'`「」『』]")).match(text).hasMatch()) return {};
    const auto prefix = QStringLiteral("(?:(?:请你|请|帮我|给我|我想要|我想看|我要|我选|要|选|选择|看看|查看|只要|换成|还是)){0,3}");
    const auto suffix = QStringLiteral("(?:的)?(?:那个|那首|这首|这个)?");
    auto match = QRegularExpression(QStringLiteral("^") + prefix + QStringLiteral("第([0-9]{1,2}|[一二两三四五六七八九十]{1,2})(?:首|个)") + suffix + QStringLiteral("$")).match(text);
    if (match.hasMatch()) {
        const auto index = ordinal(match.captured(1));
        return index > 0 ? QJsonObject{{"operation", "select"}, {"index", index}} : QJsonObject{};
    }
    match = QRegularExpression(QStringLiteral("^") + prefix + QStringLiteral("(录音室版|录音室版本|studio版|现场版|现场版本|live版)") + suffix + QStringLiteral("$"), QRegularExpression::CaseInsensitiveOption).match(text);
    if (match.hasMatch()) return {{"operation", "version"}, {"version", match.captured(1).contains(QStringLiteral("现场")) || match.captured(1).startsWith("live", Qt::CaseInsensitive) ? "live" : "studio"}};
    match = QRegularExpression(QStringLiteral("^(?:请|帮我)?(?:不要|别要|排除|去掉)(录音室版|录音室版本|studio版|现场版|现场版本|live版)") + suffix + QStringLiteral("$"), QRegularExpression::CaseInsensitiveOption).match(text);
    if (match.hasMatch()) return {{"operation", "exclude_version"}, {"version", match.captured(1).contains(QStringLiteral("现场")) || match.captured(1).startsWith("live", Qt::CaseInsensitive) ? "live" : "studio"}};
    if (QRegularExpression(QStringLiteral("^") + prefix + QStringLiteral("(?:另一个|其他)歌手(?:唱的)?") + suffix + QStringLiteral("$")).match(text).hasMatch()) return {{"operation", "other_artist"}};
    // 歌手选择只能引用本批候选中完整、精确的姓名，不能由模型凭空补身份。
    for (const auto &value : context.value("rows").toArray()) {
        const auto artist = value.toObject().value("artist").toString();
        if (artist.trimmed().isEmpty()) continue;
        auto name = artist.normalized(QString::NormalizationForm_KC); name.remove(QRegularExpression(QStringLiteral("\\s+")));
        // 澄清后可只答完整歌手名；保留整句比较，附句和标点不能被截掉冒充裸姓名。
        if (normalizedOriginal == name) return {{"operation", "artist"}, {"artist", artist}};
        const auto pattern = QStringLiteral("^") + prefix + QRegularExpression::escape(name) + QStringLiteral("(?:唱的|的)") + suffix + QStringLiteral("$");
        if (QRegularExpression(pattern).match(text).hasMatch() ||
            QRegularExpression(QStringLiteral("^我说的是") + QRegularExpression::escape(name) + QStringLiteral("(?:唱的)?") + suffix + QStringLiteral("$")).match(text).hasMatch())
            return {{"operation", "artist"}, {"artist", artist}};
    }
    return {};
}
inline bool related(QString instruction) {
    instruction = instruction.normalized(QString::NormalizationForm_KC); instruction.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (QRegularExpression(QStringLiteral("推荐|歌单|搜索|查找|查询|找一下|新歌|新作|新专辑|偏好|喜好|口味|记住|保存")).match(instruction).hasMatch()) return false;
    return QRegularExpression(QStringLiteral("第[0-9一二两三四五六七八九十零百]+(?:首|个)|(?:录音室|现场|studio|live)版|女声|男声|另一个歌手|我说的是.+(?:那个|那首)"),
        QRegularExpression::CaseInsensitiveOption).match(instruction).hasMatch();
}
inline bool validParameters(const QJsonObject &parameters, const QString &instruction, const QJsonObject &context = {}) {
    if (!validId(parameters.value("candidateSetId").toString()) || !validId(parameters.value("originMessageId").toString()) || parameters.value("originMessageId").toString().size() > 100 ||
        !validId(parameters.value("originActionId").toString())) return false;
    auto selection = parameters;
    selection.remove("candidateSetId"); selection.remove("originMessageId"); selection.remove("originActionId");
    // 即使后台误返回了play:false等额外字段也拒绝，避免读契约逐步扩成写权限。
    const auto expected = requestedSelection(instruction, context);
    return !expected.isEmpty() && selection == expected;
}
inline QJsonArray filteredIds(const QJsonObject &context, const QJsonObject &parameters, bool *known = nullptr) {
    if (known) *known = false;
    if (!valid(context)) return {};
    const auto operation = parameters.value("operation").toString();
    const auto rows = context.value("rows").toArray(), visible = context.value("visibleIds").toArray();
    if (operation == "select") {
        const auto index = integer(parameters.value("index"), 20);
        if (index < 1 || index > visible.size()) return {};
        if (known) *known = true;
        return {visible.at(int(index - 1))};
    }
    QString excludedArtist;
    if (operation == "other_artist") {
        QSet<QString> artists;
        for (const auto &rowValue : rows) {
            const auto row = rowValue.toObject();
            if (visible.contains(row.value("id"))) artists.insert(row.value("artist").toString());
        }
        if (artists.size() != 1 || artists.contains(QString())) return {};
        excludedArtist = *artists.cbegin();
    }
    QJsonArray result;
    for (const auto &rowValue : rows) {
        const auto row = rowValue.toObject(); const auto version = row.value("version").toString();
        const bool matches = operation == "version" ? version == parameters.value("version").toString()
            : operation == "exclude_version" ? version != "unknown" && version != parameters.value("version").toString()
            : operation == "artist" ? row.value("artist").toString() == parameters.value("artist").toString()
            : operation == "other_artist" ? !row.value("artist").toString().trimmed().isEmpty() && row.value("artist").toString() != excludedArtist : false;
        if (matches && result.size() < 12) result.append(row.value("id"));
    }
    if (known) *known = QStringList{"version", "exclude_version", "artist", "other_artist"}.contains(operation);
    return result;
}
}
#endif
