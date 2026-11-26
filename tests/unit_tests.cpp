// aii 单元测试：只覆盖三个"纯函数"模块，不需要模型、不需要 GUI、不需要网络。
//
//   LogParser    —— 编码探测 / 级别提取 / 行解析与规则统计
//   LogChunker   —— token 估算、块预算、重叠行、超长单行
//   ResultParser —— JSON 容错（围栏 / 尾逗号 / 包装对象 / 非法输出）
//
// 用法：cmake --build build --target unit_tests && ctest --test-dir build -V
//      返回码 0 = 全过，非 0 = 有断言失败（CTest 据此判红绿）。

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "logchunker.h"
#include "logparser.h"
#include "resultparser.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void reportFailure(const char *file, int line, const char *expr)
{
    ++g_failures;
    qCritical().noquote() << QString("  [FAIL] %1:%2  %3").arg(file).arg(line).arg(expr);
}

// —— 断言宏：失败只记录并继续，最后统一统计，避免第一个失败就中断 ——
#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond))                                                           \
            reportFailure(__FILE__, __LINE__, #cond);                           \
    } while (0)

#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        ++g_checks;                                                            \
        const auto a_ = (actual);                                              \
        const auto e_ = (expected);                                            \
        if (!(a_ == e_)) {                                                     \
            reportFailure(__FILE__, __LINE__, #actual " == " #expected);        \
            qCritical().noquote()                                                 \
                << QString("         actual = %1, expected = %2")                  \
                       .arg(QString("%1").arg(a_), QString("%1").arg(e_));         \
        }                                                                      \
    } while (0)

void testLogParserLevel()
{
    qInfo().noquote() << "LogParser::extractLevel";
    CHECK_EQ(LogParser::extractLevel("2026-03-15 10:06:44.902 [ERROR] [db-pool] exhausted"),
             QString("ERROR"));
    CHECK_EQ(LogParser::extractLevel("warn: disk almost full"), QString("WARN"));
    CHECK_EQ(LogParser::extractLevel("WARNING disk almost full"), QString("WARN"));
    // 一行多个级别取最高者
    CHECK_EQ(LogParser::extractLevel("INFO then ERROR then FATAL"), QString("FATAL"));
    CHECK_EQ(LogParser::extractLevel("plain text without level"), QString());
    // 分级：CRITICAL 高于 ERROR
    CHECK_EQ(LogParser::extractLevel("CRITICAL: kernel panic (after ERROR)"), QString("CRITICAL"));
}

void testLogParserDecode()
{
    qInfo().noquote() << "LogParser::detectAndDecode";
    // UTF-8 带 BOM
    QByteArray withBom("\xEF\xBB\xBF");
    withBom += QString("中文日志 log").toUtf8();
    CHECK_EQ(LogParser::detectAndDecode(withBom), QString("中文日志 log"));

    // UTF-8 无 BOM
    CHECK_EQ(LogParser::detectAndDecode(QString("中文日志 log").toUtf8()),
             QString("中文日志 log"));

    // GB18030（"中文" = D6 D0 CE C4）：不是合法 UTF-8，必须回退到 GB18030
    const QByteArray gbk("\xD6\xD0\xCE\xC4");
    CHECK_EQ(LogParser::detectAndDecode(gbk), QString("中文"));

    // UTF-16 LE（PowerShell 重定向输出的常见编码）
    QByteArray utf16("\xFF\xFE");
    utf16 += QByteArray(reinterpret_cast<const char *>(QString("中文").utf16()),
                        QString("中文").size() * 2);
    CHECK_EQ(LogParser::detectAndDecode(utf16), QString("中文"));

    // 非法 UTF-8 的原始字节：不允许返回空串（宁可乱码也要让用户看到内容）
    const QByteArray junk("\x80\x81\x82\x83");
    CHECK(!LogParser::detectAndDecode(junk).isEmpty());
}

