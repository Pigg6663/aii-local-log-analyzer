#include "analysisworker.h"

#include <QDebug>
#include <QThread>

#include "llamahttpclient.h"
#include "logchunker.h"
#include "logparser.h"
#include "promptbuilder.h"
#include "resultparser.h"

namespace {
constexpr int kMaxChunks = 100;          // 块数超过则均匀采样，控制总请求量
constexpr int kMaxAttemptsPerChunk = 3;  // 含首次，共 3 次尝试
}

AnalysisWorker::AnalysisWorker(QObject *parent)
    : QObject(parent)
{
    // 构造时运行在【主线程】（moveToThread 之前）；槽函数运行在【worker 线程】
    qInfo().noquote() << "[worker] AnalysisWorker 构造于线程" << QThread::currentThread();
}

void AnalysisWorker::analyze(const QString &filePath, const QString &serverBaseUrl)
{
    m_cancelled = false;
    qInfo().noquote() << "[worker] analyze() 执行于线程" << QThread::currentThread()
                      << "file =" << filePath << "server =" << serverBaseUrl;

    // 惰性创建 HTTP 客户端：必须在 worker 线程内创建（QNAM 线程亲和），
    // 不能在构造函数里建（那时还在主线程）。
    if (!m_http) {
        m_http = new LlamaHttpClient(this);
        m_http->setTimeoutMs(180000);   // CPU 推理慢，单块最多等 3 分钟
    }
    m_http->setBaseUrl(serverBaseUrl);
    m_http->setJsonMode(true);          // 检测阶段要求 JSON 输出

    // ---- 阶段 1：解析 ----
    emit progressChanged(0, 100, "解析日志文件");
    QVector<LogEntry> entries;
    LogParser::Stats stats;
    QString err;
    if (!LogParser::parseFile(filePath, &entries, &stats, &err)) {
        emit failed("解析失败：" + err);
        return;
    }
    if (entries.isEmpty()) {
        emit failed("日志文件为空，没有可分析的内容");
        return;
    }
    if (m_cancelled) { emit failed("已取消"); return; }

    // ---- 阶段 2：分块 ----
    emit progressChanged(5, 100, QString("切分日志块（共 %1 行）").arg(entries.size()));
    const QVector<LogChunk> allChunks = LogChunker::chunk(entries);
    if (allChunks.isEmpty()) {
        emit failed("日志无法分块（上下文大小设置异常？）");
        return;
    }
    QVector<LogChunk> chunks;
    bool sampled = false;
    if (allChunks.size() > kMaxChunks) {
        // 超大文件：均匀采样 kMaxChunks 块，而不是只取头部
        sampled = true;
        chunks.reserve(kMaxChunks);
        for (int i = 0; i < kMaxChunks; ++i) {
            const int idx = qRound(double(i) * (allChunks.size() - 1) / (kMaxChunks - 1));
            chunks.append(allChunks.at(idx));
        }
    } else {
        chunks = allChunks;
    }
    if (m_cancelled) { emit failed("已取消"); return; }

    // ---- 阶段 3：逐块 LLM 检测 ----
    const QString systemPrompt = PromptBuilder::systemPrompt();
    QVector<AnomalyFinding> allFindings;
    int chunksUnparsable = 0;
    const int total = chunks.size();

    for (int i = 0; i < total; ++i) {
        if (m_cancelled) { emit failed("已取消"); return; }

        emit progressChanged(10 + 75 * i / total, 100,
                             QString("LLM 检测中 %1/%2").arg(i + 1).arg(total));

        const LogChunk &chunk = chunks.at(i);
        QVector<AnomalyFinding> findings;
        QString userPrompt = PromptBuilder::chunkDetectionPrompt(chunk);
        QString raw, chatErr;
        int attempt = 0;

        for (; attempt < kMaxAttemptsPerChunk; ++attempt) {
            if (m_cancelled) { emit failed("已取消"); return; }

            if (!m_http->chat(systemPrompt, userPrompt, &raw, &chatErr, 512)) {
                if (m_cancelled) { emit failed("已取消"); return; }
                emit failed(QString("LLM 请求失败（第 %1/%2 块）：%3。"
                                    "推理服务可能已崩溃，请查看服务日志后重试")
                            .arg(i + 1).arg(total).arg(chatErr));
                return;
            }

            const ResultParser::ParseResult parsed = ResultParser::parseFindings(raw);
            if (parsed.status == ResultParser::ParseStatus::Ok) {
                findings = parsed.findings;
                break;
            }
            // 解析失败：换重试提示词（带日志原文 + 坏输出 + 严格指令）
            userPrompt = PromptBuilder::retryPrompt(chunk, raw);
        }

        if (attempt == kMaxAttemptsPerChunk) {
            // 3 次都解析不了：该块落规则兜底，提示人工检查
            ++chunksUnparsable;
            AnomalyFinding f;
            f.severity = "low";
            f.startLine = chunk.startLine;
            f.endLine = chunk.endLine;
            f.summary = "该块 LLM 输出不可解析（已重试 3 次），请人工检查此段日志";
            f.details = QString("日志块 %1/%2（第 %3-%4 行）")
                            .arg(i + 1).arg(total).arg(chunk.startLine).arg(chunk.endLine);
            findings.append(f);
        }

        // 流式：结果随检测进度一条条出现在 UI
        for (const AnomalyFinding &f : findings) {
            allFindings.append(f);
            emit findingFound(f);
        }
    }

    // ---- 阶段 4：汇总报告 ----
    emit progressChanged(90, 100, "生成总体报告");
    QString report;
    {
        m_http->setJsonMode(false);   // 汇总输出 Markdown 自由文本，不要 JSON 语法约束
        QString raw, chatErr;
        const bool ok = m_http->chat(
            PromptBuilder::summarySystemPrompt(),
            PromptBuilder::summaryPrompt(allFindings, stats, stats.totalLines,
                                         chunks.size(), allChunks.size(),
                                         sampled, chunksUnparsable),
            &raw, &chatErr, 1024);
        m_http->setJsonMode(true);
        if (ok && !raw.trimmed().isEmpty())
            report = raw;
        else
            report = PromptBuilder::ruleFallbackReport(stats, stats.totalLines, chatErr);
    }
    if (m_cancelled) { emit failed("已取消"); return; }

    emit progressChanged(100, 100, "完成");
    emit finished(allFindings, report);
}

void AnalysisWorker::cancel()
{
    m_cancelled = true;
    // 若正阻塞在 chat() 里：abort 让它立刻返回（错误为"已取消"）
    if (m_http)
        m_http->abortCurrent();
}
