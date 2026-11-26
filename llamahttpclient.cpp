#include "llamahttpclient.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

LlamaHttpClient::LlamaHttpClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_reply(nullptr)
{
}

void LlamaHttpClient::setBaseUrl(const QString &url) { m_baseUrl = url; }
void LlamaHttpClient::setTimeoutMs(int ms)           { m_timeoutMs = ms; }
void LlamaHttpClient::setJsonMode(bool on)           { m_jsonMode = on; }

bool LlamaHttpClient::chat(const QString &systemPrompt, const QString &userPrompt,
                           QString *replyText, QString *errorString,
                           int maxTokens, double temperature)
{
    if (m_reply) {
        *errorString = "已有请求在进行中";
        return false;
    }
    if (m_baseUrl.isEmpty()) {
        *errorString = "baseUrl 未设置（先调用 setBaseUrl）";
        return false;
    }

    // ---- 构造 OpenAI chat.completions 兼容请求体 ----
    QJsonArray messages;
    if (!systemPrompt.isEmpty())
        messages.append(QJsonObject{ {"role", "system"}, {"content", systemPrompt} });
    messages.append(QJsonObject{ {"role", "user"}, {"content", userPrompt} });

    QJsonObject body{
        {"messages", messages},
        {"temperature", temperature},
        {"max_tokens", maxTokens},
        {"stream", true},
        // 固定种子：同一份日志重复分析应得到同样的结果。
        // 不固定时，1.7B 模型在 temperature 0.2 下同一段日志会给出 6 条或 15 条
        // 差别很大的结果（实测两种都出现过），既无法复现也无法向用户解释。
        {"seed", 42},
        // 强制贪心解码。只把 temperature 设成 0 并不够：llama-server 实测仍在采样，
        // 同一请求两次分别给出 11 条和 5 条结果；top_k=1 之后连跑三次输出逐字节一致。
        {"top_k", 1},
        {"top_p", 1.0}
    };
    if (m_jsonMode)
        body.insert("response_format", QJsonObject{ {"type", "json_object"} });

    // 关闭模型的"思考"模式。Qwen3 一类模型默认先输出 reasoning_content（整段思维链），
    // 会把 max_tokens 预算吃光，正文 JSON 只写了个开头就被截断，导致解析必然失败
    // （实测 512 token 全部被思维链占用，content 只剩 121 字符）。
    // 该参数经 chat template 生效；模板里没有这个变量的模型会直接忽略，无副作用。
    body.insert("chat_template_kwargs", QJsonObject{ {"enable_thinking", false} });

    QNetworkRequest req(QUrl(m_baseUrl + "/v1/chat/completions"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply *reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_reply = reply;

    QString fullText;
    QByteArray sseBuf;
    m_abortRequested = false;
    m_timedOut = false;

    // ---- SSE 解析：按行切出 "data: ..."，累计 choices[0].delta.content ----
    connect(reply, &QNetworkReply::readyRead, this, [&] {
        sseBuf += reply->readAll();
        int idx;
        while ((idx = sseBuf.indexOf('\n')) >= 0) {
            const QByteArray line = sseBuf.left(idx).trimmed();
            sseBuf.remove(0, idx + 1);
            if (!line.startsWith("data:"))
                continue;
            const QByteArray payload = line.mid(5).trimmed();
            if (payload == "[DONE]")
                continue;
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
            if (err.error == QJsonParseError::NoError) {
                const QString delta = doc.object()
                                          .value("choices").toArray().first().toObject()
                                          .value("delta").toObject()
                                          .value("content").toString();
                if (!delta.isEmpty()) {
                    fullText += delta;
                    emit tokenReceived(delta);
                }
            }
        }
    });

    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    // ---- 超时兜底：Qt 5.14 没有 QNetworkRequest::setTransferTimeout（5.15 才有），
    //      用单发定时器 + abort 实现 ----
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    connect(&timeoutTimer, &QTimer::timeout, this, [&] {
        if (m_reply && m_reply->isRunning()) {
            m_timedOut = true;
            m_reply->abort();
        }
    });
    timeoutTimer.start(m_timeoutMs);

    loop.exec(); // 阻塞直到 finished（正常完成 / 网络错误 / abort / 超时）

    const QNetworkReply::NetworkError netErr = reply->error();
    if (netErr != QNetworkReply::NoError) {
        if (m_timedOut)
            *errorString = "请求超时（CPU 推理时间过长，可调大 setTimeoutMs）";
        else if (m_abortRequested)
            *errorString = "已取消";
        else
            *errorString = reply->errorString();
        m_reply = nullptr;
        reply->deleteLater();
        return false;
    }

    m_reply = nullptr;
    reply->deleteLater();
    *replyText = fullText;
    emit responseCompleted(fullText);
    return true;
}

void LlamaHttpClient::abortCurrent()
{
    if (m_reply) {
        m_abortRequested = true;
        m_reply->abort();
    }
}