void testLogParserFile()
{
    qInfo().noquote() << "LogParser::parseFile";

    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath("sample.log");

    // 第 2 行是空行：不进 entries，但行号必须继续推进（否则结果跳转会错位）
    const QString content = "line1 [INFO] ok\n"
                            "\n"
                            "line3 [ERROR] failed to connect\n"
                            "line4 中文错误\n";
    QFile f(path);
    CHECK(f.open(QIODevice::WriteOnly));
    f.write(content.toUtf8());
    f.close();

    QVector<LogEntry> entries;
    LogParser::Stats stats;
    QString err;
    CHECK(LogParser::parseFile(path, &entries, &stats, &err));
    CHECK_EQ(err, QString());

    CHECK_EQ(entries.size(), 3);
    if (entries.size() == 3) {
        CHECK_EQ(entries[0].lineNumber, 1);
        CHECK_EQ(entries[1].lineNumber, 3);   // 空行被跳过，行号不塌陷
        CHECK_EQ(entries[2].lineNumber, 4);
        CHECK_EQ(entries[1].level, QString("ERROR"));
        // 时间戳：本例没有时间戳字段，应为无效 QDateTime
        CHECK(!entries[0].timestamp.isValid());
    }

    CHECK_EQ(stats.totalLines, 4);            // 含空行
    CHECK_EQ(stats.errors, 2);                // line3 有级别；line4 无级别但命中"错误"关键词
    CHECK_EQ(stats.infos, 1);                 // line1
    CHECK(stats.errorKeywords.contains("failed"));

    // 不存在的文件：必须返回 false 并给出原因
    QVector<LogEntry> none;
    LogParser::Stats s2;
    QString err2;
    CHECK(!LogParser::parseFile(dir.filePath("missing.log"), &none, &s2, &err2));
    CHECK(!err2.isEmpty());
}

void testLogChunkerBasics()
{
    qInfo().noquote() << "LogChunker::estimateTokens / chunk 基本行为";

    CHECK_EQ(LogChunker::estimateTokens("abcd"), 3);   // size/2 + 1
    CHECK_EQ(LogChunker::estimateTokens(""), 1);

    // 空输入 → 空输出
    CHECK(LogChunker::chunk(QVector<LogEntry>()).isEmpty());

    // 10 行短日志 → 单块，行号范围覆盖 1..10
    QVector<LogEntry> small;
    for (int i = 1; i <= 10; ++i) {
        LogEntry e;
        e.lineNumber = i;
        e.text = QString("line %1").arg(i);
        small.append(e);
    }
    const QVector<LogChunk> one = LogChunker::chunk(small);
    CHECK_EQ(one.size(), 1);
    if (one.size() == 1) {
        CHECK_EQ(one[0].startLine, 1);
        CHECK_EQ(one[0].endLine, 10);
        CHECK_EQ(one[0].index, 0);
        CHECK(one[0].text.contains("1|line 1"));   // 行号|内容 的格式
    }

    // 上下文太小：(2048 - 2048) * 0.8 = 0 < 512 → 不分块
    CHECK(LogChunker::chunk(small, 2048).isEmpty());
}

void testLogChunkerOverlap()
{
    qInfo().noquote() << "LogChunker::chunk 重叠与超长行";

    // 100 行长行（每行 200 字符 ≈ 100 token），默认 ctx=8192 预算 4915 token → 必然多块
    QVector<LogEntry> many;
    const QString longText(200, QChar('x'));
    for (int i = 1; i <= 100; ++i) {
        LogEntry e;
        e.lineNumber = i;
        e.text = longText;
        many.append(e);
    }
    const QVector<LogChunk> chunks = LogChunker::chunk(many);
    CHECK(chunks.size() >= 2);
    if (chunks.size() >= 2) {
        // 相邻块必须重叠 8 行：下一块起点 = 上一块终点 - 7
        CHECK_EQ(chunks[1].startLine, chunks[0].endLine - 7);
        // 块必须一直前进，不能原地打转
        CHECK(chunks[1].startLine > chunks[0].startLine);
        // 最后一块必须覆盖到文件末尾
        CHECK_EQ(chunks.last().endLine, 100);
        // index 连续
        CHECK_EQ(chunks[0].index, 0);
        CHECK_EQ(chunks[1].index, 1);
    }

    // 超长单行：独占一块，不在行内硬切
    QVector<LogEntry> huge;
    LogEntry h;
    h.lineNumber = 7;
    h.text = QString(200000, QChar('y'));
    huge.append(h);
    const QVector<LogChunk> hc = LogChunker::chunk(huge);
    CHECK_EQ(hc.size(), 1);
    if (hc.size() == 1) {
        CHECK_EQ(hc[0].startLine, 7);
        CHECK_EQ(hc[0].endLine, 7);
        CHECK_EQ(hc[0].text.size(), 200000 + QString("7|").size() + 1);   // 行号前缀 + 换行
    }
}

