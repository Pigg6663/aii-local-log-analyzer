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

    // 合并"相邻且摘要相同"的条目：模型有时会把同一串连续错误逐行拆成多条
    // （第 33/34/35/36 行各一条），合并后只留一条 33-36。
    // 判定条件：摘要去空白后完全相同，且下一段的起点 <= 当前段的终点 + 1
    //（相邻或重叠）。保留较大者作为合并后的起止行，details 取最长的那条。
    static QVector<AnomalyFinding> mergeAdjacent(const QVector<AnomalyFinding> &in);

private:
    // 清洗：剥 ```json 围栏、掐头去尾找第一个 {/[ 到最后一个 }/]
    static QString extractJsonText(const QString &raw);
    // 容忍尾逗号：[...,] / {...,}
    static QByteArray fixTrailingCommas(const QByteArray &json);
    // 单个 JSON 元素 → AnomalyFinding；不合法返回 false
    static bool parseOne(const QJsonValue &v, AnomalyFinding *out);
};

#endif // RESULTPARSER_H
