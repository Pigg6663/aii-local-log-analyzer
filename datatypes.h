#ifndef DATATYPES_H
#define DATATYPES_H

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QVector>

// 跨线程 / 跨模块传递的纯数据结构（无行为）。
// 凡是用于 queued 信号槽传参的自定义类型，必须 Q_DECLARE_METATYPE
// 并在运行时 qRegisterMetaType（见 main.cpp）。

struct LogEntry {        // 一条日志
    int lineNumber = 0;  // 原文件行号（1-based）
    QString text;
    QDateTime timestamp; // 正则提取，无效则无效值
    QString level;       // INFO/WARN/ERROR/…，空串 = 未知
};

struct LogChunk {        // 一个送进 LLM 的块
    int index = 0;
    int startLine = 0;   // 对应原文件行号范围（含重叠）
    int endLine = 0;
    QString text;
};

struct AnomalyFinding {  // 一条检测结果
    QString severity;    // low / medium / high / critical
    int startLine = 0;
    int endLine = 0;
    QString summary;
    QString details;
};

Q_DECLARE_METATYPE(AnomalyFinding)
Q_DECLARE_METATYPE(QVector<AnomalyFinding>)

#endif // DATATYPES_H
