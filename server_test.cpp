// W1 闭环测试（控制台程序，不依赖 Qt Widgets）：
//   启动 llama-server → /health 就绪检测 → 发一次 chat 请求（SSE 流式）→
//   打印完整回复 → 优雅停止服务 → 退出。
//
// 用法：
//   server_test.exe <llama-server.exe路径> <GGUF模型路径> [端口=8080]
//
// 示例：
//   server_test.exe "D:/llama/bin/llama-server.exe" "D:/models/qwen3-1.7b-q4_k_m.gguf"

#include <QCoreApplication>
#include <QDebug>
#include <QStringList>
#include <QTimer>

#include "llamahttpclient.h"
#include "llamaservermanager.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8); // 让控制台正确显示 UTF-8（中文提示词/回复）
#endif

    QCoreApplication app(argc, argv);

    const QStringList args = QCoreApplication::arguments();
    if (args.size() < 3) {
        qInfo().noquote() << "用法: server_test.exe <llama-server.exe路径> <GGUF模型路径> [端口=8080]";
        return 1;
    }
    const QString binary = args.at(1);
    const QString model  = args.at(2);
    const quint16 port   = args.size() >= 4 ? args.at(3).toUShort() : 8080;

    qInfo().noquote() << QString("llama-server: %1").arg(binary);
    qInfo().noquote() << QString("模型: %1").arg(model);
    qInfo().noquote() << QString("端口: %1").arg(port);

    LlamaServerManager server;
    server.setServerBinary(binary);
    server.setModelPath(model);
    server.setPort(port);

    // 状态变化与 llama-server 自身的 stderr 日志（排查问题全靠它）
    QObject::connect(&server, &LlamaServerManager::stateChanged,
                     [](LlamaServerManager::State, const QString &desc) {
        qInfo().noquote() << "[状态] " << desc;
    });
    QObject::connect(&server, &LlamaServerManager::serverLog,
                     [](const QString &line) {
        qInfo().noquote() << "  [server] " << line;
    });

    // 整体看门狗：5 分钟内没跑完就失败退出
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        qCritical() << "测试超时（5 分钟），退出";
        QCoreApplication::exit(2);
    });
    watchdog.start(5 * 60 * 1000);

    // 失败路径：打印原因并退出（server 析构会兜底结束子进程）
    QObject::connect(&server, &LlamaServerManager::serverFailed,
                     [](const QString &reason) {
        qCritical().noquote() << "serverFailed: " << reason;
        QCoreApplication::exit(1);
    });
    QObject::connect(&server, &LlamaServerManager::serverCrashed,
                     [](int code) {
        qCritical().noquote() << QString("serverCrashed: exit code %1").arg(code);
    });

    // 就绪后：发一次 chat 请求。chat() 是同步阻塞调用，
    // 测试程序主线程没有其他任务，允许在这里直接调（GUI 程序里必须放 Worker 线程）。
    QObject::connect(&server, &LlamaServerManager::serverReady, [&] {
        qInfo().noquote() << "服务就绪，开始 chat 测试…";

        LlamaHttpClient client;
        client.setBaseUrl(server.apiBaseUrl());
        client.setTimeoutMs(120000); // 小模型 CPU 推理，120 秒足够

        // 统计流式增量个数，验证 SSE 解析确实在工作（token 数 > 0 才算通过）
        int tokens = 0;
        QObject::connect(&client, &LlamaHttpClient::tokenReceived,
                         [&](const QString &) { ++tokens; });

        QString reply, err;
        const bool ok = client.chat(
            "你是一个乐于助人的 AI 助手。",
            "请用一句话介绍你自己，并告诉我你能做什么。",
            &reply, &err, 256);

        if (!ok) {
            qCritical().noquote() << "chat 失败: " << err;
            server.stop(); // 测试结束，顺带验证优雅退出路径
            QCoreApplication::exit(1);
            return;
        }

        qInfo().noquote() << QString("收到完整回复（%1 个流式增量，共 %2 字符）:").arg(tokens).arg(reply.size());
        qInfo().noquote() << reply;
        qInfo().noquote() << "闭环测试通过！正在优雅停止服务…";

        QObject::connect(&server, &LlamaServerManager::serverStopped, [] {
            qInfo().noquote() << "服务已停止，测试结束。";
            QCoreApplication::exit(0);
        });
        server.stop();
    });

    server.start();
    return app.exec();
}