void testResultParser()
{
    qInfo().noquote() << "ResultParser::parseFindings";

    // 1) 空数组：解析成功，0 条
    {
        const auto r = ResultParser::parseFindings("[]");
        CHECK_EQ(int(r.status), int(ResultParser::ParseStatus::Ok));
        CHECK_EQ(r.findings.size(), 0);
    }

    // 2) 裸数组 + 全部字段
    {
        const auto r = ResultParser::parseFindings(
            R"([{"severity":"high","start_line":16,"end_line":18,"summary":"连接池耗尽","details":"active=50"}])");
        CHECK_EQ(int(r.status), int(ResultParser::ParseStatus::Ok));
        CHECK_EQ(r.findings.size(), 1);
        if (r.findings.size() == 1) {
            CHECK_EQ(r.findings[0].severity, QString("high"));
            CHECK_EQ(r.findings[0].startLine, 16);
            CHECK_EQ(r.findings[0].endLine, 18);
            CHECK_EQ(r.findings[0].summary, QString("连接池耗尽"));
            CHECK_EQ(r.findings[0].details, QString("active=50"));
        }
    }

    // 3) ```json 围栏 + 前后废话：必须能剥出来
    {
        const auto r = ResultParser::parseFindings(
            "好的，分析结果如下：\n```json\n[{\"summary\":\"fenced\",\"start_line\":3}]\n```\n以上。");
        CHECK_EQ(int(r.status), int(ResultParser::ParseStatus::Ok));
        CHECK_EQ(r.findings.size(), 1);
        if (!r.findings.isEmpty())
            CHECK_EQ(r.findings[0].summary, QString("fenced"));
    }

    // 4) 尾逗号：模型常见输出
    {
        const auto r = ResultParser::parseFindings(
            R"([{"summary":"trailing","start_line":5,},])");
        CHECK_EQ(int(r.status), int(ResultParser::ParseStatus::Ok));
        CHECK_EQ(r.findings.size(), 1);
    }

    // 5) {"findings":[...]} 包装形态
    {
        const auto r = ResultParser::parseFindings(
            R"({"findings":[{"summary":"wrapped","start_line":9,"end_line":10}]})");
        CHECK_EQ(int(r.status), int(ResultParser::ParseStatus::Ok));
        CHECK_EQ(r.findings.size(), 1);
        if (!r.findings.isEmpty())
            CHECK_EQ(r.findings[0].endLine, 10);
    }

    // 6) 模型偷懒：没有数组，单个对象（宽容处理成 1 条）
    {
        const auto r = ResultParser::parseFindings(
            R"({"summary":"single object","start_line":2})");
        CHECK_EQ(r.findings.size(), 1);
    }

    // 7) severity 归一：大写 → 小写；非法值 → medium
    {
        const auto r = ResultParser::parseFindings(
            R"([{"summary":"a","severity":"HIGH"},{"summary":"b","severity":"开天辟地"}])");
        CHECK_EQ(r.findings.size(), 2);
        if (r.findings.size() == 2) {
            CHECK_EQ(r.findings[0].severity, QString("high"));
            CHECK_EQ(r.findings[1].severity, QString("medium"));
        }
    }

    // 8) 缺 summary 的噪声条目被丢弃
    {
        const auto r = ResultParser::parseFindings(R"([{"start_line":4}])");
        CHECK_EQ(r.findings.size(), 0);
    }

    // 9) 行号容错：只给一端 / 字符串形式 / 起止颠倒
    {
        const auto r = ResultParser::parseFindings(
            R"([{"summary":"one-end","start_line":7},)"
            R"({"summary":"string","start_line":"11","end_line":"12"},)"
            R"({"summary":"swapped","start_line":20,"end_line":15}])");
        CHECK_EQ(r.findings.size(), 3);
        if (r.findings.size() == 3) {
            CHECK_EQ(r.findings[0].endLine, 7);          // 只给 start → end 补成同值
            CHECK_EQ(r.findings[1].startLine, 11);       // 数字字符串
            CHECK_EQ(r.findings[2].startLine, 15);       // 起止颠倒 → 交换
            CHECK_EQ(r.findings[2].endLine, 20);
        }
    }

    // 10) 完全不是 JSON（截断 / 自然语言）→ Retry，触发重试提示词
    {
        CHECK_EQ(int(ResultParser::parseFindings("抱歉，我无法分析这段日志。").status),
                 int(ResultParser::ParseStatus::Retry));
        CHECK_EQ(int(ResultParser::parseFindings("[{\"summary\": \"半截").status),
                 int(ResultParser::ParseStatus::Retry));
        CHECK_EQ(int(ResultParser::parseFindings("").status),
                 int(ResultParser::ParseStatus::Retry));
    }
}

