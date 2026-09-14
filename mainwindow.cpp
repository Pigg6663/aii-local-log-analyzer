#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include "analysisworker.h"
#include "logparser.h"
#include "settingsdialog.h"

#include <QAction>
#include <QCloseEvent>
#include <QColor>
#include <QDebug>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle("AI 日志分析工具");
    setAcceptDrops(true);
    resize(1000, 700);

    // ★ 线程边界验证点：主线程指针基准值，与 worker 的打印对比
    qInfo().noquote() << "[ui] MainWindow 构造于线程" << QThread::currentThread();

    buildUi();
    buildToolbar();
    buildMenus();

    // ---- 推理服务管理器（UI 线程；配置来自 QSettings，见设置对话框）----
    // 注意：构造函数只做配置和连接，【不调用 start()】。
    // 按需启动的两个入口：① 工具栏"启动推理服务"按钮（onServerToggled）
    //                      ② "开始分析"且服务未就绪（onAnalyzeClicked）
    m_server = new LlamaServerManager(this);
    const ServerSettings cfg = SettingsDialog::loadSettings();
    m_server->setServerBinary(cfg.binaryPath);
    m_server->setModelPath(cfg.modelPath);
    m_server->setPort(cfg.port);
    m_server->setThreads(cfg.threads);
    m_server->setContextSize(cfg.contextSize);
    if (cfg.binaryPath.isEmpty() || cfg.modelPath.isEmpty())
        m_runLog->appendPlainText("提示：尚未配置 llama-server 路径，请通过菜单【设置 → 设置…】完成配置。\n");

    connect(m_server, &LlamaServerManager::stateChanged, this, &MainWindow::onServerStateChanged);
    connect(m_server, &LlamaServerManager::serverReady, this, &MainWindow::onServerReady);
    connect(m_server, &LlamaServerManager::serverFailed, this, &MainWindow::onServerFailed);
    connect(m_server, &LlamaServerManager::serverLog,
            [](const QString &line) { qInfo().noquote() << "[server]" << line; });
    onServerStateChanged(LlamaServerManager::State::Idle, "未启动");

    // ---- Worker 线程（设计第 3 节：worker-object 模式）----
    m_thread = new QThread(this);
    m_worker = new AnalysisWorker;              // 无 parent：有 parent 的对象 moveToThread 会失败
    m_worker->moveToThread(m_thread);
    m_thread->start();

    // UI → Worker（跨线程，自动变 Queued）
    connect(this, &MainWindow::analyzeRequested, m_worker, &AnalysisWorker::analyze);
    connect(this, &MainWindow::cancelRequested,  m_worker, &AnalysisWorker::cancel);
    // Worker → UI（跨线程，自动变 Queued）
    connect(m_worker, &AnalysisWorker::progressChanged, this, &MainWindow::onProgress);
    connect(m_worker, &AnalysisWorker::findingFound,    this, &MainWindow::onFindingFound);
    connect(m_worker, &AnalysisWorker::finished,        this, &MainWindow::onFinished);
    connect(m_worker, &AnalysisWorker::failed,          this, &MainWindow::onFailed);
    // 退出清理：线程事件循环结束后，在 worker 线程内安全删除 worker 对象
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
}

MainWindow::~MainWindow()
{
    // 兜底：正常路径 closeEvent 已处理；这里防"窗口未关闭直接析构"的情况
    if (m_thread->isRunning()) {
        m_thread->quit();
        m_thread->wait(5000);
    }
    delete ui;
}

// ---- UI 构建 ----

