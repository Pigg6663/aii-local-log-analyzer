#ifndef LLAMAHTTPCLIENT_H
#define LLAMAHTTPCLIENT_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// llama-server 的 OpenAI 兼容 HTTP 客户端（POST /v1/chat/completions，SSE 流式）。
//
// 同步封装：chat() 内部用 QEventLoop 阻塞等待，返回时响应已完整。
// 注意：chat() 会阻塞调用线程的事件循环——只应在 Worker 线程调用，
// 绝不要在 UI 线程调用（控制台测试程序里主线程无事可做，可以例外）。
class LlamaHttpClient : public QObject
{
    Q_OBJECT
public:
    explicit LlamaHttpClient(QObject *parent = nullptr);

    void setBaseUrl(const QString &url);    // 例：http://127.0.0.1:8080
    void setTimeoutMs(int ms);              // 单次请求总超时，默认 300000（CPU 推理慢，给足）
    void setJsonMode(bool on);              // 请求 body 附带 response_format=json_object

    // 发送一次 chat 请求，阻塞到完成。成功返回 true，*replyText 为完整回复。
    // 失败 / 超时 / 取消返回 false，*errorString 给出原因。
    bool chat(const QString &systemPrompt, const QString &userPrompt,
              QString *replyText, QString *errorString,
              int maxTokens = 512, double temperature = 0.2);

    void abortCurrent();                    // 取消当前请求（chat() 返回 false）
    bool isBusy() const { return m_reply != nullptr; }

signals:
    void tokenReceived(const QString &delta);  // 流式增量（同线程直连时同步触发）
    void responseCompleted(const QString &fullText);

private:
    QNetworkAccessManager *m_nam;
    QNetworkReply *m_reply;      // 在途请求（同一时刻只有一个）
    QString m_baseUrl;
    int m_timeoutMs = 300000;
    bool m_jsonMode = false;
    bool m_abortRequested = false;
    bool m_timedOut = false;
};

#endif // LLAMAHTTPCLIENT_H
