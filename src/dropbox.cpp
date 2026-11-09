#include "dropbox.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#ifndef OMAREADER_DROPBOX_KEY
#define OMAREADER_DROPBOX_KEY ""
#endif

QString dropboxAppKey() {
    // Tests and a development build can name another Dropbox app.
    const QByteArray override = qgetenv("OMAREADER_DROPBOX_KEY");
    return override.isEmpty() ? QStringLiteral(OMAREADER_DROPBOX_KEY).trimmed() : QString::fromUtf8(override).trimmed();
}

namespace {

QString redirectUri(quint16 port) { return QStringLiteral("http://localhost:%1/dropbox").arg(port); }

QByteArray form(const QList<QPair<QString, QString>> &fields) {
    QUrlQuery query;
    for (const auto &[key, value] : fields)
        query.addQueryItem(key, QString::fromLatin1(QUrl::toPercentEncoding(value)));
    return query.query(QUrl::FullyEncoded).toUtf8();
}

}

// The remote.

DropboxRemote::DropboxRemote(const QString &appKey, const QString &refreshToken, const DropboxHosts &hosts)
    : m_appKey(appKey), m_refreshToken(refreshToken), m_hosts(hosts) {}

DropboxRemote::~DropboxRemote() = default;

void DropboxRemote::setAccessToken(const QString &token, qint64 expiresAtMs) {
    m_accessToken = token;
    m_expiresAt = expiresAtMs;
}

