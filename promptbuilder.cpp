#include "promptbuilder.h"

#include <QPair>
#include <QStringList>
#include <algorithm>

namespace {

// 规则统计里的高频错误关键词（Top 8），两个报告函数共用
QString topKeywords(const LogParser::Stats &stats)
{
    QVector<QPair<int, QString>> top;
    for (auto it = stats.errorKeywords.constBegin(); it != stats.errorKeywords.constEnd(); ++it)
        top.append(qMakePair(it.value(), it.key()));
    std::sort(top.begin(), top.end(),
              [](const QPair<int, QString> &a, const QPair<int, QString> &b) {
                  return a.first > b.first;
              });
    QStringList kws;
    const int n = qMin(8, top.size());
    for (int i = 0; i < n; ++i)
        kws << QString("%1(%2行)").arg(top.at(i).second).arg(top.at(i).first);
    return kws.isEmpty() ? QString("无") : kws.join("、");
}

// 检测结果清单（按严重度排序，最多 40 条），汇总提示与取消报告共用
void appendFindingsList(QString &s, const QVector<AnomalyFinding> &findings)
{
    QVector<AnomalyFinding> listed = findings;
    const auto sevRank = [](const QString &sev) {
        if (sev == "critical") return 4;
        if (sev == "high") return 3;
        if (sev == "medium") return 2;
        return 1;
    };
    std::sort(listed.begin(), listed.end(),
              [&](const AnomalyFinding &a, const AnomalyFinding &b) {
                  return sevRank(a.severity) > sevRank(b.severity);
              });
    int shown = 0;
    for (const AnomalyFinding &f : listed) {
        if (shown >= 40) {
            s += QStringLiteral("…（其余 %1 条已省略）\n").arg(listed.size() - shown);
            break;
        }
        s += QString("- [%1] 第 %2-%3 行：%4（%5）\n")
                 .arg(f.severity)
                 .arg(f.startLine > 0 ? QString::number(f.startLine) : QStringLiteral("?"))
                 .arg(f.endLine > 0 ? QString::number(f.endLine) : QStringLiteral("?"))
                 .arg(f.summary)
                 .arg(f.details);
        ++shown;
    }
}

} // namespace

QString PromptBuilder::systemPrompt()
{
    return QStringLiteral(
        "你是一名资深运维日志分析专家，擅长从日志中识别异常、错误和潜在风险。\n"
        "\n"
        "你的任务：分析给定的日志片段（每行格式为 行号|内容），找出异常并输出检测结果。\n"
        "\n"
        "输出要求（必须严格遵守）：\n"
        "1. 只输出一个 JSON 数组，不要任何解释、前言或 Markdown 代码块。\n"
        "2. 数组每个元素是一条检测结果，字段如下：\n"
        "   {\n"
        "     \"severity\": \"low\" | \"medium\" | \"high\" | \"critical\",\n"
        "     \"start_line\": <整数，异常起始行号>,\n"
        "     \"end_line\": <整数，异常结束行号>,\n"
        "     \"summary\": \"<一句话中文描述>\",\n"
        "     \"details\": \"<详细解释，说明判断依据，可多句>\"\n"
        "   }\n"
        "3. 行号必须引用日志中实际出现的行号。\n"
        "4. 没有发现异常时输出空数组 []。\n"
        "5. 不要编造日志中不存在的内容，异常判定从严（宁缺毋滥）。\n"
        "6. 相邻且同因的异常合并为一条，用起止行号表示范围。");
}

QString PromptBuilder::chunkDetectionPrompt(const LogChunk &chunk)
{
    return QString(
        "请分析以下日志片段（第 %1-%2 行），找出异常、错误和潜在风险。\n"
        "\n"
        "=== 日志片段开始 ===\n"
        "%3"
        "=== 日志片段结束 ===\n"
        "\n"
        "请按系统提示要求的 JSON 数组格式输出检测结果。")
        .arg(chunk.startLine).arg(chunk.endLine).arg(chunk.text);
}

QString PromptBuilder::retryPrompt(const LogChunk &chunk, const QString &lastFailedOutput)
{
    // 坏输出截断，避免重试提示词本身撑爆上下文
    const QString trimmed = lastFailedOutput.left(1500);
    return QString(
        "你上一次的输出不是合法的 JSON，无法解析。请重新分析同一日志片段，"
        "并严格按照系统提示的 JSON 数组格式输出，只输出 JSON 本身，"
        "不要任何其他内容（不要 Markdown 代码块、不要解释）。\n"
        "\n"
        "=== 日志片段开始（与上次相同）===\n"
        "%1"
        "=== 日志片段结束 ===\n"
        "\n"
        "你上一次的错误输出如下（注意：不要沿用它的格式错误）：\n"
        "---\n"
        "%2\n"
        "---")
        .arg(chunk.text, trimmed);
}