void MainWindow::buildUi()
{
    auto *central = new QFrame(this);
    central->setStyleSheet(
        "QFrame { border: 2px dashed #999999; border-radius: 8px; background: #f8f8f8; }");

    auto *v = new QVBoxLayout(central);

    m_dropLabel = new QLabel("将 .log / .txt 文件拖到这里", central);
    m_dropLabel->setAlignment(Qt::AlignCenter);
    m_dropLabel->setStyleSheet("color: #666666; font-size: 15px; border: none; background: transparent;");
    v->addWidget(m_dropLabel);

    // 日志视图：跳转 + 高亮的目标（只读）
    m_logView = new QPlainTextEdit(central);
    m_logView->setReadOnly(true);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logView->setPlaceholderText("日志内容将显示在这里（拖入文件后）");
    m_logView->setStyleSheet("border: 1px solid #cccccc; border-radius: 4px; background: #ffffff;");

    // 三个页签：检测结果 / 分析报告 / 运行日志
    m_tabs = new QTabWidget(central);
    m_tabs->setStyleSheet("border: 1px solid #cccccc; border-radius: 4px; background: #ffffff;");

    m_findingsTable = new QTableWidget(m_tabs);
    m_findingsTable->setColumnCount(3);
    m_findingsTable->setHorizontalHeaderLabels({"严重度", "行号", "摘要"});
    m_findingsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_findingsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_findingsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_findingsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_findingsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_findingsTable->setAlternatingRowColors(true);
    m_findingsTable->verticalHeader()->setVisible(false);
    connect(m_findingsTable, &QTableWidget::cellDoubleClicked, this, &MainWindow::onFindingJumped);
    m_tabs->addTab(m_findingsTable, "检测结果");

    m_reportView = new QTextBrowser(m_tabs);
    m_reportView->setOpenExternalLinks(true);
    m_tabs->addTab(m_reportView, "分析报告");

    m_runLog = new QPlainTextEdit(m_tabs);
    m_runLog->setReadOnly(true);
    m_tabs->addTab(m_runLog, "运行日志");

    auto *splitter = new QSplitter(Qt::Vertical, central);
    splitter->addWidget(m_logView);
    splitter->addWidget(m_tabs);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setStyleSheet("border: none; background: transparent;");
    v->addWidget(splitter, 1);

    setCentralWidget(central);

    // 状态栏：左侧 = 分析进度，右侧永久区 = 服务状态
    m_progressLabel = new QLabel("就绪", this);
    statusBar()->addWidget(m_progressLabel);
    // 初始文案直接给上，确保任何情况下状态栏都不显示空白
    m_serverStatusLabel = new QLabel("推理服务：未启动", this);
    statusBar()->addPermanentWidget(m_serverStatusLabel);
}

void MainWindow::buildToolbar()
{
    auto *toolbar = addToolBar("主工具栏");
    toolbar->setMovable(false);
    m_analyzeAction = toolbar->addAction("开始分析");
    m_cancelAction  = toolbar->addAction("取消分析");
    toolbar->addSeparator();
    m_exportAction  = toolbar->addAction("导出报告");
    toolbar->addSeparator();
    m_serverAction  = toolbar->addAction("启动推理服务");
    m_analyzeAction->setEnabled(false);   // 拖入文件后才可用
    m_cancelAction->setEnabled(false);
    m_exportAction->setEnabled(false);    // 有报告后才可用
    connect(m_analyzeAction, &QAction::triggered, this, &MainWindow::onAnalyzeClicked);
    connect(m_cancelAction,  &QAction::triggered, this, &MainWindow::onCancelClicked);
    connect(m_exportAction,  &QAction::triggered, this, &MainWindow::onExportClicked);
    connect(m_serverAction,  &QAction::triggered, this, &MainWindow::onServerToggled);
}

void MainWindow::buildMenus()
{
    QMenu *menu = menuBar()->addMenu("设置(&S)");
    QAction *act = menu->addAction("设置…");
    act->setShortcut(QKeySequence("Ctrl+,"));
    connect(act, &QAction::triggered, this, &MainWindow::onSettingsClicked);
}

// ---- 拖放（只收单个 .log / .txt 文件）----

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.size() == 1) {
        const QString suffix = QFileInfo(urls.first().toLocalFile()).suffix().toLower();
        if (suffix == "log" || suffix == "txt") {
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    m_currentFile = event->mimeData()->urls().first().toLocalFile();
    loadLogForDisplay(m_currentFile);
    // 清掉上一次的分析产物
    m_findingsTable->setRowCount(0);
    m_reportView->clear();
    m_reportMarkdown.clear();
    m_exportAction->setEnabled(false);
    m_logView->setExtraSelections(QList<QTextEdit::ExtraSelection>());
    m_dropLabel->setText(QString("当前文件：%1（可再次拖入替换）").arg(m_currentFile));
    m_analyzeAction->setEnabled(!m_running);
    event->acceptProposedAction();
}

void MainWindow::loadLogForDisplay(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_logView->setPlainText("（无法打开文件用于显示）");
        return;
    }
    // 显示上限 2MB：UI 线程的 IO 必须有界；完整文件由 worker 线程分析
    QByteArray data;
    bool truncated = false;
    if (file.size() > 2 * 1024 * 1024) {
        data = file.read(2 * 1024 * 1024);
        truncated = true;
    } else {
        data = file.readAll();
    }
    m_logView->setPlainText(LogParser::detectAndDecode(data));
    if (truncated)
        statusBar()->showMessage("文件较大，仅显示前 2MB（分析使用完整文件）", 6000);
}