DropboxRemote::Reply DropboxRemote::send(const QString &url, const QByteArray &contentType, const QByteArray &body,
                                         const QList<QPair<QByteArray, QByteArray>> &headers) {
    if (!m_network)
        m_network = std::make_unique<QNetworkAccessManager>();
    QNetworkRequest request{QUrl(url)};
    // Gives up on a connection that stops moving, not on a slow one.
    request.setTransferTimeout(120000);
    if (!contentType.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    for (const auto &[name, value] : headers)
        request.setRawHeader(name, value);
    QNetworkReply *reply = m_network->post(request, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        loop.exec();
    Reply out;
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.body = reply->readAll();
    out.result = reply->rawHeader("Dropbox-API-Result");
    out.retryAfter = reply->rawHeader("Retry-After");
    if (out.status == 0)
        out.networkError = reply->errorString();
    reply->deleteLater();
    return out;
}

bool DropboxRemote::refresh() {
    const Reply reply = send(m_hosts.api + "/oauth2/token", "application/x-www-form-urlencoded",
                             form({{"grant_type", "refresh_token"},
                                   {"refresh_token", m_refreshToken},
                                   {"client_id", m_appKey}}),
                             {});
    if (reply.status == 200) {
        const QJsonObject object = QJsonDocument::fromJson(reply.body).object();
        m_accessToken = object.value("access_token").toString();
        // A minute early, so a token never expires halfway through a call.
        m_expiresAt = QDateTime::currentMSecsSinceEpoch() + (object.value("expires_in").toInteger(14400) - 60) * 1000;
        return !m_accessToken.isEmpty();
    }
    // invalid_grant: the refresh token was revoked or the app removed.
    if (reply.status == 400 || reply.status == 401) {
        const QString error = QJsonDocument::fromJson(reply.body).object().value("error").toString();
        if (error == "invalid_grant" || reply.status == 401)
            m_revoked = true;
    }
    m_error = reply.status == 0 ? reply.networkError
                                : QStringLiteral("Dropbox refused the sign-in (%1)").arg(summary(reply));
    return false;
}

bool DropboxRemote::ensureToken() {
    if (!m_accessToken.isEmpty() && QDateTime::currentMSecsSinceEpoch() < m_expiresAt)
        return true;
    return refresh();
}

DropboxRemote::Reply DropboxRemote::call(Host host, const QString &endpoint, const QByteArray &contentType,
                                         const QByteArray &body, const QList<QPair<QByteArray, QByteArray>> &headers) {
    const QString url = (host == Api ? m_hosts.api : m_hosts.content) + "/2/" + endpoint;
    bool refreshed = false, waited = false;
    while (true) {
        if (!ensureToken()) {
            Reply failed;
            failed.networkError = m_error;
            return failed;
        }
        QList<QPair<QByteArray, QByteArray>> all = headers;
        all.append({"Authorization", "Bearer " + m_accessToken.toUtf8()});
        const Reply reply = send(url, contentType, body, all);
        if (reply.status == 401 && !refreshed) {
            refreshed = true;
            m_accessToken.clear();
            continue;
        }
        // One short wait for a busy Dropbox; longer ones wait for the next sync.
        if ((reply.status == 429 || reply.status >= 500) && !waited) {
            const int seconds = reply.retryAfter.isEmpty() ? 2 : reply.retryAfter.toInt();
            if (seconds <= 15) {
                waited = true;
                QThread::sleep(std::chrono::seconds(qMax(1, seconds)));
                continue;
            }
        }
        return reply;
    }
}

DropboxRemote::Reply DropboxRemote::rpc(const QString &endpoint, const QJsonObject &arguments) {
    const QByteArray body =
        arguments.isEmpty() ? QByteArray("null") : QJsonDocument(arguments).toJson(QJsonDocument::Compact);
    return call(Api, endpoint, "application/json", body);
}

DropboxRemote::Reply DropboxRemote::content(const QString &endpoint, const QJsonObject &arguments,
                                            const QByteArray &body) {
    // A download sends no body, and Dropbox refuses the form type Qt would
    // otherwise pick for an empty POST; this is the type Dropbox documents
    // for exactly that.
    return call(Content, endpoint,
                body.isNull() ? QByteArray("text/plain; charset=dropbox-cors-hack")
                              : QByteArray("application/octet-stream"),
                body,
                {{"Dropbox-API-Arg", SyncFormat::headerJson(arguments)}});
}

QString DropboxRemote::summary(const Reply &reply) {
    const QJsonObject object = QJsonDocument::fromJson(reply.body).object();
    QString text = object.value("error_summary").toString();
    if (text.isEmpty())
        text = object.value("error_description").toString();
    if (text.isEmpty())
        text = object.value("error").toString();
    if (text.isEmpty())
        text = QString::fromUtf8(reply.body.left(200)).simplified();
    return QStringLiteral("HTTP %1: %2").arg(reply.status).arg(text);
}

SyncRemote::Status DropboxRemote::fail(const Reply &reply) {
    if (reply.status == 0)
        m_error = reply.networkError.isEmpty() ? QStringLiteral("Dropbox could not be reached") : reply.networkError;
    else
        m_error = QStringLiteral("Dropbox said %1").arg(summary(reply));
    if (reply.status == 409) {
        const QString text = QJsonDocument::fromJson(reply.body).object().value("error_summary").toString();
        if (text.startsWith("path/conflict"))
            return Conflict;
        if (text.startsWith("path/not_found") || text.startsWith("path_lookup/not_found"))
            return NotFound;
    }
    return Failed;
}

RemoteFile DropboxRemote::fileFrom(const QJsonObject &metadata) {
    RemoteFile file;
    file.path = metadata.value("path_lower").toString();
    file.rev = metadata.value("rev").toString();
    file.contentHash = metadata.value("content_hash").toString();
    file.size = metadata.value("size").toInteger();
    return file;
}

QJsonObject DropboxRemote::writeMode(const QString &rev) {
    // Update must be spelled out as an object; add can be a bare tag.
    return QJsonObject{{".tag", "update"}, {"update", rev}};
}

SyncRemote::Status DropboxRemote::list(QList<RemoteFile> *files) {
    files->clear();
    Reply reply = rpc("files/list_folder", QJsonObject{{"path", ""}, {"recursive", true}, {"limit", 2000}});
    while (true) {
        if (reply.status != 200)
            return fail(reply);
        const QJsonObject object = QJsonDocument::fromJson(reply.body).object();
        for (const QJsonValue &value : object.value("entries").toArray()) {
            const QJsonObject entry = value.toObject();
            if (entry.value(".tag").toString() == "file")
                files->append(fileFrom(entry));
        }
        if (!object.value("has_more").toBool())
            return Ok;
        reply = rpc("files/list_folder/continue", QJsonObject{{"cursor", object.value("cursor").toString()}});
    }
}

SyncRemote::Status DropboxRemote::download(const QString &path, QByteArray *data, QString *rev) {
    const Reply reply = content("files/download", QJsonObject{{"path", path}});
    if (reply.status != 200)
        return fail(reply);
    *data = reply.body;
    if (rev)
        *rev = QJsonDocument::fromJson(reply.result).object().value("rev").toString();
    return Ok;
}

SyncRemote::Status DropboxRemote::upload(const QString &path, const QByteArray &data, const QString &rev,
                                         QString *newRev) {
    QJsonObject arguments{{"path", path}, {"mute", true}};
    if (rev.isEmpty()) {
        arguments.insert("mode", "add");
    } else {
        arguments.insert("mode", writeMode(rev));
        // Without it Dropbox would write over a file another device deleted.
        arguments.insert("strict_conflict", true);
    }
    const Reply reply = content("files/upload", arguments, data.isNull() ? QByteArray("") : data);
    if (reply.status != 200)
        return fail(reply);
    if (newRev)
        *newRev = QJsonDocument::fromJson(reply.body).object().value("rev").toString();
    return Ok;
}

SyncRemote::Status DropboxRemote::uploadFile(const QString &path, const QString &localFile, QString *newRev) {
    QFile file(localFile);
    if (!file.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("Could not read %1").arg(localFile);
        return Failed;
    }
    const qint64 size = file.size();
    const QJsonObject commit{{"path", path}, {"mode", "overwrite"}, {"mute", true}};
    if (size <= qMin(m_chunk, m_singleLimit)) {
        const Reply reply = content("files/upload", commit, file.readAll());
        if (reply.status != 200)
            return fail(reply);
        if (newRev)
            *newRev = QJsonDocument::fromJson(reply.body).object().value("rev").toString();
        return Ok;
    }

    // Larger books go in pieces through an upload session, one piece in
    // memory at a time.
    QByteArray piece = file.read(m_chunk);
    Reply reply = content("files/upload_session/start", QJsonObject{{"close", false}}, piece);
    if (reply.status != 200)
        return fail(reply);
    const QString session = QJsonDocument::fromJson(reply.body).object().value("session_id").toString();
    qint64 offset = piece.size();
    while (size - offset > m_chunk) {
        piece = file.read(m_chunk);
        reply = content("files/upload_session/append_v2",
                        QJsonObject{{"cursor", QJsonObject{{"session_id", session}, {"offset", offset}}},
                                    {"close", false}},
                        piece);
        if (reply.status != 200)
            return fail(reply);
        offset += piece.size();
    }
    piece = file.readAll();
    reply = content("files/upload_session/finish",
                    QJsonObject{{"cursor", QJsonObject{{"session_id", session}, {"offset", offset}}}, {"commit", commit}},
                    piece.isNull() ? QByteArray("") : piece);
    if (reply.status != 200)
        return fail(reply);
    if (newRev)
        *newRev = QJsonDocument::fromJson(reply.body).object().value("rev").toString();
    return Ok;
}

SyncRemote::Status DropboxRemote::remove(const QString &path) {
    const Reply reply = rpc("files/delete_v2", QJsonObject{{"path", path}});
    return reply.status == 200 ? Ok : fail(reply);
}

bool DropboxRemote::account(QString *name, QString *email) {
    const Reply reply = rpc("users/get_current_account", {});
    if (reply.status != 200) {
        fail(reply);
        return false;
    }
    const QJsonObject object = QJsonDocument::fromJson(reply.body).object();
    *name = object.value("name").toObject().value("display_name").toString();
    *email = object.value("email").toString();
    return true;
}

bool DropboxRemote::revoke() {
    const Reply reply = rpc("auth/token/revoke", {});
    if (reply.status != 200 && reply.status != 401) {
        fail(reply);
        return false;
    }
    return true;
}

// Connecting.

DropboxAuth::DropboxAuth(const QString &appKey, QObject *parent, const DropboxHosts &hosts)
    : QObject(parent), m_appKey(appKey), m_hosts(hosts) {
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(5 * 60 * 1000);
    connect(&m_timeout, &QTimer::timeout, this, [this] { finish(tr("Dropbox didn't answer in time. Try again.")); });
}

DropboxAuth::~DropboxAuth() { stopListening(); }

const QList<quint16> &DropboxAuth::ports() {
    static const QList<quint16> list{53682, 53683, 53684};
    return list;
}

QString DropboxAuth::randomText(int length) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    QString text;
    for (int i = 0; i < length; ++i)
        text.append(QChar(alphabet[QRandomGenerator::system()->bounded(int(sizeof(alphabet) - 1))]));
    return text;
}

QString DropboxAuth::challenge(const QString &verifier) {
    return QString::fromLatin1(QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
                                   .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString DropboxAuth::start() {
    cancel();
    // localhost may resolve to either loopback address, so listen on both
    // (IPv6 may be missing, which is fine), and never on the network.
    for (const quint16 port : ports()) {
        auto *v4 = new QTcpServer(this);
        if (!v4->listen(QHostAddress::LocalHost, port)) {
            delete v4;
            continue;
        }
        m_servers.append(v4);
        auto *v6 = new QTcpServer(this);
        if (v6->listen(QHostAddress::LocalHostIPv6, port))
            m_servers.append(v6);
        else
            delete v6;
        m_port = port;
        break;
    }
    if (m_servers.isEmpty())
        return {};
    for (QTcpServer *server : std::as_const(m_servers))
        connect(server, &QTcpServer::newConnection, this, &DropboxAuth::onConnection);
    m_verifier = randomText(64);
    m_state = randomText(32);
    m_timeout.start();

    QUrlQuery query;
    query.addQueryItem("client_id", m_appKey);
    query.addQueryItem("response_type", "code");
    query.addQueryItem("code_challenge", challenge(m_verifier));
    query.addQueryItem("code_challenge_method", "S256");
    query.addQueryItem("token_access_type", "offline");
    query.addQueryItem("redirect_uri", redirectUri(m_port));
    query.addQueryItem("state", m_state);
    QUrl url(m_hosts.authorize);
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

void DropboxAuth::cancel() {
    m_timeout.stop();
    stopListening();
    m_verifier.clear();
    m_state.clear();
}

void DropboxAuth::stopListening() {
    for (QTcpServer *server : std::as_const(m_servers)) {
        server->close();
        server->deleteLater();
    }
    m_servers.clear();
}

void DropboxAuth::onConnection() {
    auto *server = qobject_cast<QTcpServer *>(sender());
    while (server && server->hasPendingConnections()) {
        QTcpSocket *socket = server->nextPendingConnection();
        // Accepted sockets belong to their server, which goes away as soon as
        // the answer arrives; the page still has to reach the browser.
        socket->setParent(this);
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onRequest(socket); });
    }
}

void DropboxAuth::onRequest(QTcpSocket *socket) {
    // Only the request line matters; a browser sends it in its first packet.
    if (!socket->canReadLine() && socket->bytesAvailable() < 8192)
        return;
    const QList<QByteArray> parts = socket->readLine(8192).trimmed().split(' ');
    const QUrl url(QString::fromLatin1(parts.value(1)));
    auto respond = [socket](int status, const QString &title, const QString &text) {
        const QByteArray body =
            QStringLiteral("<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width\">"
                           "<title>omareader</title><body style=\"font:16px system-ui;margin:3em auto;max-width:32em;"
                           "padding:0 1em\"><h1 style=\"font-size:1.4em\">%1</h1><p>%2</p>")
                .arg(title.toHtmlEscaped(), text.toHtmlEscaped())
                .toUtf8();
        socket->write(QStringLiteral("HTTP/1.1 %1 %2\r\nContent-Type: text/html; charset=utf-8\r\n"
                                     "Content-Length: %3\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n")
                          .arg(status)
                          .arg(status == 200 ? "OK" : "Not Found")
                          .arg(body.size())
                          .toLatin1()
                      + body);
        socket->disconnectFromHost();
    };
    if (parts.value(0) != "GET" || url.path() != "/dropbox") {
        respond(404, tr("Not here"), tr("omareader is waiting for Dropbox."));
        return;
    }
    const QUrlQuery query(url);
    if (m_state.isEmpty() || query.queryItemValue("state") != m_state) {
        respond(404, tr("Not here"), tr("This sign-in isn't the one omareader started."));
        return;
    }
    const QString code = query.queryItemValue("code");
    if (code.isEmpty()) {
        const QString reason = query.queryItemValue("error_description", QUrl::FullyDecoded);
        respond(200, tr("Dropbox isn't connected"), tr("You can close this page and try again in omareader."));
        finish(reason.isEmpty() ? tr("Dropbox wasn't connected.") : reason);
        return;
    }
    respond(200, tr("Dropbox is connected"), tr("You can close this page and go back to omareader."));
    stopListening();
    exchange(code);
}

void DropboxAuth::exchange(const QString &code) {
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    QNetworkRequest request{QUrl(m_hosts.api + "/oauth2/token")};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setTransferTimeout(60000);
    QNetworkReply *reply = m_network->post(request, form({{"code", code},
                                                          {"grant_type", "authorization_code"},
                                                          {"code_verifier", m_verifier},
                                                          {"client_id", m_appKey},
                                                          {"redirect_uri", redirectUri(m_port)}}));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
        const QString refresh = object.value("refresh_token").toString();
        if (status != 200 || refresh.isEmpty()) {
            const QString reason = object.value("error_description").toString();
            finish(status == 0 ? reply->errorString()
                               : tr("Dropbox refused the sign-in: %1")
                                     .arg(reason.isEmpty() ? QString::number(status) : reason));
            return;
        }
        m_timeout.stop();
        m_verifier.clear();
        m_state.clear();
        emit connected(refresh, object.value("access_token").toString(),
                       int(object.value("expires_in").toInteger(14400)));
    });
}

void DropboxAuth::finish(const QString &error) {
    cancel();
    emit failed(error);
}
