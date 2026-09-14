#include "logparser.h"

#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTextCodec>

namespace {

// 错误关键词（英文 + 中文），命中即计入规则统计
const QStringList kErrKeywords = {
    "error", "exception", "failed", "failure", "timeout", "refused",
    "denied", "crash", "panic", "fatal", "invalid", "abort",
    "失败", "超时", "异常", "错误", "拒绝", "崩溃"
};

// 三种时间戳形态，优先级从上到下（避免纯时间正则误匹配 ISO 里的时间部分）
const char *kTsIsoPattern =
    R"((\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})(?:[.,](\d{1,6}))?)";
const char *kTsApachePattern =
    R"((\d{2})/(\w{3})/(\d{4}):(\d{2}):(\d{2}):(\d{2}))";
const char *kTsTimePattern =
    R"((\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,3}))?)";

} // namespace

// ---- 时间戳提取 ----

QDateTime LogParser::extractTimestamp(const QString &line)
{
    static const QRegularExpression iso(QString::fromLatin1(kTsIsoPattern));
    static const QRegularExpression apache(QString::fromLatin1(kTsApachePattern));
    static const QRegularExpression timeOnly(QString::fromLatin1(kTsTimePattern));

    QRegularExpressionMatch m;

    // ISO：2024-03-15 10:20:30.123 或 2024-03-15T10:20:30
    if ((m = iso.match(line)).hasMatch()) {
        const int y  = m.captured(1).toInt();
        const int mo = m.captured(2).toInt();
        const int d  = m.captured(3).toInt();
        const int h  = m.captured(4).toInt();
        const int mi = m.captured(5).toInt();
        const int s  = m.captured(6).toInt();
        const QString msStr = m.captured(7);
        const int ms = msStr.isEmpty() ? 0 : msStr.left(3).rightJustified(3, '0').toInt();
        return QDateTime(QDate(y, mo, d), QTime(h, mi, s, ms));
    }

    // Apache：15/Mar/2024:10:20:30（月份缩写固定英文，避免 locale 差异）
    if ((m = apache.match(line)).hasMatch()) {
        static const QMap<QString, int> months = {
            {"Jan", 1}, {"Feb", 2}, {"Mar", 3}, {"Apr", 4},  {"May", 5},  {"Jun", 6},
            {"Jul", 7}, {"Aug", 8}, {"Sep", 9}, {"Oct", 10}, {"Nov", 11}, {"Dec", 12}
        };
        const int mo = months.value(m.captured(2), 0);
        if (mo == 0)
            return QDateTime();
        return QDateTime(QDate(m.captured(3).toInt(), mo, m.captured(1).toInt()),
                         QTime(m.captured(4).toInt(), m.captured(5).toInt(), m.captured(6).toInt()));
    }

    // 仅时间：10:20:30.123（日期置 1900-01-01，仅用于排序/展示）
    if ((m = timeOnly.match(line)).hasMatch()) {
        const int h  = m.captured(1).toInt();
        const int mi = m.captured(2).toInt();
        const int s  = m.captured(3).toInt();
        const QString msStr = m.captured(4);
        const int ms = msStr.isEmpty() ? 0 : msStr.left(3).rightJustified(3, '0').toInt();
        return QDateTime(QDate(1900, 1, 1), QTime(h, mi, s, ms));
    }

    return QDateTime();
}

// ---- 级别提取 ----

QString LogParser::extractLevel(const QString &text)
{
    static const QRegularExpression re(
        R"(\b(TRACE|DEBUG|INFO|NOTICE|WARN(?:ING)?|ERROR|ERR|FATAL|CRITICAL|SEVERE)\b)",
        QRegularExpression::CaseInsensitiveOption);

    const auto rank = [](const QString &l) -> int {
        if (l == "FATAL") return 8;
        if (l == "CRITICAL" || l == "SEVERE") return 7;
        if (l == "ERROR" || l == "ERR") return 6;
        if (l == "WARN" || l == "WARNING") return 5;
        if (l == "NOTICE") return 4;
        if (l == "INFO") return 3;
        if (l == "DEBUG") return 2;
        if (l == "TRACE") return 1;
        return 0;
    };

    QString best;
    int bestRank = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QString l = it.next().captured(1).toUpper();
        const int r = rank(l);
        if (r > bestRank) {
            bestRank = r;
            best = (l == "WARNING") ? "WARN" : (l == "ERR" ? "ERROR" : l);
        }
    }
    return best;
}

