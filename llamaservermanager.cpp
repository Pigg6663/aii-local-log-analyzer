#include "llamaservermanager.h"

#include <QAbstractSocket>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>

LlamaServerManager::LlamaServerManager(QObject *parent)
    : QObject(parent)
    , m_proc(new QProcess(this))
    , m_nam(new QNetworkAccessManager(this))
    , m_healthReply(nullptr)
    , m_healthTimer(new QTimer(this))
    , m_healthElapsed(0)
    , m_probeSocket(new QTcpSocket(this))
    , m_probeTimer(new QTimer(this))
    , m_probeHealthReply(nullptr)
    , m_probeHealthTimer(new QTimer(this))
    , m_restartRequested(false)
    , m_threads(QThread::idealThreadCount())
{
    // 跨线程 queued 连接时需要（本项目同线程为主，注册上以防万一）
    qRegisterMetaType<LlamaServerManager::State>("LlamaServerManager::State");

    // stderr 单独捕获，喂给"服务日志"面板 / 测试程序
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);

    m_healthTimer->setInterval(500);
    connect(m_healthTimer, &QTimer::timeout, this, &LlamaServerManager::onHealthCheck);

    // 端口预检：连接超时 2s（本地端口拒绝会立即返回，超时基本=被别的程序占用）
    m_probeTimer->setSingleShot(true);
    m_probeTimer->setInterval(2000);
    connect(m_probeTimer, &QTimer::timeout, this, &LlamaServerManager::onProbeTimeout);

    connect(m_probeSocket, &QTcpSocket::connected, this, &LlamaServerManager::onProbeConnected);
    // Qt 5.14 的信号名是 error()（errorOccurred 是 5.15 才改的名），QOverload 消除与 error() 取值函数的歧义
    connect(m_probeSocket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, &LlamaServerManager::onProbeError);

    // 复用确认：对方 /health 的回复超时 3s
    m_probeHealthTimer->setSingleShot(true);
    m_probeHealthTimer->setInterval(3000);
    connect(m_probeHealthTimer, &QTimer::timeout, this, &LlamaServerManager::onProbeHealthTimeout);

    connect(m_proc, &QProcess::errorOccurred, this, &LlamaServerManager::onProcessError);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &LlamaServerManager::onProcessFinished);
    connect(m_proc, &QProcess::readyReadStandardError, this, &LlamaServerManager::onStderrReady);

    // 进程创建成功后才开始健康轮询（started 只表示进程起来了，不代表能收请求）
    connect(m_proc, &QProcess::started, this, [this] {
        setState(State::Loading, QString("模型加载中…（最多等 %1 秒）").arg(m_loadTimeoutMs / 1000));
        emit serverLoading();
        m_healthTimer->start();
    });
}

QString LlamaServerManager::apiBaseUrl() const
{
    return QString("http://127.0.0.1:%1").arg(m_port);
}

// ---- 配置 ----

void LlamaServerManager::setServerBinary(const QString &path) { m_binary = path; }
void LlamaServerManager::setModelPath(const QString &path)    { m_model = path; }
void LlamaServerManager::setPort(quint16 port)                { m_port = port; }
void LlamaServerManager::setContextSize(int tokens)           { m_ctxSize = tokens; }
void LlamaServerManager::setThreads(int n)                    { m_threads = n; }
void LlamaServerManager::setLoadTimeoutMs(int ms)             { m_loadTimeoutMs = ms; }

// ---- 生命周期 ----

void LlamaServerManager::start()
{
    if (m_state == State::Starting || m_state == State::Loading
        || m_state == State::Ready || m_state == State::Stopping)
        return; // 已在运行或启动中

    // W5 端口预检：启动自己的进程之前先探测端口——
    //   被健康的 llama-server 占用 → 直接复用（省一次模型加载）；
    //   被其他程序占用 → 明确报错，引导用户改端口；
    //   空闲 → 正常拉起自己的进程。
    // 探测放在二进制/模型检查之前：复用外部服务时根本用不到这两个路径。
    setState(State::Starting, QString("正在检测端口 %1…").arg(m_port));
    m_probeSocket->abort();   // 清上次探测残留（此时 m_probing 仍为 false，信号被忽略）
    m_probing = true;
    m_probeSocket->connectToHost("127.0.0.1", m_port);
    m_probeTimer->start();
}

