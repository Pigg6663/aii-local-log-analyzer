# 设计文档：本地 AI 日志分析工具（aii）

> 版本：1.0（2026-09-12）　状态：W1-W4 已实现，W5+ 收尾中

## 1. 目标与约束

**目标**：用户拖入一个日志文件，程序解析后调用**本地大模型**（LLM）做异常检测与总体摘要，全程离线、数据不出本机。

**硬约束**：开发工具链为 Qt 5.14.2 + MinGW 7.3.0（GCC 7.3）+ CMake。现代 llama.cpp 主分支要求 C++17+（新提交含 C++20 语法）与 GCC 13.3+，**无法在本工具链下编译**。因此集成方案定为 **Route B：llama-server 子进程 + OpenAI 兼容 HTTP API**——用 QProcess 拉起预编译的 `llama-server.exe`，通过 `QNetworkAccessManager` 走 HTTP + SSE 流式接口，不直接链接 llama.cpp 的 C API。

**范围决策**（实习项目 1-2 个月）：
- 保留亮点：拖拽 + 流式 UI 闭环、长日志分块 + map-reduce（块级检测 → 全局摘要）、JSON 结构化输出 + 行号跳转回日志、QThread 非阻塞 UI、报告导出、纯规则统计兜底。
- 砍掉：花哨日志查看器、多格式日志解析（只做行式文本 + 时间戳正则）、实时 tail、模型下载管理、RAG/向量库、多文件批量、GPU 加速。

## 2. 总体架构

```
┌────────────────────────────────────────────────────────────────┐
│                     MainWindow（UI 线程）                        │
│  拖放 → 日志视图（≤2MB 显示）│ 检测结果列表（双击跳行高亮）        │
│  分析报告页（Markdown）│ 运行日志页 │ 设置页（QSettings）│ 导出    │
└───────┬───────────────────────────────┬────────────────────────┘
        │ queued 信号                    │ 直连（同线程）
        │ analyzeRequested(filePath,    │ start()/stop()
        │   serverBaseUrl)              │ serverReady/serverFailed
        ▼                               ▼
┌────────────────────────┐   ┌────────────────────────────────┐
│   AnalysisWorker       │   │   LlamaServerManager（UI 线程）  │
│   （Worker 线程）       │   │   QProcess 生命周期 + 状态机     │
│                        │   │   /health 就绪轮询（500ms）      │
│  LogParser（解析）      │   └──────────────┬─────────────────┘
│  LogChunker（分块）     │                  │ QProcess 拉起
│  PromptBuilder（提示词）│                  ▼
│  LlamaHttpClient（HTTP）│◄──────HTTP POST /v1/chat/completions（SSE）
│  ResultParser（JSON）   │───►  llama-server 子进程（CPU 推理）
└────────┬───────────────┘
         │ queued 信号：progressChanged / findingFound / finished / failed
         ▼
        MainWindow（UI 线程）：进度条、结果逐条上屏、报告渲染、可导出
```

## 3. 核心类职责