// ---- 编码探测 ----

QString LogParser::detectAndDecode(const QByteArray &data)
{
    // UTF-8 BOM
    if (data.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(data.mid(3));
    // UTF-16（PowerShell 重定向输出等常见）
    if (data.startsWith("\xFF\xFE")) {
        const int nChars = (data.size() - 2) / 2;
        return QString::fromUtf16(reinterpret_cast<const ushort *>(data.constData() + 2), nChars);
    }
    if (data.startsWith("\xFE\xFF")) {
        QTextCodec *be = QTextCodec::codecForName("UTF-16BE");
        if (be)
            return be->toUnicode(data.mid(2));
    }

    // UTF-8 严格校验：invalidChars == 0 才可信
    QTextCodec *utf8 = QTextCodec::codecForName("UTF-8");
    QTextCodec::ConverterState state;
    state.invalidChars = 0;
    const QString utf8Text = utf8->toUnicode(data.constData(), data.size(), &state);
    if (state.invalidChars == 0)
        return utf8Text;

    // 回退 GB18030（GBK 超集）→ GBK → Latin-1
    QTextCodec *gb = QTextCodec::codecForName("GB18030");
    if (gb)
        return gb->toUnicode(data);
    gb = QTextCodec::codecForName("GBK");
    if (gb)
        return gb->toUnicode(data);
    return QString::fromLatin1(data);
}

// ---- 主入口 ----

bool LogParser::parseFile(const QString &path, QVector<LogEntry> *entries,
                          Stats *stats, QString *errorString)
{
    entries->clear();
    if (stats)
        *stats = Stats();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *errorString = "无法打开文件：" + file.errorString();
        return false;
    }
    if (file.size() > 200 * 1024 * 1024) {
        // 读入本身要有界；大文件靠 LogChunker 的采样策略控制 LLM 请求量
        *errorString = "文件过大（>200MB），暂不支持";
        return false;
    }

    const QByteArray data = file.readAll();
    const QString text = detectAndDecode(data);
    const QStringList lines = text.split('\n');

    int lineNo = 1;
    for (const QString &rawLine : lines) {
        QString line = rawLine;
        if (line.endsWith('\r'))
            line.chop(1);

        // 统计总行数（含空行）
        if (stats)
            ++stats->totalLines;

        // 空行不进 entries（省 LLM token），但行号继续推进，保证与原文一致
        if (line.trimmed().isEmpty()) {
            ++lineNo;
            continue;
        }

        LogEntry e;
        e.lineNumber = lineNo;
        e.text = line;
        e.timestamp = extractTimestamp(line);
        e.level = extractLevel(line);
        entries->append(e);

        // ---- 规则统计 ----
        if (stats) {
            QSet<QString> hitKeywords;
            for (const QString &kw : kErrKeywords) {
                if (line.contains(kw, Qt::CaseInsensitive))
                    hitKeywords.insert(kw.toLower());
            }
            for (const QString &kw : hitKeywords)
                ++stats->errorKeywords[kw];

            if (e.level == "ERROR" || e.level == "FATAL" || e.level == "CRITICAL")
                ++stats->errors;
            else if (e.level == "WARN")
                ++stats->warnings;
            else if (!e.level.isEmpty())
                ++stats->infos;
            else if (!hitKeywords.isEmpty())
                ++stats->errors;      // 无级别但命中错误关键词
            else
                ++stats->unknowns;
        }
        ++lineNo;
    }
    return true;
}