void LlamaServerManager::onProbeConnected()
{
    m_probing = false;
    m_probeTimer->stop();
    m_probeSocket->abort();   // 断开探测连接（m_probing 已置 false，其后信号被忽略）

    // 端口被占：确认对方是不是健康的 llama-server，是则直接复用
    setState(State::Starting, QString("端口 %1 已被占用，正在确认是否可复用…").arg(m_port));
    QNetworkRequest req(QUrl(apiBaseUrl() + "/health"));
    m_probeHealthReply = m_nam->get(req);
    connect(m_probeHealthReply, &QNetworkReply::finished,
            this, &LlamaServerManager::onProbeHealthFinished);
    m_probeHealthTimer->start();
}

void LlamaServerManager::onProbeError(QAbstractSocket::SocketError error)
{
    if (!m_probing)
        return;   // 主动 abort 或已进入复用确认阶段
    m_probing = false;
    m_probeTimer->stop();

    if (error == QAbstractSocket::ConnectionRefusedError) {
        beginProcessLaunch();   // 端口空闲 → 正常拉起自己的 llama-server
        return;
    }
    // 其他错误：按"被占且不可用"处理
    setState(State::Failed, "端口探测失败");
    emit serverFailed(QString("端口 %1 不可用（%2）。请在【设置 → 设置…】中修改端口后重试")
                      .arg(m_port).arg(m_probeSocket->errorString()));
}

void LlamaServerManager::onProbeTimeout()
{
    if (!m_probing)
        return;
    m_probing = false;
    m_probeSocket->abort();
    failPortOccupied();
}

void LlamaServerManager::onProbeHealthFinished()
{
    if (!m_probeHealthReply)
        return;

    QNetworkReply *reply = m_probeHealthReply;
    m_probeHealthReply = nullptr;
    reply->deleteLater();

    m_probeHealthTimer->stop();
    if (m_state != State::Starting)
        return;   // 确认期间被 stop() 取消

    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 200) {
        // 已有健康的 llama-server：直接复用，本程序不负责它的生命周期
        m_ownsProcess = false;
        setState(State::Ready, QString("复用已有服务（端口 %1）").arg(m_port));
        emit serverReady();
    } else {
        failPortOccupied();
    }
}

void LlamaServerManager::onProbeHealthTimeout()
{
    if (!m_probeHealthReply)
        return;

    QNetworkReply *reply = m_probeHealthReply;
    m_probeHealthReply = nullptr;
    m_probeHealthTimer->stop();
    reply->abort();         // 其 finished 信号到达时 m_probeHealthReply 已空，不会二次处理
    reply->deleteLater();

    failPortOccupied();
}

void LlamaServerManager::failPortOccupied()
{
    if (m_state != State::Starting)
        return;
    setState(State::Failed, "端口被其他程序占用");
    emit serverFailed(QString("端口 %1 已被其他程序占用（对方不是可用的 llama-server）。"
                              "请在【设置 → 设置…】中修改端口后重试").arg(m_port));
}

void LlamaServerManager::beginProcessLaunch()
{
    if (!QFileInfo::exists(m_binary)) {
        setState(State::Failed, "llama-server.exe 不存在");
        emit serverFailed("llama-server.exe 不存在：" + m_binary);
        return;
    }
    if (!QFileInfo::exists(m_model)) {
        setState(State::Failed, "模型文件不存在");
        emit serverFailed("模型文件不存在：" + m_model);
        return;
    }

    const QStringList args{
        "-m", m_model,
        "--host", "127.0.0.1",
        "--port", QString::number(m_port),
        "--ctx-size", QString::number(m_ctxSize),
        "--threads", QString::number(m_threads),
        "-ngl", "0"   // 明确走 CPU 推理
    };

    m_healthElapsed = 0;
    setState(State::Starting, "正在启动 llama-server…");
    m_ownsProcess = true;
    m_proc->start(m_binary, args);
    // 后续流程由 started / errorOccurred / finished 信号驱动
}

