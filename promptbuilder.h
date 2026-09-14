#ifndef PROMPTBUILDER_H
#define PROMPTBUILDER_H

#include <QString>
#include <QVector>

#include "datatypes.h"
#include "logparser.h"

// 提示词模板（纯静态函数，可在任意线程调用）。全部中文，面向中文日志场景。
class PromptBuilder
{
public:
    // 检测阶段系统提示：角色设定 + JSON 输出 schema
    static QString systemPrompt();

    // 逐块检测的用户提示
    static QString chunkDetectionPrompt(const LogChunk &chunk);

    // 块级 JSON 解析失败后的重试提示。
    // 注意必须带上日志块原文——否则模型上下文里只剩坏输出，重试必然失败。
    static QString retryPrompt(const LogChunk &chunk, const QString &lastFailedOutput);

    // 汇总阶段系统提示（自由文本 Markdown，无 JSON 约束）
    static QString summarySystemPrompt();

    // 汇总阶段用户提示：块级结果 + 规则统计
    static QString summaryPrompt(const QVector<AnomalyFinding> &findings,
                                 const LogParser::Stats &stats,
                                 int totalLines, int chunksAnalyzed, int chunksTotal,
                                 bool sampled, int chunksUnparsable);

    // LLM 汇总失败时的纯规则兜底报告（保证永远有产出）
    static QString ruleFallbackReport(const LogParser::Stats &stats, int totalLines,
                                      const QString &llmError);
};

#endif // PROMPTBUILDER_H
