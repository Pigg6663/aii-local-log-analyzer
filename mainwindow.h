#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include "datatypes.h"
#include "llamaservermanager.h"

class QAction;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QPlainTextEdit;
class QTabWidget;
class QTableWidget;
class QTextBrowser;
class QThread;

class AnalysisWorker;

namespace Ui { class MainWindow; }

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

signals:
    // UI → Worker（queued）。serverBaseUrl 由 MainWindow 从 server 管理器取，
    // worker 不关心进程管理，只关心请求地址。
    void analyzeRequested(const QString &filePath, const QString &serverBaseUrl);
    void cancelRequested();                           // UI → Worker（queued）

private slots:
    void onAnalyzeClicked();
    void onCancelClicked();
    void onServerToggled();
    void onServerStateChanged(LlamaServerManager::State state, const QString &desc);
    void onServerReady();
    void onServerFailed(const QString &reason);
    void onSettingsClicked();
    void onExportClicked();
    void onFindingJumped(int row, int column);
    void onProgress(int current, int total, const QString &stage);   // Worker → UI（queued）
    void onFindingFound(const AnomalyFinding &finding);              // Worker → UI（queued）
    void onFinished(const QVector<AnomalyFinding> &findings, const QString &report);
    void onFailed(const QString &reason);

private:
    void buildUi();
    void buildToolbar();
    void buildMenus();
    void beginAnalysis();                   // 服务就绪后真正启动分析（emit analyzeRequested）
    void loadLogForDisplay(const QString &path);  // 只读显示，上限 2MB（分析用完整文件）
    void jumpToLine(int lineNumber);        // 日志视图跳转 + 整行高亮

    Ui::MainWindow *ui;

    QThread *m_thread;                 // worker 线程（parent = this，析构前必须 quit+wait）
    AnalysisWorker *m_worker;          // 无 parent（moveToThread 的前提），由 finished→deleteLater 回收
    LlamaServerManager *m_server;      // UI 线程，进程生命周期管理

    QLabel *m_dropLabel;               // 顶部：当前文件/状态提示
    QPlainTextEdit *m_logView;         // 日志视图（只读，跳转 + 高亮目标）
    QTabWidget *m_tabs;
    QTableWidget *m_findingsTable;     // 检测结果列表
    QTextBrowser *m_reportView;        // 分析报告（Markdown 渲染）
    QPlainTextEdit *m_runLog;          // 运行日志（状态消息、服务/解析错误）

    QLabel *m_progressLabel;           // 状态栏左侧：分析进度
    QLabel *m_serverStatusLabel;       // 状态栏右侧永久区：服务状态
    QAction *m_analyzeAction;
    QAction *m_cancelAction;
    QAction *m_serverAction;
    QAction *m_exportAction;

    QString m_currentFile;
    QString m_reportMarkdown;          // 供导出用的原始 Markdown
    bool m_running = false;
    bool m_pendingAnalysis = false;    // 已点"开始分析"，正在等服务就绪
};

#endif // MAINWINDOW_H
