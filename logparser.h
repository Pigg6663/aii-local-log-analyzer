#ifndef LOGPARSER_H
#define LOGPARSER_H

#include <QMap>
#include <QString>
#include <QVector>

#include "datatypes.h"

// 日志解析：读文件（UTF-8/GBK 编码探测）→ 按行切分 → 正则提取时间戳/级别 → 规则统计。
// 纯静态方法、无状态，可在任意线程调用（本项目在 worker 线程）。
class LogParser
{
public:
    struct Stats {                     // 纯规则统计：LLM 挂掉时的兜底产物
        int totalLines = 0;            // 原始行数（含空行）
        int errors = 0;                // ERROR/FATAL/CRITICAL，或命中错误关键词
        int warnings = 0;
        int infos = 0;
        int unknowns = 0;              // 未提取到级别、也未命中关键词
        QMap<QString, int> errorKeywords; // 错误关键词 → 命中行数
    };

    // 读取并解析日志文件。成功返回 true；失败时 *errorString 给出原因。
    // entries 只含非空行（lineNumber 保持与原文件一致，供结果跳转）。
    static bool parseFile(const QString &path, QVector<LogEntry> *entries,
                          Stats *stats, QString *errorString);

    // 编码探测：UTF-8 严格校验优先，回退 GB18030/GBK，再不行 Latin-1（绝不返回空串）。
    static QString detectAndDecode(const QByteArray &data);

    // 提取级别，返回规范值（TRACE/DEBUG/INFO/NOTICE/WARN/ERROR/FATAL/CRITICAL），
    // 一行多个级别时取最高者；未找到返回空串。
    static QString extractLevel(const QString &text);

private:
    static QDateTime extractTimestamp(const QString &line);
};

#endif // LOGPARSER_H