| 类 | 所在线程 | 职责 |
|---|---|---|
| `LogParser` | Worker（静态函数） | 读文件（上限 200MB）→ 编码探测解码 → 按行切分（保留原始行号）→ 正则提取时间戳/级别 → 规则统计（ERROR/WARN 计数、中英文错误关键词计数）。编码探测链：BOM（UTF-8/UTF-16）→ UTF-8 严格校验（`QTextCodec::ConverterState`）→ GB18030 → GBK → Latin-1。空行跳过但行号不变，保证行号可追溯。 |
| `LogChunker` | Worker（静态函数） | 按 token 预算切块：1 token ≈ 2 字符，预算 = `(ctxSize − 1024) × 0.8`（下限 512）；只在行边界切，相邻块重叠 8 行；块文本格式 `行号\|内容` 喂给 LLM。 |
| `PromptBuilder` | Worker（静态函数） | 三套提示词模板：① 系统提示（中文角色 + JSON schema：severity/start_line/end_line/summary/details，宁缺毋滥）；② 块检测提示；③ 重试提示（**必须重带块文本**，否则模型丢失上下文）；④ 汇总提示（top-40 条 finding 按严重度排序 + top-8 关键词）；⑤ 规则兜底报告（LLM 全挂时仍有产出）。 |
| `LlamaServerManager` | UI | llama-server 进程生命周期：按需启动、**启动前端口预检（W5）**——QTcpSocket 探测：连接被拒=端口空闲→正常拉起；连接成功→GET `/health` 200=已有健康 llama-server，直接复用（`m_ownsProcess=false`，`stop()` 只断开不终止它）；非 200/超时=明确报错引导改端口。其余：`/health` 就绪轮询（500ms，仅 HTTP 200 视为就绪）、180s 加载超时、状态机（Idle/Starting/Loading/Ready/Stopping/Failed/Crashed）、优雅关闭（terminate → 5s → kill）、stderr 逐行捕获、孤儿进程感知。启动参数：`-m <模型> --host 127.0.0.1 --port <端口> --ctx-size <N> --threads <N> -ngl 0`（CPU 推理）。 |
| `LlamaHttpClient` | Worker（惰性创建） | OpenAI 兼容 `/v1/chat/completions`（stream:true，SSE 逐行解析 `data:` / `[DONE]`）；QEventLoop 阻塞式同步 `chat()`（只允许在 worker 线程调用）；180s 超时（QTimer + abort）、`abortCurrent()` 支持取消；`response_format: json_object`；信号 tokenReceived/responseCompleted。**必须惰性创建于 worker 线程**以保证 QNAM 线程亲和性。 |
| `ResultParser` | Worker（静态函数） | 清洗模型输出：剥 ```json 围栏 → 掐头去尾找 `{...}` → 容忍尾逗号 → QJsonDocument 解析；接受数组/`{"findings":[...]}`/单对象；丢弃无 summary 的条目、严重度归一化、容忍字符串行号、交换 start>end；返回 Ok（可汇总）或 Retry（触发重试）。 |
| `AnalysisWorker` | Worker | 管线编排：解析 → 分块 → 逐块"提示词 → HTTP → 解析"，失败重试 ≤3 次（仍失败生成"该块 LLM 输出不可解析"低严重度条目）→ 汇总报告；**100 块上限**（均匀采样，超大日志不失控）；`cancel()` 置标志 + abort 当前请求；信号 progressChanged/findingFound/finished/failed。 |
| `MainWindow` | UI | 拖放过滤（.log/.txt）→ 日志视图解码显示（上限 2MB，分析用完整文件）；按需启动门控（`m_pendingAnalysis` + `onServerReady` 兑现）；创建 worker 线程与全部 queued 连接；检测结果列表（双击跳行 + 整行高亮）；报告 Markdown 渲染（`QTextBrowser::setMarkdown`，Qt 5.14 原生）；导出 .md/.html；设置页读写 QSettings；退出清理。 |
| `SettingsDialog` | UI | 设置页：llama-server 路径、模型路径（带浏览按钮）、端口、线程数、上下文大小；QSettings 持久化（键前缀 `server/`，Windows 下 `HKCU\Software\aii\...`）。 |

## 4. 数据流（一次完整分析）

```
用户拖入 .log/.txt
  │ dropEvent（UI 线程）
  ▼
MainWindow：日志视图显示（≤2MB，与 worker 同一套编码探测）
  │
  │ 点"开始分析"
  │ ① 服务未就绪 → LlamaServerManager::start()（按需启动）
  │ ② 等 serverReady → beginAnalysis()
  │ ③ emit analyzeRequested(filePath, apiBaseUrl)      [queued]
  ▼
