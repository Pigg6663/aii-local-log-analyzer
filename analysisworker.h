#ifndef ANALYSISWORKER_H
#define ANALYSISWORKER_H

#include <QObject>

#include "datatypes.h"
#include "logparser.h"   // LogParser::Stats（嵌套类型，前向声明不够用）

class LlamaHttpClient;

// 分析管线编排器（W3 真实管线版）：
//   解析（LogParser）→ 分块（LogChunker）→ 逐块 LLM 检测（LlamaHttpClient，
//   输出解析失败用重试提示词再请求，最多 3 次）→ 汇总报告（PromptBuilder）。
//   取消不丢已检出成果：走 finished(部分结果 + 取消报告)，而不是 failed。
// 对象经 moveToThread 常驻 Worker 线程；UI 只通过信号（queued）与它通信。
class AnalysisWorker : public QObject
{
    Q_OBJECT
public:
    explicit AnalysisWorker(QObject *parent = nullptr);

public slots:
    // serverBaseUrl 例：http://127.0.0.1:8080（由 MainWindow 传入，worker 不关心进程管理）
    void analyze(const QString &filePath, const QString &serverBaseUrl);
    void cancel();

signals:
    void progressChanged(int current, int total, const QString &stage);
    void findingFound(const AnomalyFinding &finding);
    void finished(const QVector<AnomalyFinding> &findings, const QString &report);
    void failed(const QString &reason);

private:
    bool m_cancelled = false;          // 只在 worker 线程内读写，无竞态
    LlamaHttpClient *m_http = nullptr; // 惰性创建于 worker 线程（QNAM 线程亲和要求）

    // 取消收尾：保留已检出结果 + 规则兜底报告，走 finished 流程
    void finishCancelled(const QVector<AnomalyFinding> &findings,
                         const LogParser::Stats &stats, const QString &stage);
};

#endif // ANALYSISWORKER_H