QString PromptBuilder::summarySystemPrompt()
{
    return QStringLiteral(
        "你是一名资深运维日志分析专家，擅长撰写条理清晰、结论明确的日志分析报告。");
}

QString PromptBuilder::summaryPrompt(const QVector<AnomalyFinding> &findings,
                                     const LogParser::Stats &stats,
                                     int totalLines, int chunksAnalyzed, int chunksTotal,
                                     bool sampled, int chunksUnparsable)
{
    QString s;
    s += QStringLiteral("请基于以下材料，输出一份日志分析总体报告（Markdown 格式，中文）。\n\n");

    s += QStringLiteral("## 分析范围\n");
    s += QString("- 日志总行数：%1\n").arg(totalLines);
    s += QString("- 分析分块：%1/%2 块%3\n")
             .arg(chunksAnalyzed).arg(chunksTotal)
             .arg(sampled ? QStringLiteral("（文件过大，已均匀采样）") : QString());
    if (chunksUnparsable > 0)
        s += QString("- %1 个分块的 LLM 输出不可解析（已提示人工检查）\n").arg(chunksUnparsable);

    s += QStringLiteral("\n## 规则统计（非 LLM 结果，供交叉验证）\n");
    s += QString("- ERROR/FATAL 行数：%1\n").arg(stats.errors);
    s += QString("- WARN 行数：%1\n").arg(stats.warnings);
    s += QString("- 高频错误关键词：%1\n").arg(topKeywords(stats));

    s += QStringLiteral("\n## LLM 检测结果（共 %1 条）\n").arg(findings.size());
    if (findings.isEmpty()) {
        s += QStringLiteral("（无）\n");
    } else {
        appendFindingsList(s, findings);
    }

    s += QStringLiteral(
        "\n## 请输出以下内容的报告：\n"
        "1. 总体健康度评估（一句话）\n"
        "2. 主要异常按严重程度排序的摘要（合并同类，突出最严重的 3-5 个）\n"
        "3. 根因分析与排查建议\n"
        "4. 若规则统计与 LLM 检测结果明显矛盾，说明可能的原因\n"
        "直接输出报告正文，不要重复引用提示词。");
    return s;
}

QString PromptBuilder::ruleFallbackReport(const LogParser::Stats &stats, int totalLines,
                                          const QString &llmError)
{
    QString s;
    s += QStringLiteral("## 总体报告（LLM 汇总失败，以下为纯规则统计兜底）\n\n");
    s += QString("- LLM 失败原因：%1\n\n").arg(llmError);
    s += QString("- 日志总行数：%1\n").arg(totalLines);
    s += QString("- ERROR/FATAL：%1 行\n").arg(stats.errors);
    s += QString("- WARN：%1 行\n").arg(stats.warnings);
    s += QString("- INFO/DEBUG/TRACE：%1 行\n").arg(stats.infos);
    s += QString("- 未识别级别：%1 行\n").arg(stats.unknowns);
    s += QString("- 高频错误关键词：%1\n").arg(topKeywords(stats));
    s += QStringLiteral("\n建议：优先排查 ERROR/FATAL 集中的时间段与高频错误关键词相关代码路径。");
    return s;
}

QString PromptBuilder::cancelledReport(const QVector<AnomalyFinding> &findings,
                                       const LogParser::Stats &stats, int totalLines,
                                       const QString &cancelStage)
{
    QString s;
    s += QStringLiteral("## 分析已取消（部分结果）\n\n");
    s += QString("- 取消位置：%1\n").arg(cancelStage);
    s += QString("- 日志总行数：%1\n").arg(totalLines);

    s += QStringLiteral("\n## 已检出异常（共 %1 条）\n").arg(findings.size());
    if (findings.isEmpty()) {
        s += QStringLiteral("（无）\n");
    } else {
        appendFindingsList(s, findings);
    }

    s += QStringLiteral("\n## 规则统计\n\n");
    s += QString("- ERROR/FATAL：%1 行\n").arg(stats.errors);
    s += QString("- WARN：%1 行\n").arg(stats.warnings);
    s += QString("- INFO/DEBUG/TRACE：%1 行\n").arg(stats.infos);
    s += QString("- 未识别级别：%1 行\n").arg(stats.unknowns);
    s += QString("- 高频错误关键词：%1\n").arg(topKeywords(stats));
    s += QStringLiteral("\n如需完整分析，请重新点击“开始分析”。");
    return s;
}
