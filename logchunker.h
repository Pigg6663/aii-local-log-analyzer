#ifndef LOGCHUNKER_H
#define LOGCHUNKER_H

#include <QVector>

#include "datatypes.h"

// 按 token 预算把日志行切成可送进 LLM 的块。
// 只在行边界切；相邻块重叠上一块末尾几行（保持跨块上下文连续）。
// 块内文本格式为 行号|内容（LLM 依据行号回报 line_range）。
// 纯静态方法，可在任意线程调用。
class LogChunker
{
public:
    // 保守估算：1 token ≈ 2 字符（中文 1 token ≈ 1.5~2 字符，英文 ≈ 4 字符，取安全值）
    static int estimateTokens(const QString &text);

    // ctxSize：llama-server 上下文大小（默认 8192）
    // overlapLines：相邻块重叠行数（默认 8，要求 ≥ 1）
    static QVector<LogChunk> chunk(const QVector<LogEntry> &entries,
                                   int ctxSize = 8192,
                                   int overlapLines = 8);
};

#endif // LOGCHUNKER_H