// ---- 按钮 / 菜单 ----

void MainWindow::onAnalyzeClicked()
{
    if (m_currentFile.isEmpty() || m_running)
        return;

    // 按需启动（设计约定）：点"开始分析"时若服务未就绪，自动拉起 llama-server
    const auto s = m_server->state();
    if (s != LlamaServerManager::State::Ready
        && s != LlamaServerManager::State::Starting
        && s != LlamaServerManager::State::Loading
        && s != LlamaServerManager::State::Stopping) {
        m_server->start();
    }

    if (m_server->isReady()) {
        beginAnalysis();
    } else {
        // 服务在加载中：等 serverReady 信号（onServerReady）再真正开始
        m_pendingAnalysis = true;
        m_runLog->appendPlainText("== 等待推理服务就绪（模型加载中）… ==\n");
        m_cancelAction->setEnabled(true);
        m_dropLabel->setText(QString("当前文件：%1\n（等待服务就绪…）").arg(m_currentFile));
    }
}

void MainWindow::onServerReady()
{
    // 服务就绪后兑现挂起的分析请求（手动启动服务时无挂起则忽略）
    if (m_pendingAnalysis && !m_running) {
        m_pendingAnalysis = false;
        beginAnalysis();
    }
}

void MainWindow::beginAnalysis()
{
    m_running = true;
    m_findingsTable->setRowCount(0);
    m_reportView->clear();
    m_reportMarkdown.clear();
    m_exportAction->setEnabled(false);
    m_logView->setExtraSelections(QList<QTextEdit::ExtraSelection>());
    m_runLog->appendPlainText("== 开始分析（真实管线）==\n");
    m_analyzeAction->setEnabled(false);
    m_cancelAction->setEnabled(true);
    m_dropLabel->setText(QString("当前文件：%1\n（分析中…）").arg(m_currentFile));
    emit analyzeRequested(m_currentFile, m_server->apiBaseUrl());   // 跨线程 queued
}

void MainWindow::onCancelClicked()
{
    m_cancelAction->setEnabled(false);
    if (m_pendingAnalysis) {
        // 还在等服务就绪：直接取消，不必发 worker 信号
        m_pendingAnalysis = false;
        m_runLog->appendPlainText("== 已取消（分析尚未开始）==\n");
        return;
    }
    m_runLog->appendPlainText("[ui] 已发送取消请求，等待 worker 响应…\n");
    emit cancelRequested();                 // 跨线程 queued
}

void MainWindow::onServerToggled()
{
    const auto s = m_server->state();
    if (s == LlamaServerManager::State::Starting || s == LlamaServerManager::State::Loading
        || s == LlamaServerManager::State::Ready || s == LlamaServerManager::State::Stopping)
        m_server->stop();
    else
        m_server->start();
}

// ---- 设置 / 导出 ----

void MainWindow::onSettingsClicked()
{
    SettingsDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const ServerSettings cfg = dlg.settings();
    SettingsDialog::saveSettings(cfg);
    m_server->setServerBinary(cfg.binaryPath);
    m_server->setModelPath(cfg.modelPath);
    m_server->setPort(cfg.port);
    m_server->setThreads(cfg.threads);
    m_server->setContextSize(cfg.contextSize);

    const auto s = m_server->state();
    const bool running = (s == LlamaServerManager::State::Starting
                          || s == LlamaServerManager::State::Loading
                          || s == LlamaServerManager::State::Ready);
    if (running)
        QMessageBox::information(this, "设置",
            "路径已更新；端口/线程数/上下文大小将在下次启动推理服务时生效。");
    m_runLog->appendPlainText("设置已保存。\n");
}

