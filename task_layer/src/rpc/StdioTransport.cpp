#include "rpc/StdioTransport.h"

#include <cstdio>

#include <QFile>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace rpc {

// ---------------------------------------------------------------- StdinReader
void StdinReader::run()
{
    QFile in;
    if (!in.open(stdin, QIODevice::ReadOnly)) {
        emit closed();
        return;
    }
    while (!m_stop.load()) {
        const QByteArray raw = in.readLine();     // 阻塞直到一行或 EOF
        if (raw.isEmpty()) {                      // EOF：UI 侧已退出
            if (!m_stop.load()) emit closed();
            return;
        }
        const QString line = QString::fromUtf8(raw).trimmed();
        if (!line.isEmpty()) emit lineReceived(line);
    }
}

// ------------------------------------------------------------- StdioTransport
StdioTransport::StdioTransport(QObject* parent) : QObject(parent)
{
#ifdef Q_OS_WIN
    // 关掉文本模式换行转换，保证 JSON 行按字节原样收发
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    m_reader = new StdinReader();       // 无 parent：析构时手工收尾，避免线程未停先删对象
    connect(m_reader, &StdinReader::lineReceived, this, &StdioTransport::onLine,
            Qt::QueuedConnection);
    connect(m_reader, &StdinReader::closed, this, &StdioTransport::inputClosed,
            Qt::QueuedConnection);
    m_reader->start();
}

StdioTransport::~StdioTransport()
{
    if (!m_reader) return;
    m_reader->requestStop();
    if (!m_reader->wait(300)) {
        // 仍阻塞在 read 上（管道未关闭）：进程即将退出，强制收尾
        m_reader->terminate();
        m_reader->wait(500);
    }
    delete m_reader;
    m_reader = nullptr;
}

void StdioTransport::onLine(const QString& line)
{
    dispatch(line.toUtf8());
}

void StdioTransport::dispatch(const QByteArray& line)
{
    bool isRequest = false;
    QJsonValue id;
    QString method;
    QJsonObject params;
    QByteArray errReply;
    if (!parseMessage(line, &isRequest, &id, &method, &params, &errReply)) {
        writeLine(errReply);
        return;
    }
    if (isRequest) emit requestReceived(id, method, params);
}

void StdioTransport::writeLine(const QByteArray& data)
{
    if (data.isEmpty()) return;
    std::fwrite(data.constData(), 1, size_t(data.size()), stdout);
    std::fflush(stdout);
}

void StdioTransport::sendResponse(const QJsonValue& id, const QJsonValue& result)
{
    writeLine(makeResponse(id, result));
}

void StdioTransport::sendError(const QJsonValue& id, int code, const QString& message,
                              const QJsonObject& data)
{
    writeLine(makeError(id, code, message, data));
}

void StdioTransport::notify(const QString& method, const QJsonObject& params)
{
    writeLine(makeNotification(method, params));
}

void StdioTransport::flush()
{
    std::fflush(stdout);
}

} // namespace rpc
