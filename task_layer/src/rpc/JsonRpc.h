// JsonRpc.h — JSON-RPC 2.0 消息信封（Qt Core 版，仅头文件）
#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QVariant>

namespace rpc {

// 标准 JSON-RPC 错误码（2.0 规范保留 -32768..-32000）
enum ErrorCode {
    ParseError     = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams  = -32602,
    InternalError  = -32603,
    // 应用层错误码（-32000 以下自定义）
    ErrCancelled   = -32000,
    ErrTaskNotFound = -32001,
    ErrTaskRunning  = -32002,
    ErrInputInvalid = -32003,
    ErrConfig       = -32004,
    ErrModuleExec   = -32005,
};

struct Response {
    QJsonValue id;          // null | number | string（请求原样返回）
    QJsonValue result;      // 成功时
    int errorCode = 0;      // 失败时
    QString errorMessage;
    QJsonObject errorData;
    bool isError = false;
};

inline QByteArray makeRequest(const QJsonValue& id, const QString& method,
                              const QJsonObject& params)
{
    QJsonObject o;
    o["jsonrpc"] = "2.0";
    o["id"] = id;
    o["method"] = method;
    o["params"] = params;
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

inline QByteArray makeResponse(const QJsonValue& id, const QJsonValue& result)
{
    QJsonObject o;
    o["jsonrpc"] = "2.0";
    o["id"] = id;
    o["result"] = result;
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

inline QByteArray makeError(const QJsonValue& id, int code, const QString& message,
                            const QJsonObject& data = {})
{
    QJsonObject o;
    o["jsonrpc"] = "2.0";
    o["id"] = id;
    QJsonObject e;
    e["code"] = code;
    e["message"] = message;
    if (!data.isEmpty()) e["data"] = data;
    o["error"] = e;
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

inline QByteArray makeNotification(const QString& method, const QJsonObject& params)
{
    QJsonObject o;
    o["jsonrpc"] = "2.0";
    o["method"] = method;
    o["params"] = params;
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

// 解析一行输入；返回 false 表示解析失败（填充 outResponse 为解析错误）
inline bool parseMessage(const QByteArray& line, bool* isRequest,
                         QJsonValue* id, QString* method, QJsonObject* params,
                         QByteArray* errorReply)
{
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        *errorReply = makeError(QJsonValue(), ParseError, "invalid JSON: " + perr.errorString());
        return false;
    }
    const QJsonObject o = doc.object();
    if (o.value("jsonrpc").toString() != "2.0" || !o.contains("method")) {
        *errorReply = makeError(o.value("id"), InvalidRequest, "not a jsonrpc 2.0 request");
        return false;
    }
    *isRequest = true;
    *id = o.value("id");
    *method = o.value("method").toString();
    *params = o.value("params").toObject();
    return true;
}

} // namespace rpc
