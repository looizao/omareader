#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <functional>
#include <memory>

class QTcpServer;

namespace Fixtures {
void writeEpub(const QString &path, const QByteArray &coverPng);
void writeFb2(const QString &path);
void writeDocx(const QString &path);
void writeOdt(const QString &path);
// A stand-in for qwen-worker.py that speaks the same JSON-lines protocol
// without any model: each sentence becomes half a second of 12 kHz audio per
// word, every word timed. It logs its arguments and requests to
// FAKE_QWEN_LOG, waits FAKE_QWEN_DELAY seconds per request, fails sentences
// reading FAIL, and stops with a fatal error when the model path contains
// "broken".
QByteArray fakeQwenWorker();
}

// A tiny HTTP/1.1 server on localhost that answers each request with a
// handler, for testing the Dropbox client and sign-in without Dropbox.
class FakeHttpServer {
public:
    struct Request {
        QByteArray method;
        QByteArray target; // path and query
        QHash<QByteArray, QByteArray> headers; // names in lower case
        QByteArray body;
    };
    struct Response {
        int status = 200;
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers;
    };
    using Handler = std::function<Response(const Request &)>;

    explicit FakeHttpServer(Handler handler);
    ~FakeHttpServer();
    QString url() const;
    QList<Request> requests;

private:
    Handler m_handler;
    std::unique_ptr<QTcpServer> m_server;
};
