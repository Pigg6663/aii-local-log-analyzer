#include "logchunker.h"

namespace {

// 预留 token：提示词模板 + 输出上限（max_tokens 512 ≈ 1024 token 余量）
constexpr int kReservedTokens = 1024;

// 每行的显示格式：行号|内容
QString lineText(const LogEntry &e)
{
    return QString("%1|%2\n").arg(e.lineNumber).arg(e.text);
}

} // namespace

int LogChunker::estimateTokens(const QString &text)
{
    return text.size() / 2 + 1;
}

QVector<LogChunk> LogChunker::chunk(const QVector<LogEntry> &entries,
                                    int ctxSize, int overlapLines)
{
    QVector<LogChunk> chunks;
    if (entries.isEmpty())
        return chunks;

    // 块预算：上下文减预留，再打 8 折留安全余量
    const int budgetTokens = int((ctxSize - kReservedTokens) * 0.8);
    if (budgetTokens < 512)
        return chunks;   // 上下文太小，无法工作

    int start = 0;
    int index = 0;
    while (start < entries.size()) {
        // 贪心装行，超预算即停：end 停在"下一行的开头"，即本块的行边界
        int end = start;
        int tokens = 0;
        while (end < entries.size()) {
            const int lineTokens = estimateTokens(lineText(entries.at(end)));
            if (end > start && tokens + lineTokens > budgetTokens)
                break;
            tokens += lineTokens;
            ++end;
        }
        // 单行超预算时 end == start + 1，该行独占一块（不在行内硬切）

        LogChunk c;
        c.index = index++;
        c.startLine = entries.at(start).lineNumber;
        c.endLine = entries.at(end - 1).lineNumber;
        c.text.clear();
        for (int i = start; i < end; ++i)
            c.text += lineText(entries.at(i));
        chunks.append(c);

        if (end >= entries.size())
            break;
        // 下一块回退 overlapLines 行形成重叠；qMax 保证始终前进，避免死循环
        start = qMax(start + 1, end - overlapLines);
    }
    return chunks;
}