AnalysisWorker::analyze（Worker 线程）
  │
  ├─1. LogParser::parseFile ──► QVector<LogEntry> + Stats        （进度 0-5）
  ├─2. LogChunker::chunk ──► QVector<LogChunk>                   （进度 5）
  │
  ├─3. 逐块循环（进度 10-85，每块实时 emit findingFound）：
  │     PromptBuilder::chunkDetectionPrompt(chunk)
  │       └─► LlamaHttpClient::chat ──► POST /v1/chat/completions（SSE 流式）
  │     ResultParser::parseFindings
  │       ├─ Ok     ──► 收集 finding，UI 结果列表逐条上屏
  │       └─ Retry  ──► retryPrompt(chunk, 上次坏输出) ──► 重试 ≤3 次
  │            仍失败 ──► 生成低严重度"该块 LLM 输出不可解析"
  │
  ├─4. PromptBuilder::summaryPrompt(findings, stats, …)          （进度 90）
  │       └─► chat ──► 总体报告（Markdown）
  │   汇总失败 ──► ruleFallbackReport(stats)（规则兜底，保证有产出）
  │
  └─5. emit finished(findings, report)                           [queued]
  ▼
MainWindow（UI 线程）：
  检测结果列表（严重度着色）＋ 分析报告页（Markdown 渲染）
  → 双击某条 finding → 日志视图跳到对应行 + 整行高亮（ExtraSelection）
  → 工具栏"导出报告" → .md 直写 / .html（QTextDocument::setMarkdown 转换）
```

**取消链路**：点"取消分析" → `cancelRequested()` [queued] → worker `cancel()` 置标志 + `abortCurrent()` 中断当前 HTTP → 块循环退出 → `finished`（或 `failed`）回到 UI 复位按钮。

## 5. 线程模型

| 线程 | 内容 |
|---|---|
| UI 线程 | MainWindow、全部控件、SettingsDialog、LlamaServerManager（QProcess + QTimer） |
| Worker 线程 | AnalysisWorker 及其惰性创建的 LlamaHttpClient；LogParser/LogChunker/PromptBuilder/ResultParser 均为静态函数，随调用在 worker 线程执行 |

**规则**（全部已验证）：
1. **worker-object 模式**：不继承 QThread；worker 无 parent，`moveToThread(thread)` 后经事件循环调度，`QThread::finished → deleteLater` 回收。有 parent 的对象 moveToThread 会直接失败。
2. 跨线程通信只用 **queued 信号槽**（Qt 5.14 新式语法 `&Sender::sig` / `&Receiver::slot`）；自定义类型（`AnomalyFinding`、`QVector<AnomalyFinding>`）在 `main()` 里 `qRegisterMetaType`，否则连接建立但槽永远收不到。
3. UI 只向 worker 发 `analyzeRequested(filePath, baseUrl)` 与 `cancelRequested()` 两个信号——**worker 完全不感知进程管理**；"按需启动 + 就绪后再分析"的门控全部在 MainWindow 完成（`m_pendingAnalysis` 挂起 → `onServerReady` 兑现）。
4. QNetworkAccessManager **必须创建在 worker 线程内**（`analyze()` 里惰性 new），QEventLoop 阻塞式 `chat()` 只允许出现在 worker 线程。
5. 验证手段：关键路径打印线程指针（`[ui]` vs `[worker]`），对比即可确认 queued 调度与 metatype 注册生效。

## 6. llama-server 进程生命周期

```
                    用户点"启动推理服务"
                    或点"开始分析"且服务未就绪
                              │
                              ▼
      ┌──── 启动 QProcess（-m 模型 --port … -ngl 0）────┐
      │  State: Idle → Starting → Loading               │
      │  每 500ms 轮询 GET /health（单次在途保护）        │
      │     ├─ 200 ───────► Ready（emit serverReady）   │
      │     ├─ 503/拒绝 ──► 继续（模型加载中）            │
      │     └─ 超 180s ───► Failed（emit serverFailed） │
      └────────────────────────────────────────────────┘

异常退出：QProcess finished 且退出码 ≠ 0（或运行中消失）
      → State: Crashed → UI 状态栏显示"已崩溃"

优雅关闭：应用 closeEvent →
      ① emit cancelRequested（worker 尽快停下）
      ② thread->quit() + wait(5000)
      ③ server->stop()：terminate → 等 5s → 仍活着则 kill