void MainWindow::onExportClicked()
{
    if (m_reportMarkdown.isEmpty())
        return;

    const QString base = QFileInfo(m_currentFile).completeBaseName();
    const QString defaultName = (base.isEmpty() ? QStringLiteral("log_report") : base + "_报告");
    QString path = QFileDialog::getSaveFileName(
        this, "导出报告", defaultName,
        "Markdown 文件 (*.md);;HTML 文件 (*.html)");
    if (path.isEmpty())
        return;

    QByteArray content;
    if (path.endsWith(".html", Qt::CaseInsensitive)) {
        // Qt 5.14 的 QTextDocument 原生支持 Markdown，直接转换
        QTextDocument doc;
        doc.setMarkdown(m_reportMarkdown);
        content = doc.toHtml().toUtf8();
    } else {
        if (!path.endsWith(".md", Qt::CaseInsensitive))
            path += ".md";
        content = m_reportMarkdown.toUtf8();
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "导出失败", "无法写入文件：" + f.errorString());
        return;
    }
    f.write(content);
    statusBar()->showMessage("报告已导出：" + path, 5000);
}

// ---- 结果列表跳转 ----

void MainWindow::onFindingJumped(int row, int /*column*/)
{
    const QTableWidgetItem *item = m_findingsTable->item(row, 1);
    if (!item)
        return;
    const int start = item->data(Qt::UserRole).toInt();
    const int end   = item->data(Qt::UserRole + 1).toInt();
    if (start <= 0) {
        statusBar()->showMessage("该结果没有可跳转的行号", 3000);
        return;
    }
    jumpToLine(start, end);
}

void MainWindow::jumpToLine(int startLine, int endLine)
{
    QTextDocument *doc = m_logView->document();
    const int blockCount = doc->blockCount();

    // 行号归一：end 无效或倒置时按单行处理（ResultParser 已做交换，这里是防御）
    if (endLine < startLine)
        endLine = startLine;

    if (startLine < 1 || startLine > blockCount) {
        statusBar()->showMessage(
            QString("第 %1 行超出显示范围（当前仅显示前 %2 行）")
                .arg(startLine).arg(blockCount), 5000);
        return;
    }
    if (endLine > blockCount) {
        // 日志视图有 2MB 显示上限，范围可能被截断：高亮到可见部分为止
        statusBar()->showMessage(
            QString("第 %1-%2 行超出显示范围，仅高亮到第 %3 行")
                .arg(startLine).arg(endLine).arg(blockCount), 5000);
        endLine = blockCount;
    }

    // 光标定到范围首行并滚动过去
    QTextCursor cursor(doc->findBlockByNumber(startLine - 1));
    m_logView->setTextCursor(cursor);
    m_logView->centerCursor();

    // 整段高亮：选区从首行开头拉到末行行尾（FullWidthSelection 让每一行都整行着色，
    // 单条 ExtraSelection，下一次跳转自动替换）
    QTextCursor rangeCursor(doc->findBlockByNumber(startLine - 1));
    rangeCursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor, endLine - startLine);
    rangeCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);

    QTextEdit::ExtraSelection sel;
    sel.format.setBackground(QColor(255, 235, 156));
    sel.format.setProperty(QTextFormat::FullWidthSelection, true);
    sel.cursor = rangeCursor;
    m_logView->setExtraSelections(QList<QTextEdit::ExtraSelection>() << sel);
}

// ---- 服务状态（UI 线程内直连）----

void MainWindow::onServerStateChanged(LlamaServerManager::State state, const QString &desc)
{
    m_serverStatusLabel->setText("推理服务：" + desc);
    const bool running = (state == LlamaServerManager::State::Starting
                          || state == LlamaServerManager::State::Loading
                          || state == LlamaServerManager::State::Ready
                          || state == LlamaServerManager::State::Stopping);
    m_serverAction->setText(running ? "停止推理服务" : "启动推理服务");
}