void LlamaServerManager::cancelProbe()
{
    m_probing = false;
    m_probeTimer->stop();
    m_probeSocket->abort();
    if (m_probeHealthReply) {
        QNetworkReply *reply = m_probeHealthReply;
        m_probeHealthReply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    m_probeHealthTimer->stop();
}

void LlamaServerManager::stop()
{
    cancelProbe();   // 探测/复用确认阶段被停：取消在途请求（未探测时是 no-op）

    if (m_proc->state() == QProcess::NotRunning) {
        setState(State::Idle, m_ownsProcess ? "服务已停止"
                                            : "已断开（外部服务仍在运行）");
        emit serverStopped();
        return;
    }

    setState(State::Stopping, "正在停止服务…");
    m_healthTimer->stop();
    m_proc->terminate();
    // terminate 对 Windows 控制台程序不一定生效，5 秒后未退出则强杀
    QTimer::singleShot(5000, this, [this] {
        if (m_proc->state() != QProcess::NotRunning)
            m_proc->kill();
    });
}

void LlamaServerManager::restart()
{
    if (m_proc->state() != QProcess::NotRunning) {
        m_restartRequested = true;
        stop();
    } else {
        start();
    }
}

// ---- 就绪检测 ----

void LlamaServerManager::onHealthCheck()
{
    if (m_healthReply)
        return; // 上一次健康检查仍在途，跳过本拍

    m_healthElapsed += m_healthTimer->interval();
    if (m_healthElapsed > m_loadTimeoutMs) {
        m_healthTimer->stop();
        setState(State::Failed, "模型加载超时");
        emit serverFailed(QString("模型加载超时（%1 秒），请检查模型文件，或调大 setLoadTimeoutMs")
                          .arg(m_loadTimeoutMs / 1000));
        m_proc->kill();
        return;
    }

    QNetworkRequest req(QUrl(apiBaseUrl() + "/health"));
    m_healthReply = m_nam->get(req);
    connect(m_healthReply, &QNetworkReply::finished,
            this, &LlamaServerManager::onHealthFinished);
}

void LlamaServerManager::onHealthFinished()
{
    if (!m_healthReply)
        return;

    QNetworkReply *reply = m_healthReply;
    m_healthReply = nullptr;
    reply->deleteLater();

    if (m_state != State::Loading)
        return; // 已就绪 / 已失败，迟到的回复忽略

    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 200) {
        m_healthTimer->stop();
        setState(State::Ready, "服务就绪");
        emit serverReady();
    }
    // 其他情况（连接拒绝 / 503 加载中 / 任何错误）都继续等下一拍：
    // 模型加载完成前 /health 不会返回 200，这是就绪的唯一判据
}

// ---- 进程异常处理 ----

void LlamaServerManager::onProcessError(QProcess::ProcessError error)
{
    // 运行期错误都走 finished 信号，这里只需处理启动失败
    if (error == QProcess::FailedToStart) {
        m_healthTimer->stop();
        setState(State::Failed, "启动失败");
        emit serverFailed("无法启动 llama-server：" + m_proc->errorString()
                          + "（请检查路径、端口占用，以及 exe 同目录下的 ggml*.dll 是否齐全）");
    }
}

void LlamaServerManager::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    const bool wasReady = (m_state == State::Ready);
    m_healthTimer->stop();

    // 进程已退出，在途的健康检查作废
    if (m_healthReply) {
        QNetworkReply *reply = m_healthReply;
        m_healthReply = nullptr;
        reply->abort();
        reply->deleteLater();
    }

    if (m_state == State::Stopping) {
        setState(State::Idle, "服务已停止");
        emit serverStopped();
        if (m_restartRequested) {
            m_restartRequested = false;
            start();
        }
    } else if (wasReady) {
        setState(State::Crashed, "服务异常退出");
        emit serverCrashed(exitCode);
    } else {
        setState(State::Failed, "启动阶段进程退出");
        emit serverFailed(QString("llama-server 在就绪前退出（exit code %1），"
                                  "请查看服务日志排查（常见原因：端口被占用、模型文件损坏）")
                          .arg(exitCode));
    }
}

void LlamaServerManager::onStderrReady()
{
    m_stderrBuf += QString::fromLocal8Bit(m_proc->readAllStandardError());
    int idx;
    while ((idx = m_stderrBuf.indexOf('\n')) >= 0) {
        const QString line = m_stderrBuf.left(idx).trimmed();
        m_stderrBuf.remove(0, idx + 1);
        if (!line.isEmpty())
            emit serverLog(line);
    }
}

void LlamaServerManager::setState(State s, const QString &desc)
{
    m_state = s;
    emit stateChanged(m_state, desc);
}
