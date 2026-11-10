// Made-up books in every format omareader parses natively, built in temp
// directories by the tests. Kept out of the test class's file because moc's
// lexer trips over the XML raw strings.
#include "fixtures.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <private/qzipwriter_p.h>

namespace {
void makeZip(const QString &path, const QList<std::pair<QString, QByteArray>> &files) {
    QZipWriter zip(path);
    for (const auto &[name, data] : files)
        zip.addFile(name, data);
    zip.close();
}
}

namespace Fixtures {

void writeEpub(const QString &path, const QByteArray &png) {
    makeZip(path, {
            {"mimetype", "application/epub+zip"},
            {"META-INF/container.xml", R"(<?xml version="1.0"?><container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
            {"OEBPS/content.opf", R"(<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>A Made-up Book</dc:title><dc:creator>Ada Example</dc:creator></metadata>
<manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="c1" href="text/one.xhtml" media-type="application/xhtml+xml"/><item id="c1b" href="text/one-b.xhtml" media-type="application/xhtml+xml"/><item id="c2" href="text/two%20b.xhtml" media-type="application/xhtml+xml"/><item id="img" href="images/cover.png" media-type="image/png" properties="cover-image"/></manifest>
<spine><itemref idref="c1"/><itemref idref="c1b"/><itemref idref="c2"/></spine></package>)"},
            {"OEBPS/nav.xhtml", R"(<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol><li><a href="text/one.xhtml">The Beginning</a></li><li><a href="text/two%20b.xhtml#x">The End</a></li></ol></nav></body></html>)"},
            {"OEBPS/text/one.xhtml", "<html><body><h1>One</h1><p>Once upon a time.</p></body></html>"},
            {"OEBPS/text/one-b.xhtml", "<html><body><p>The middle, split into its own file.</p></body></html>"},
            {"OEBPS/text/two b.xhtml", "<html><body><p>They lived happily.</p></body></html>"},
            {"OEBPS/images/cover.png", png},
    });
}

void writeFb2(const QString &path) {
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(R"(<?xml version="1.0" encoding="utf-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:l="http://www.w3.org/1999/xlink">
<description><title-info><author><first-name>Ivan</first-name><last-name>Example</last-name></author><book-title>Short Tale</book-title></title-info></description>
<body><section><title><p>Part One</p></title><p>First <emphasis>words</emphasis> here.</p><section><title><p>Inner</p></title><p>Nested text.</p></section></section>
<section><title><p>Part Two</p></title><p>Last words.</p></section></body>
<body name="notes"><section><p>A note that is not read.</p></section></body></FictionBook>)");
}

void writeDocx(const QString &path) {
    makeZip(path, {
            {"[Content_Types].xml", "<Types/>"},
            {"docProps/core.xml", R"(<cp:coreProperties xmlns:cp="x" xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Doc Title</dc:title><dc:creator>Doc Author</dc:creator></cp:coreProperties>)"},
            {"word/document.xml", R"(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>
<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Opening</w:t></w:r></w:p>
<w:p><w:r><w:t xml:space="preserve">Hello </w:t></w:r><w:r><w:t>world.</w:t></w:r></w:p>
<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Closing</w:t></w:r></w:p>
<w:p><w:r><w:t>Goodbye</w:t><w:delText>deleted</w:delText><w:t>.</w:t></w:r></w:p></w:body></w:document>)"},
    });
}

void writeOdt(const QString &path) {
    makeZip(path, {
            {"mimetype", "application/vnd.oasis.opendocument.text"},
            {"meta.xml", R"(<office:document-meta xmlns:office="o" xmlns:dc="http://purl.org/dc/elements/1.1/"><office:meta><dc:title>Odt Title</dc:title></office:meta></office:document-meta>)"},
            {"content.xml", R"(<office:document-content xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"><office:body><office:text>
<text:h text:outline-level="1">Heading</text:h><text:p>Two<text:s/>words<text:tab/>and<text:line-break/>a break.</text:p></office:text></office:body></office:document-content>)"},
    });
}

}

QByteArray Fixtures::fakeQwenWorker() {
    return R"PY(import json, os, queue, sys, threading, time, wave
args = sys.argv[1:]
log = os.environ.get("FAKE_QWEN_LOG")
lock = threading.Lock()
def note(entry):
    if log:
        with lock, open(log, "a") as f:
            f.write(json.dumps(entry) + "\n")
def reply(message):
    with lock:
        sys.stdout.write(json.dumps(message) + "\n")
        sys.stdout.flush()
note({"args": args})
model = args[args.index("--model") + 1]
if "broken" in model:
    reply({"event": "fatal", "message": "The model is broken."})
    sys.exit(1)
reply({"event": "ready", "device": "cpu", "gpu": "", "alignerDevice": "cpu"})
# Like the real worker, stdin is read on its own thread so a cancel lands
# while a request is being "spoken".
work = queue.Queue()
cancelled = set()
def read():
    for line in sys.stdin:
        request = json.loads(line)
        if request.get("op") == "quit":
            break
        if request.get("op") == "cancel":
            note({"cancel": request["req"]})
            cancelled.add(request["req"])
            continue
        note({"request": request})
        work.put(request)
    work.put(None)
threading.Thread(target=read, daemon=True).start()
while True:
    request = work.get()
    if request is None:
        break
    delay = float(os.environ.get("FAKE_QWEN_DELAY", "0"))
    while delay > 0 and request["req"] not in cancelled:
        time.sleep(min(delay, 0.02))
        delay -= 0.02
    if request["req"] in cancelled:
        note({"cancelled": request["req"]})
        reply({"event": "done", "req": request["req"], "items": [{"ok": False, "error": "cancelled"}] * len(request["items"])})
        continue
    results = []
    for item in request["items"]:
        if item["text"] == "FAIL":
            results.append({"ok": False, "error": "boom"})
            continue
        words = item["text"].split()
        try:
            with wave.open(item["wav"], "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(12000)
                w.writeframes(b"\x00\x10" * (6000 * len(words)))
            with open(item["tsv"], "w") as f:
                f.write("word\tstart_sec\tend_sec\n")
                for i, word in enumerate(words):
                    f.write(f"{word}\t{i * 0.5:.3f}\t{i * 0.5 + 0.4:.3f}\n")
        except OSError as error:  # the folder went away with a cancelled run
            results.append({"ok": False, "error": str(error)})
            continue
        results.append({"ok": True, "seconds": 0.5 * len(words), "aligned": True})
    reply({"event": "done", "req": request["req"], "items": results})
)PY";
}

FakeHttpServer::FakeHttpServer(Handler handler) : m_handler(std::move(handler)), m_server(new QTcpServer) {
    m_server->listen(QHostAddress::LocalHost, 0);
    QObject::connect(m_server.get(), &QTcpServer::newConnection, m_server.get(), [this] {
        while (QTcpSocket *socket = m_server->nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                buffer->append(socket->readAll());
                const qsizetype end = buffer->indexOf("\r\n\r\n");
                if (end < 0)
                    return;
                Request request;
                const QList<QByteArray> lines = buffer->left(end).split('\n');
                const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
                request.method = first.value(0);
                request.target = first.value(1);
                for (qsizetype i = 1; i < lines.size(); ++i) {
                    const qsizetype colon = lines.at(i).indexOf(':');
                    if (colon > 0)
                        request.headers.insert(lines.at(i).left(colon).trimmed().toLower(),
                                               lines.at(i).mid(colon + 1).trimmed());
                }
                const qsizetype length = request.headers.value("content-length").toLongLong();
                if (buffer->size() < end + 4 + length)
                    return;
                request.body = buffer->mid(end + 4, length);
                buffer->clear();
                requests.append(request);
                const Response response = m_handler(request);
                QByteArray out = "HTTP/1.1 " + QByteArray::number(response.status) + " X\r\n";
                for (const auto &[name, value] : response.headers)
                    out += name + ": " + value + "\r\n";
                out += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\nConnection: close\r\n\r\n";
                socket->write(out + response.body);
                socket->disconnectFromHost();
            });
        }
    });
}

FakeHttpServer::~FakeHttpServer() = default;

QString FakeHttpServer::url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server->serverPort()); }