```

**关键约定**：
- **构造函数永不 start()**。按需启动只有两个入口：工具栏"启动推理服务"按钮、点"开始分析"时服务未就绪。状态栏服务未启动时必须显示"推理服务：未启动"而非空白。
- **孤儿进程**：Qt Creator 的停止按钮直接杀应用，不走 closeEvent，QProcess 析构不会执行 → 残留的 llama-server 会一直占着端口。下次启动时表现为"就绪前退出（exit code 1）"= 端口被占。处理：文档化 + 提示用户 `tasklist | findstr llama` 后 taskkill（端口占用预检列入 W5 候选）。

## 7. 关键设计决策

1. **Route B（子进程 + HTTP）**：见第 1 节约束。附带收益：解耦 llama.cpp 版本（升级 exe 不动代码）、服务可被外部工具复用、崩溃不带走宿主进程。
2. **重试提示词必须重带块文本**：初版设计只把"上次坏输出"发给模型，模型会丢失日志块上下文无法重析；`retryPrompt(chunk, lastFailedOutput)` 同时包含块文本与截断到 1500 字符的坏输出。
3. **100 块均匀采样上限**（`kMaxChunks=100`）：超大日志（>200 万行量级）在 CPU 推理下不可能逐块全检，按索引均匀采样保证覆盖度，报告里如实标注"共 N 块，采样 M 块"。
4. **规则兜底**：纯正则的 ERROR/WARN 计数 + 中英文错误关键词统计永远可算；LLM 汇总失败时用它生成报告——**任何情况下用户都有产出**，不因模型故障全盘失败。
5. **行号贯穿全程**：解析保留原始行号、块文本带 `行号|内容`、JSON schema 要求 start_line/end_line、UI 列表跳转高亮——"模型说了哪一行，双击就能看到哪一行"是核心可验证亮点。
6. **UI 线程 IO 有界**：日志显示只读前 2MB（分析用完整文件），大文件不冻结界面。
7. **编码探测链兜底**：UTF-8 严格校验失败后逐级降级 GB18030 → GBK → Latin-1（Latin-1 永不失败），中文日志不出现乱码。
8. **Qt 5.14 原生 Markdown**：`QTextDocument::setMarkdown` 同时承担报告页渲染与 .html 导出转换，不引第三方库。

## 8. 落地顺序

| 里程碑 | 内容 | 状态 |
|---|---|---|
| W1 | 工具链验证：LlamaServerManager + LlamaHttpClient + `server_test` 控制台闭环（启动 → /health 就绪 → 一次 chat → 完整响应） | ✅ 已验证 |
| W2 | MainWindow 骨架 + AnalysisWorker 空壳（假信号），验证线程边界、queued 连接、qRegisterMetaType、按需启动 | ✅ 已验证 |
| W3 | 真实管线：LogParser/LogChunker/PromptBuilder/ResultParser + 真 AnalysisWorker（分块 map-reduce、重试、兜底） | ✅ 已验证 |
| W4 | 设置页（QSettings）、报告导出（.md/.html）、检测结果列表双击跳行高亮、日志视图 | ✅ 已实现（最终验证中） |
| W5+（候选） | 端口占用预检（启动前先探测 8080 并给出"杀孤儿进程"指引）、日志视图行号边栏、导出 PDF、分析历史记录、README 截图替换 | 待定 |

## 9. 附录：默认配置与阈值速查

| 项 | 值 |
|---|---|
| 服务地址 | `http://127.0.0.1:8080` |
| 就绪轮询 / 加载超时 | 500ms / 180s |
| HTTP 超时 / 温度 / 最大 token | 180s / 0.2 / 512 |
| 分块预算 | `(ctxSize − 1024) × 0.8`，下限 512 |
| 块重叠 / 块数上限 / 单块重试 | 8 行 / 100 / 3 次 |
| 文件上限 / 显示上限 | 200MB / 2MB |
| 推理参数 | `-ngl 0`（CPU） |