void MainWindow::onServerFailed(const QString &reason)
{
    m_runLog->appendPlainText("[server] 失败：" + reason + "\n");
    if (m_pendingAnalysis) {
        m_pendingAnalysis = false;
        m_cancelAction->setEnabled(false);
        m_dropLabel->setText(QString("当前文件：%1").arg(m_currentFile));
        m_runLog->appendPlainText("== 服务启动失败，分析未开始 ==\n");
    }
    // 启动失败会直接阻断用户操作（含端口预检失败）：弹窗明确告知，而不是只沉在运行日志页
    QMessageBox::warning(this, "推理服务启动失败", reason);
}

// ---- Worker → UI 的槽（全部经 queued 连接在主线程执行）----

void MainWindow::onProgress(int current, int total, const QString &stage)
{
    m_progressLabel->setText(QString("%1  %2/%3").arg(stage).arg(current).arg(total));
}

void MainWindow::onFindingFound(const AnomalyFinding &finding)
{
    // ★ 线程边界验证点：这行打印应显示【主线程】指针，
    //   与 analyze() 里的 worker 线程指针不同 → 自定义类型 queued 传递成功
    qInfo().noquote() << "[ui] onFindingFound 执行于线程" << QThread::currentThread();

    static const auto sevColor = [](const QString &sev) -> QColor {
        if (sev == "critical") return QColor(200, 0, 0);
        if (sev == "high")     return QColor(220, 90, 0);
        if (sev == "medium")   return QColor(160, 120, 0);
        return QColor(110, 110, 110);   // low
    };

    const int row = m_findingsTable->rowCount();
    m_findingsTable->insertRow(row);

    auto *sevItem = new QTableWidgetItem(finding.severity);
    sevItem->setForeground(sevColor(finding.severity));

    const QString lineText = finding.startLine > 0
        ? QString("%1-%2").arg(finding.startLine).arg(finding.endLine)
        : QString("?");
    auto *lineItem = new QTableWidgetItem(lineText);
    lineItem->setData(Qt::UserRole, finding.startLine);       // 双击跳转用
    lineItem->setData(Qt::UserRole + 1, finding.endLine);

    auto *sumItem = new QTableWidgetItem(finding.summary);
    sumItem->setToolTip(finding.details);

    m_findingsTable->setItem(row, 0, sevItem);
    m_findingsTable->setItem(row, 1, lineItem);
    m_findingsTable->setItem(row, 2, sumItem);

    m_runLog->appendPlainText(
        QString("收到检测结果: [%1] 第 %2-%3 行: %4\n")
            .arg(finding.severity).arg(finding.startLine).arg(finding.endLine).arg(finding.summary));
}

void MainWindow::onFinished(const QVector<AnomalyFinding> &findings, const QString &report)
{
    m_running = false;
    m_analyzeAction->setEnabled(!m_currentFile.isEmpty());
    m_cancelAction->setEnabled(false);
    m_progressLabel->setText("分析完成");
    m_dropLabel->setText(QString("当前文件：%1（共 %2 条检测结果）")
                         .arg(m_currentFile).arg(findings.size()));

    m_reportMarkdown = report;
    m_reportView->setMarkdown(report);       // Qt 5.14 原生 Markdown 渲染
    m_exportAction->setEnabled(true);
    m_runLog->appendPlainText(QString("== 分析完成，共 %1 条结果 ==\n").arg(findings.size()));
    m_tabs->setCurrentWidget(m_reportView);  // 完成后自动切到报告页
}

void MainWindow::onFailed(const QString &reason)
{
    m_running = false;
    m_analyzeAction->setEnabled(!m_currentFile.isEmpty());
    m_cancelAction->setEnabled(false);
    m_progressLabel->setText("失败 / 取消");
    m_runLog->appendPlainText("== 分析结束：" + reason + " ==\n");
}

// ---- 退出 ----

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 1) 让 worker 尽快停下（cancel 槽在 worker 线程执行）
    if (m_running)
        emit cancelRequested();
    // 2) 退出 worker 事件循环并等待线程结束
    m_thread->quit();
    m_thread->wait(5000);
    // 3) 优雅停止 llama-server（terminate → 5s → kill 阶梯在 manager 内部）
    m_server->stop();
    event->accept();
}