// Google Drive, in memory.

namespace {

QJsonObject driveMetadata(const FakeDrive::File &f) {
    QJsonObject properties;
    for (auto it = f.properties.cbegin(); it != f.properties.cend(); ++it)
        properties.insert(it.key(), it.value());
    return QJsonObject{{"id", f.id},
                       {"name", f.name},
                       {"mimeType", f.mimeType},
                       {"properties", properties},
                       {"headRevisionId", "rev" + QString::number(f.revision)},
                       {"sha256Checksum",
                        QString::fromLatin1(QCryptographicHash::hash(f.data, QCryptographicHash::Sha256).toHex())},
                       {"size", QString::number(f.data.size())},
                       {"createdTime", f.created},
                       {"modifiedTime", f.modified},
                       {"trashed", f.trashed}};
}

FakeHttpServer::Response json(const QJsonObject &object, int status = 200) {
    return {status, QJsonDocument(object).toJson(QJsonDocument::Compact), {}};
}

// The text between key and the next quote in a Drive query.
QString valueAfter(const QString &q, const QString &key) {
    const qsizetype at = q.indexOf(key);
    if (at < 0)
        return {};
    const qsizetype start = at + key.size();
    return q.mid(start, q.indexOf('\'', start) - start);
}

}

QString FakeDrive::stamp() { return QStringLiteral("2026-11-10T00:%1:00.000Z").arg(++clock, 2, 10, QChar('0')); }

