#ifndef RESULTPARSER_H
#define RESULTPARSER_H

#include <QJsonValue>
#include <QString>
#include <QVector>

#include "datatypes.h"

// 解析 LLM 输出的 JSON 检测结果。纯静态方法，可在任意线程调用。
class ResultParser
{
public:
    enum class ParseStatus {
        Ok,     // 解析成功（空数组也算成功，表示"无异常"）
        Retry   // 输出不是合法 JSON，建议用重试提示词再请求
    };

    struct ParseResult {
        ParseStatus status = ParseStatus::Retry;
        QVector<AnomalyFinding> findings;
    };

    static ParseResult parseFindings(const QString &raw);

private:
    // 清洗：剥 ```json 围栏、掐头去尾找第一个 {/[ 到最后一个 }/]
    static QString extractJsonText(const QString &raw);
    // 容忍尾逗号：[...,] / {...,}
    static QByteArray fixTrailingCommas(const QByteArray &json);
    // 单个 JSON 元素 → AnomalyFinding；不合法返回 false
    static bool parseOne(const QJsonValue &v, AnomalyFinding *out);
};

#endif // RESULTPARSER_H
