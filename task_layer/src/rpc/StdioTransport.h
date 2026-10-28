// StdioTransport.h — stdin/stdout 行式 JSON-RPC 传输（供 QProcess 与 UI 通信）
//
// 注意：Qt6 的 QTextStream 既不是 QObject 也没有 readyRead 信号，
// 因此 stdin 必须由一个阻塞读线程驱动（StdinReader）。
#pragma once

#include <atomic>

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QThread>

#include "rpc/JsonRpc.h"

namespace rpc {

// 阻塞读 stdin 的后台线程：每读到一行就发信号（跨线程自动排队到主线程）。
class StdinReader : public QThread
{
    Q_OBJECT
public:
    explicit StdinReader(QObject* parent = nullptr) : QThread(parent) {}
    void requestStop() { m_stop.store(true); }

signals:
    void lineReceived(const QString& line);
    void closed();

protected:
    void run() override;

private:
    std::atomic<bool> m_stop{false};
};

class StdioTransport : public QObject
{
    Q_OBJECT
public:
    explicit StdioTransport(QObject* parent = nullptr);
    ~StdioTransport() override;

    // 向对端发送响应（带请求 id）
    void sendResponse(const QJsonValue& id, const QJsonValue& result);
    void sendError(const QJsonValue& id, int code, const QString& message,
                   const QJsonObject& data = {});
    // 主动通知（无 id）
    void notify(const QString& method, const QJsonObject& params);
    void flush();

signals:
    void requestReceived(const QJsonValue& reqId, const QString& method,
                         const QJsonObject& params);
    void inputClosed();   // stdin 已关闭（UI 进程退出）

private slots:
    void onLine(const QString& line);

private:
    void dispatch(const QByteArray& line);
    void writeLine(const QByteArray& data);

    StdinReader* m_reader = nullptr;
};

} // namespace rpc