FakeDrive::File *FakeDrive::file(const QString &id) {
    for (File &f : files)
        if (f.id == id)
            return &f;
    return nullptr;
}

QList<FakeDrive::File *> FakeDrive::withPath(const QString &path) {
    QList<File *> found;
    for (File &f : files)
        if (f.properties.value("omareaderPath") == path)
            found.append(&f);
    return found;
}

FakeDrive::File &FakeDrive::add(const QString &path, const QByteArray &data, const QString &parent) {
    File f;
    f.id = QStringLiteral("f%1").arg(++nextId, 3, 10, QChar('0'));
    f.name = path.section('/', -1);
    f.mimeType = "application/json";
    f.parent = parent;
    f.properties = {{"omareader", "file"}, {"omareaderPath", path}};
    f.data = data;
    f.created = f.modified = stamp();
    files.append(f);
    return files.last();
}

FakeHttpServer::Response FakeDrive::handle(const FakeHttpServer::Request &r) {
    const QUrl url("http://drive" + QString::fromLatin1(r.target));
    const QUrlQuery query(url);
    const QString path = url.path();
    if (path == "/token") {
        ++tokenRequests;
        lastTokenRequest = r.body;
        return json({{"access_token", token}, {"expires_in", 3600}});
    }
    if (path == "/revoke")
        return {200, "", {}};
    if (path.startsWith("/session/")) {
        if (!sessions.contains(path) || expireSession) {
            expireSession = false;
            sessions.remove(path);
            return json({{"error", QJsonObject{{"code", 404}, {"message", "session expired"}}}}, 404);
        }
        Session &s = sessions[path];
        const QByteArray range = r.headers.value("content-range");
        auto incomplete = [&s]() -> FakeHttpServer::Response {
            if (s.received.isEmpty())
                return {308, "", {}};
            return {308, "", {{"Range", "bytes=0-" + QByteArray::number(s.received.size() - 1)}}};
        };
        if (range.startsWith("bytes */"))
            return incomplete();
        const qint64 from = range.mid(6, range.indexOf('-') - 6).toLongLong();
        if (losePiece && from > 0) {
            losePiece = false;
            return {503, "", {}};
        }
        if (from != s.received.size())
            return {400, "", {}};
        s.received.append(r.body);
        if (s.received.size() < s.total)
            return incomplete();
        const QJsonObject metadata = QJsonDocument::fromJson(s.metadata).object();
        File *f = s.fileId.isEmpty() ? nullptr : file(s.fileId);
        if (!f) {
            const QString filePath = metadata.value("properties").toObject().value("omareaderPath").toString();
            f = &add(filePath, {}, metadata.value("parents").toArray().first().toString());
        } else {
            ++f->revision;
            f->modified = stamp();
        }
        if (metadata.contains("name"))
            f->name = metadata.value("name").toString();
        f->data = s.received;
        const QJsonObject done = driveMetadata(*f);
        sessions.remove(path);
        return json(done);
    }
    if (r.headers.value("authorization") != "Bearer " + token.toUtf8())
        return json({{"error", QJsonObject{{"code", 401}, {"message", "Invalid Credentials"}}}}, 401);
    if (busy > 0) {
        --busy;
        return json({{"error", QJsonObject{{"code", 429}, {"message", "Rate limit exceeded"}}}}, 429);
    }
    if (path == "/drive/v3/about")
        return json({{"user", QJsonObject{{"displayName", "Ana"}, {"emailAddress", "ana@example.com"}}}});

    if (path == "/drive/v3/files" && r.method == "GET") {
        const QString q = query.queryItemValue("q", QUrl::FullyDecoded);
        const bool folders = q.contains("value='folder'");
        const QString wanted = valueAfter(q, "key='omareaderPath' and value='");
        QJsonArray found;
        for (const File &f : std::as_const(files)) {
            if (f.trashed || f.properties.value("omareader") != (folders ? "folder" : "file"))
                continue;
            if (!wanted.isEmpty() && f.properties.value("omareaderPath") != wanted)
                continue;
            found.append(driveMetadata(f));
        }
        // Pages of two, so the client has to follow them.
        const int from = query.queryItemValue("pageToken").toInt();
        QJsonObject page{{"files", QJsonArray()}};
        QJsonArray slice;
        for (int i = from; i < qMin(from + 2, int(found.size())); ++i)
            slice.append(found.at(i));
        page.insert("files", slice);
        if (from + 2 < found.size())
            page.insert("nextPageToken", QString::number(from + 2));
        return json(page);
    }
    if (path == "/drive/v3/files" && r.method == "POST") {
        const QJsonObject metadata = QJsonDocument::fromJson(r.body).object();
        File f;
        f.id = QStringLiteral("d%1").arg(++nextId, 3, 10, QChar('0'));
        f.name = metadata.value("name").toString();
        f.mimeType = metadata.value("mimeType").toString();
        const QJsonObject properties = metadata.value("properties").toObject();
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it)
            f.properties.insert(it.key(), it.value().toString());
        f.created = f.modified = stamp();
        files.append(f);
        return json({{"id", f.id}});
    }
    if (path.startsWith("/drive/v3/files/")) {
        File *f = file(path.section('/', -1));
        if (!f)
            return json({{"error", QJsonObject{{"code", 404}, {"message", "File not found"}}}}, 404);
        if (r.method == "PATCH") {
            if (QJsonDocument::fromJson(r.body).object().value("trashed").toBool())
                f->trashed = true;
            return json({{"id", f->id}});
        }
        if (query.queryItemValue("alt") == "media")
            return {200, f->data, {}};
        return json(driveMetadata(*f));
    }
    if (path.startsWith("/upload/drive/v3/files")) {
        const QString type = query.queryItemValue("uploadType");
        const QString id = path == "/upload/drive/v3/files" ? QString() : path.section('/', -1);
        if (!id.isEmpty() && !file(id))
            return json({{"error", QJsonObject{{"code", 404}, {"message", "File not found"}}}}, 404);
        if (type == "resumable") {
            const QString link = QStringLiteral("/session/%1").arg(sessions.size() + nextId + 1);
            sessions.insert(link, {id, r.body, r.headers.value("x-upload-content-length").toLongLong(), {}});
            return {200, "", {{"Location", (base + link).toUtf8()}}};
        }
        if (type == "media" && !id.isEmpty()) {
            File *f = file(id);
            f->data = r.body;
            ++f->revision;
            f->modified = stamp();
            return json(driveMetadata(*f));
        }
        if (type == "multipart" && id.isEmpty()) {
            const QByteArray contentType = r.headers.value("content-type");
            const QByteArray boundary = "--" + contentType.mid(contentType.indexOf("boundary=") + 9);
            const qsizetype first = r.body.indexOf("\r\n\r\n") + 4;
            const qsizetype firstEnd = r.body.indexOf("\r\n" + boundary, first);
            const qsizetype second = r.body.indexOf("\r\n\r\n", firstEnd) + 4;
            const qsizetype secondEnd = r.body.indexOf("\r\n" + boundary, second);
            const QJsonObject metadata = QJsonDocument::fromJson(r.body.mid(first, firstEnd - first)).object();
            File &f = add(metadata.value("properties").toObject().value("omareaderPath").toString(),
                          r.body.mid(second, secondEnd - second), metadata.value("parents").toArray().first().toString());
            f.name = metadata.value("name").toString();
            return json(driveMetadata(f));
        }
    }
    return {404, "", {}};
}
