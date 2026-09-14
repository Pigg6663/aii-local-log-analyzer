#include "resultparser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

QString ResultParser::extractJsonText(const QString &raw)
{
    // 掐头去尾：第一个 { 或 [ 到最后一个 } 或 ]
    const int firstBrace = raw.indexOf('{');
    const int firstBracket = raw.indexOf('[');
    int first = -1;
    if (firstBrace >= 0 && firstBracket >= 0)
        first = qMin(firstBrace, firstBracket);
    else
        first = qMax(firstBrace, firstBracket);
    const int last = qMax(raw.lastIndexOf('}'), raw.lastIndexOf(']'));
    if (first < 0 || last <= first)
        return QString();
    return raw.mid(first, last - first + 1);
}

QByteArray ResultParser::fixTrailingCommas(const QByteArray &json)
{
    QString s = QString::fromUtf8(json);
    static const QRegularExpression re(QStringLiteral(",\\s*([}\\]])"));
    s.replace(re, QStringLiteral("\\1"));
    return s.toUtf8();
}

bool ResultParser::parseOne(const QJsonValue &v, AnomalyFinding *out)
{
    if (!v.isObject())
        return false;
    const QJsonObject o = v.toObject();

    out->summary = o.value("summary").toString().trimmed();
    if (out->summary.isEmpty())
        return false;   // 没 summary 的条目多半是模型输出噪声，丢弃

    // severity 归一：非法值按 medium 处理
    QString severity = o.value("severity").toString().trimmed().toLower();
    if (severity != "low" && severity != "medium"
        && severity != "high" && severity != "critical")
        severity = "medium";
    out->severity = severity;

    // 行号：容忍数字字符串
    const auto toLine = [](const QJsonValue &v) -> int {
        if (v.isDouble())
            return v.toInt();
        if (v.isString())
            return v.toString().toInt();
        return 0;
    };
    int start = toLine(o.value("start_line"));
    int end = toLine(o.value("end_line"));
    if (start > 0 && end == 0)
        end = start;
    if (end > 0 && start == 0)
        start = end;
    if (start > end)
        qSwap(start, end);
    out->startLine = start;
    out->endLine = end;

    out->details = o.value("details").toString().trimmed();
    if (out->details.isEmpty())
        out->details = out->summary;
    return true;
}

ResultParser::ParseResult ResultParser::parseFindings(const QString &raw)
{
    ParseResult result;

    const QString jsonText = extractJsonText(raw);
    if (jsonText.isEmpty()) {
        result.status = ParseStatus::Retry;   // 连 {} 或 [] 都没有
        return result;
    }

    QJsonParseError err;
    const QJsonDocument doc =
        QJsonDocument::fromJson(fixTrailingCommas(jsonText.toUtf8()), &err);
    if (err.error != QJsonParseError::NoError || doc.isNull()) {
        result.status = ParseStatus::Retry;
        return result;
    }

    // 接受两种形态：[ {...}, ... ] 或 { "findings"/"results"/...: [ ... ] }
    QJsonArray arr;
    if (doc.isArray()) {
        arr = doc.array();
    } else if (doc.isObject()) {
        const QJsonObject obj = doc.object();
        const QStringList wrapKeys = {"findings", "results", "anomalies", "issues", "items"};
        bool wrapped = false;
        for (const QString &k : wrapKeys) {
            if (obj.value(k).isArray()) {
                arr = obj.value(k).toArray();
                wrapped = true;
                break;
            }
        }
        if (!wrapped) {
            // 对象本身就是一条结果（模型偷懒没包数组），宽容处理
            arr.append(obj);
        }
    }

    for (const QJsonValue &v : arr) {
        AnomalyFinding f;
        if (parseOne(v, &f))
            result.findings.append(f);
    }

    result.status = ParseStatus::Ok;
    return result;
}