// 结果合并：模型逐行输出同一串连续错误时，列表里会出现一堆 33-33 / 34-34，
// 合并后应只剩一条 33-36。
void testResultParserMerge()
{
    qInfo().noquote() << "ResultParser::mergeAdjacent";

    const auto F = [](const char *sev, int s, int e, const char *sum,
                      const char *det) {
        AnomalyFinding f;
        f.severity = sev;
        f.startLine = s;
        f.endLine = e;
        f.summary = sum;
        f.details = det;
        return f;
    };

    // 1) 逐行同因 → 合并成一条区间
    {
        const QVector<AnomalyFinding> in{
            F("high", 33, 33, "认证失败尝试记录", "d1"),
            F("high", 34, 34, "认证失败尝试记录", "d2"),
            F("high", 35, 35, "认证失败尝试记录", "d3"),
            F("high", 36, 36, "认证失败尝试记录", "d4")};
        const auto out = ResultParser::mergeAdjacent(in);
        CHECK_EQ(out.size(), 1);
        if (out.size() == 1) {
            CHECK_EQ(out[0].startLine, 33);
            CHECK_EQ(out[0].endLine, 36);
        }
    }

    // 2) 摘要不同 → 即使行号相邻也不合并
    {
        const QVector<AnomalyFinding> in{
            F("high", 16, 16, "连接池耗尽", "a"),
            F("high", 17, 17, "HTTP 超时", "b")};
        CHECK_EQ(ResultParser::mergeAdjacent(in).size(), 2);
    }

    // 3) 摘要相同但中间隔了行（21 与 23）→ 不合并
    {
        const QVector<AnomalyFinding> in{
            F("high", 21, 21, "HTTP请求超时", "a"),
            F("high", 23, 23, "HTTP请求超时", "b")};
        CHECK_EQ(ResultParser::mergeAdjacent(in).size(), 2);
    }

    // 4) 区间重叠 → 合并，终点评取较大者，details 留更长的
    {
        const QVector<AnomalyFinding> in{
            F("high", 10, 14, "磁盘写入失败", "短"),
            F("high", 12, 18, "磁盘写入失败", "这一条的解释更长一些")};
        const auto out = ResultParser::mergeAdjacent(in);
        CHECK_EQ(out.size(), 1);
        if (out.size() == 1) {
            CHECK_EQ(out[0].startLine, 10);
            CHECK_EQ(out[0].endLine, 18);
            CHECK_EQ(out[0].details, QString("这一条的解释更长一些"));
        }
    }

    // 5) 乱序输入 → 先排序再合并，结果不受输入顺序影响
    {
        const QVector<AnomalyFinding> in{
            F("high", 36, 36, "认证失败尝试记录", "d"),
            F("high", 33, 33, "认证失败尝试记录", "a"),
            F("high", 35, 35, "认证失败尝试记录", "c"),
            F("high", 34, 34, "认证失败尝试记录", "b")};
        const auto out = ResultParser::mergeAdjacent(in);
        CHECK_EQ(out.size(), 1);
        if (out.size() == 1) {
            CHECK_EQ(out[0].startLine, 33);
            CHECK_EQ(out[0].endLine, 36);
        }
    }

    // 6) 空输入 / 单条 → 原样返回
    {
        CHECK_EQ(ResultParser::mergeAdjacent({}).size(), 0);
        const QVector<AnomalyFinding> one{F("low", 5, 5, "单条", "d")};
        const auto out = ResultParser::mergeAdjacent(one);
        CHECK_EQ(out.size(), 1);
        if (out.size() == 1) {
            CHECK_EQ(out[0].startLine, 5);
            CHECK_EQ(out[0].endLine, 5);
        }
    }
}

} // namespace

int main()
{
    qInfo().noquote() << "== aii unit tests ==";

    testLogParserLevel();
    testLogParserDecode();
    testLogParserFile();
    testLogChunkerBasics();
    testLogChunkerOverlap();
    testResultParser();
    testResultParserMerge();

    if (g_failures == 0) {
        qInfo().noquote() << QString("== 全部通过：%1 项断言 ==").arg(g_checks);
        return 0;
    }
    qCritical().noquote() << QString("== 失败：%1 / %2 项断言 ==").arg(g_failures).arg(g_checks);
    return 1;
}
