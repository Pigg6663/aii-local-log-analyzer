# aii — 本地 AI 日志分析工具

![C++](https://img.shields.io/badge/C%2B%2B-14-00599C?logo=cplusplus&logoColor=white)
![Qt](https://img.shields.io/badge/Qt-5.14.2-41CD52?logo=qt&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-%E2%89%A5%203.5-064F8C?logo=cmake&logoColor=white)
![Platform](https://img.shields.io/badge/platform-Windows%20%2F%20MinGW%207.3-0078D4)
![License](https://img.shields.io/badge/license-MIT-green)

拖入一个日志文件，程序解析后调用**本地大模型**（llama.cpp 的 llama-server）做异常检测与总体摘要。全程离线，数据不出本机。这是一个 1-2 个月实习项目，核心是打通「Qt 桌面 UI → 长日志分块 → 本地 LLM 逐块检测 → 结构化结果跳转回日志」的完整闭环。

> 开发状态：功能已完成，端到端可跑（`server_test` 控制台闭环 + GUI 手动验证）。设计细节见 [docs/DESIGN.md](docs/DESIGN.md)。

## 效果示例

仓库自带一段含典型故障的示例日志 [`samples/demo.log`](samples/demo.log)（56 行，模拟一个订单服务的运行记录）。拖进程序跑一次，得到：

**输入节选**

```
16  2026-03-15 10:06:44.902 [ERROR] [db-pool] connection pool exhausted active=50 waiting=17 maxWait=3000ms
26  2026-03-15 10:07:01.556 [ERROR] [db] deadlock found when trying to get lock; try restarting transaction
31  2026-03-15 10:07:41.884 [ERROR] [disk] write failed ... errno=28 No space left on device
37  2026-03-15 10:07:57.229 [ERROR] [auth] account locked after 5 failed attempts user=1042 ip=10.22.8.31
39  2026-03-15 10:08:14.902 [ERROR] [order] null pointer dereference in OrderService.calculateTotal
```

**输出（检测结果列表）**

| 行号 | 严重度 | 类型 | 说明 |
|---|---|---|---|
| 16 | 🔴 高 | 资源耗尽 | 数据库连接池打满（50/50，排队 17），已导致后续订单接口连续 500 |
| 26 | 🟡 中 | 并发冲突 | 行锁死锁，与 24-25 行的长事务（91s/88s）时间吻合 |
| 31 | 🔴 高 | 磁盘故障 | `errno=28 No space left on device`，分片上传中断 |
| 37 | 🟡 中 | 安全 | 同一 IP 连续 5 次登录失败后锁定账户，疑似撞库 |
| 39 | 🔴 高 | 空指针 | `OrderService.calculateTotal` 空指针，栈显示由 `/api/orders` 触发 |

双击任意一行 → 日志视图跳转到对应行，并高亮整个起止范围。

> 检测结果由本地模型生成，用词与严重度会随模型不同而变化；上表的「说明」列为示意，关键在于**行号能对上原文**。

## 功能特性

- **拖放即用**：拖入 `.log` / `.txt` 自动解码显示（BOM → UTF-8 → GB18030 → GBK 逐级探测，中文日志不乱码）
- **本地 LLM 异常检测**：长日志按 token 预算分块（相邻块重叠 8 行），逐块调 llama-server 检测异常，再 map-reduce 汇总成总体报告
- **行号可追溯**：检测结果以列表呈现（严重度着色），**双击任意一条 → 日志视图跳到对应行并高亮整个起止行范围**
- **规则兜底**：即使 LLM 全部失败，也会输出纯规则的 ERROR/WARN 统计 + 错误关键词报告
- **报告导出**：Markdown 报告可导出为 `.md`（原文）或 `.html`（内置转换渲染）
- **按需启动服务**：llama-server 只在点"启动推理服务"或"开始分析"时才拉起；分析全程 UI 不卡顿（QThread）
- **端口预检与复用**：启动前探测端口——已有健康的 llama-server 直接复用（省一次模型加载）；被其他程序占用则弹窗提示改端口
- **设置页**：模型路径、端口、线程数等经 QSettings 持久化

## 技术栈

| 层 | 技术 |
|---|---|
| UI / 框架 | Qt 5.14.2（Widgets + Network），QMainWindow |
| 语言 / 构建 | C++14（原模板 C++11 已升级），CMake ≥3.5，MinGW 7.3.0（GCC 7.3） |
| 推理后端 | [llama.cpp](https://github.com/ggml-org/llama.cpp) 预编译 `llama-server.exe`（子进程 + OpenAI 兼容 HTTP API，SSE 流式） |
| 模型 | GGUF 格式；开发调试用 Qwen3-1.7B Q4_K_M，实际使用建议 7-8B Q4_K_M（CPU 推理） |

## 技术决策

| 决策点 | 选择 | 理由 |
|---|---|---|
| 推理接入 | 子进程 + OpenAI 兼容 HTTP（Route B） | MinGW 7.3.0（GCC 7.3）编译不了要求 C++17+/GCC 13.3+ 的现代 llama.cpp；子进程还能把「模型崩溃」与「主程序」隔离，升级 exe 不用改代码 |
| 长日志切分 | 按 token 预算分块，相邻块重叠 8 行 | 上下文窗口有限；重叠是为了让恰好跨越块边界的异常不被切断而漏报 |
| 多块汇总 | map-reduce 两级（逐块检测 → 汇总报告） | 全部块塞不进一次请求；两级汇总既保住逐块细节，又产出全局视角 |
| 并发模型 | worker-object + `moveToThread`（不继承 QThread） | 线程与业务对象解耦；代价是对象不能有 parent、`run()` 里不跑槽函数 |
| 服务生命周期 | 端口预检 + `/health` 轮询 500ms | 复用已在跑的实例省一次模型加载；避免端口冲突时静默失败 |
| 模型输出容错 | 三段：掐头去尾 → 兼容 `[...]` / `{"findings":[...]}` 两种形态 → 逐条字段归一，失败返回 Retry 触发重试 | 小模型不保证输出合法 JSON，管线不能因此整条中断 |
| 结果可追溯 | finding 带起止行号，双击跳转并高亮整段 | 「检测到异常」不够，运维要能一秒看到原始上下文 |

> 更完整的取舍与放弃的替代方案见 [docs/DESIGN.md](docs/DESIGN.md) 第 7 节。

## 架构总览

```
用户拖入 .log/.txt
      │
      ▼
┌──────────────────────────────────────────────────────────┐
│  MainWindow（UI 线程）                                     │
│  日志视图（≤2MB 显示）│ 检测结果列表（双击跳行高亮）          │
│  分析报告页 │ 运行日志页 │ 设置页 │ 导出                    │
└──────┬──────────────────────────────┬───────────────────┘
       │ queued 信号                   │ 直连 start()/stop()
       ▼                              ▼
┌─────────────────────┐   ┌──────────────────────────────┐
│ AnalysisWorker      │   │ LlamaServerManager（UI 线程） │
│ （Worker 线程）      │   │ QProcess 生命周期 + 状态机     │
│  LogParser 解析      │   │ /health 就绪轮询（500ms）      │
│  LogChunker 分块     │   └──────────────┬───────────────┘
│  PromptBuilder 提示词│                  │ QProcess 拉起
│  LlamaHttpClient HTTP│◄───HTTP POST /v1/chat/completions（SSE）
│  ResultParser 解析   │──► llama-server 子进程（CPU 推理）
└─────────────────────┘
```

## 目录结构

```
aii/
├── CMakeLists.txt            # 主程序 aii + 控制台测试 server_test（W1 闭环）
├── README.md
├── LICENSE                   # MIT
├── samples/
│   └── demo.log              # 示例日志（模拟订单服务，含典型故障，可直接试跑）
├── docs/
│   └── DESIGN.md             # 设计文档：类职责/数据流/线程模型/进程生命周期
├── main.cpp                  # qRegisterMetaType + QSettings 归属
├── mainwindow.{h,cpp,ui}     # UI：拖放、结果列表、报告页、设置、导出、服务门控
├── analysisworker.{h,cpp}    # 管线编排（Worker 线程）：解析→分块→逐块检测→汇总
├── llamaservermanager.{h,cpp}# llama-server 子进程生命周期管理
├── llamahttpclient.{h,cpp}   # OpenAI 兼容 HTTP 客户端（SSE 流式）
├── logparser.{h,cpp}         # 编码探测 + 行切分 + 时间戳/级别 + 规则统计
├── logchunker.{h,cpp}        # 按 token 预算分块（行边界 + 重叠）
├── promptbuilder.{h,cpp}     # 系统/块检测/重试/汇总提示词 + 规则兜底报告
├── resultparser.{h,cpp}      # 模型 JSON 输出清洗与解析
├── settingsdialog.{h,cpp}    # 设置页（QSettings 持久化）
├── datatypes.h               # LogEntry / LogChunk / AnomalyFinding
└── server_test.cpp           # W1 控制台闭环测试（仅依赖 QtCore+QtNetwork）
```

## 项目规模

| 指标 | 数值 |
|---|---|
| C++ 源文件 | 21 个（11 个 `.cpp` + 10 个 `.h`）+ 1 个 `.ui` |
| 代码行数 | 约 2,600 行（非空约 2,200 行） |
| 功能模块 | 9 个：UI / 管线编排 / 服务管理 / HTTP 客户端 / 日志解析 / 分块 / 提示词构建 / 结果解析 / 设置 |
| 第三方依赖 | 无，只依赖 Qt 5.14（Widgets + Network）与标准库 |
| 验证方式 | `server_test` 控制台闭环（不依赖 GUI）+ [`samples/demo.log`](samples/demo.log) 手动回归 |

## 构建与运行

**环境要求**

- Windows + Qt 5.14.2（MinGW 7.3.0 套件），CMake ≥3.5
- `llama-server.exe`（可 `winget install ggml.llamacpp` 获取）
- 一个 GGUF 模型，如 Qwen3-1.7B-Instruct Q4_K_M（下载后放到如 `D:/models/`）

**命令行构建**

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=D:/Qt/5.14.2/mingw73_64
cmake --build build
```

**Qt Creator 构建**

打开 `CMakeLists.txt` → 选择 Desktop Qt 5.14.2 MinGW 7.3.0 64-bit 套件 → 构建运行 `aii`。

**首次运行**

1. 启动 `aii`，菜单 **设置 → 设置…**，填入 `llama-server.exe` 路径与 GGUF 模型路径（其余用默认值即可）
2. 拖入一个日志文件，点 **开始分析**——程序会自动拉起 llama-server，模型加载就绪后自动开始分析（CPU 推理，加载与逐块检测需要耐心）

## 已知限制

- **平台/工具链**：仅在 Windows + MinGW 7.3.0 验证；受工具链限制无法直接编译现代 llama.cpp，依赖预编译 `llama-server.exe`
- **日志格式**：仅支持行式文本 `.log`/`.txt`；时间戳/级别靠有限正则识别，特殊格式可能解析为 unknown；>200MB 文件拒绝分析
- **显示上限**：日志视图只显示前 2MB（分析本身使用完整文件），超出部分的 finding 无法跳转显示
- **性能**：CPU 推理（`-ngl 0`），大模型单块检测可能耗时数十秒；超大日志按 100 块均匀采样（报告会如实标注）
- **检测质量**：取决于本地模型能力；LLM 失败时自动降级为规则统计报告（有产出但无模型级分析）
- **端口冲突**：默认 8080 被占用（包括残留的孤儿 llama-server 进程）时服务启动失败，需 `tasklist | findstr llama` 后手动结束
- **设置生效时机**：服务运行中修改端口/线程数/上下文大小，需重启推理服务后生效
- **HTML 导出**：使用 Qt 内置 Markdown 转换，样式为基础渲染

## 延伸阅读

- [docs/DESIGN.md](docs/DESIGN.md) — 类职责、数据流、线程模型、llama-server 进程生命周期、关键设计决策与默认阈值速查表

## 许可证

[MIT](LICENSE) © 2026 周文浩
