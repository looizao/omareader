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

// Google Drive's API in memory, enough of it for the Drive client: files
// with properties, revisions and the trash, listing in pages of two, and
// multipart and resumable uploads. It can refuse a stale token, be busy,
// lose a piece of an upload, or forget an upload session.
struct FakeDrive {
    struct File {
        QString id;
        QString name;
        QString mimeType;
        QString parent;
        QHash<QString, QString> properties;
        QByteArray data;
        QString created;
        QString modified;
        int revision = 1;
        bool trashed = false;
    };
    struct Session {
        QString fileId; // empty for a new file
        QByteArray metadata;
        qint64 total = 0;
        QByteArray received;
    };

    FakeHttpServer::Response handle(const FakeHttpServer::Request &request);
    File *file(const QString &id);
    // The files with this omareaderPath property, trashed or not.
    QList<File *> withPath(const QString &path);
    File &add(const QString &path, const QByteArray &data, const QString &parent = {});
    QString stamp();

    QString base; // the server's URL, for session links
    QString token = QStringLiteral("fresh");
    QList<File> files;
    QHash<QString, Session> sessions;
    int tokenRequests = 0;
    QByteArray lastTokenRequest;
    int busy = 0; // 429s still to give
    bool losePiece = false; // fail the next piece after the first
    bool expireSession = false; // the next piece finds its session gone
    int clock = 0;
    int nextId = 0;
};
