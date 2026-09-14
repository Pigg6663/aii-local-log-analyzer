#ifndef LLAMASERVERMANAGER_H
#define LLAMASERVERMANAGER_H

#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTcpSocket>   // QAbstractSocket 一并引入（端口预检）

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// llama-server 子进程的生命周期管理：
//   端口预检（被健康服务占用则直接复用）→ 启动（QProcess）→ /health 轮询就绪检测
//   → 崩溃/异常退出监控 → 优雅关闭。
// 只依赖 QtCore + QtNetwork，可在任意带事件循环的线程使用（本项目放在 UI 线程）。
class LlamaServerManager : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Idle,       // 未启动
        Starting,   // QProcess 启动中
        Loading,    // 进程已起，等待模型加载完成（/health 尚未返回 200）
        Ready,      // /health 返回 200，可接收请求
        Stopping,   // 正在优雅关闭
        Failed,     // 启动失败 / 加载超时
        Crashed     // 就绪后异常退出
    };

    explicit LlamaServerManager(QObject *parent = nullptr);
    // 析构不做显式处理：QProcess 析构会兜底结束子进程，保证不会遗留孤儿进程

    // ---- 配置（必须在 start() 之前设置）----
    void setServerBinary(const QString &path);  // llama-server.exe 完整路径
    void setModelPath(const QString &path);     // GGUF 模型路径
    void setPort(quint16 port);                 // 默认 8080
    void setContextSize(int tokens);            // 默认 8192
    void setThreads(int n);                     // 默认 QThread::idealThreadCount()
    void setLoadTimeoutMs(int ms);              // 就绪检测总超时，默认 180000

    // ---- 查询 ----
    State state() const { return m_state; }
    bool isReady() const { return m_state == State::Ready; }
    QString apiBaseUrl() const;                 // http://127.0.0.1:<port>

public slots:
    void start();    // 按需启动；已在运行/启动中则忽略
    void stop();     // 优雅退出阶梯：terminate → 5 秒 → kill
    void restart();  // 运行中则先停后起，未运行则直接起

signals:
    void stateChanged(LlamaServerManager::State state, const QString &desc);
    void serverLoading();
    void serverReady();
    void serverFailed(const QString &reason);
    void serverCrashed(int exitCode);
    void serverStopped();
    void serverLog(const QString &line);        // llama-server 的 stderr 输出（按行）

private slots:
    void onHealthCheck();
    void onHealthFinished();
    void onProcessError(QProcess::ProcessError error);
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onStderrReady();
    // 端口预检（W5）：同步探测（waitForConnected），空闲则拉起，被占则走 /health 复用确认
    void onProbeHealthFinished();
    void onProbeHealthTimeout();

private:
    void setState(State s, const QString &desc);
    void beginProcessLaunch();   // 端口空闲：二进制/模型检查 + QProcess 拉起
    void beginReuseCheck();      // 端口被占：GET /health 确认对方是否可复用的 llama-server
    void cancelProbe();          // 取消在途的复用确认请求（stop() 在探测阶段也会调用）
    void failPortOccupied();     // 复用确认失败：统一报"端口被占"错误

    QProcess *m_proc;
    QNetworkAccessManager *m_nam;   // 仅用于 /health 检查
    QNetworkReply *m_healthReply;   // 在途的健康检查（同一时刻只有一个）
    QTimer *m_healthTimer;
    int m_healthElapsed;            // 已等待毫秒数

    // 端口预检（W5）
    QTcpSocket *m_probeSocket;          // 探测目标端口（waitForConnected 同步探测，连接成功 = 被占）
    QNetworkReply *m_probeHealthReply;  // 复用确认的 /health 请求（同一时刻只有一个）
    QTimer *m_probeHealthTimer;         // 复用确认超时（3s）
    bool m_ownsProcess = false;         // true=本程序拉起的进程（stop 才终止）；false=复用外部服务

    QString m_stderrBuf;            // stderr 按行切分的剩余缓冲
    bool m_restartRequested;        // stop 完成后是否要自动再启动

    // 配置
    QString m_binary;
    QString m_model;
    quint16 m_port = 8080;
    int m_ctxSize = 8192;
    int m_threads = 0;
    int m_loadTimeoutMs = 180000;

    State m_state = State::Idle;
};

#endif // LLAMASERVERMANAGER_H
