#include "dropbox.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

#ifndef OMAREADER_DROPBOX_KEY
#define OMAREADER_DROPBOX_KEY ""
#endif

QString dropboxAppKey() {
    // Tests and a development build can name another Dropbox app.
    const QByteArray override = qgetenv("OMAREADER_DROPBOX_KEY");
    return override.isEmpty() ? QStringLiteral(OMAREADER_DROPBOX_KEY).trimmed() : QString::fromUtf8(override).trimmed();
}

OAuthClient dropboxClient(const QString &appKey, const DropboxHosts &hosts) {
    OAuthClient client;
    client.name = QStringLiteral("Dropbox");
    client.clientId = appKey;
    client.authorizeUrl = hosts.authorize;
    client.tokenUrl = hosts.api + "/oauth2/token";
    client.redirectHost = QStringLiteral("localhost");
    client.redirectPath = QStringLiteral("/dropbox");
    client.ports = DropboxAuth::ports();
    client.authorizeExtras = {{"token_access_type", "offline"}};
    return client;
}

// The remote.

DropboxRemote::DropboxRemote(const QString &appKey, const QString &refreshToken, const DropboxHosts &hosts)
    : OAuthRemote(dropboxClient(appKey, hosts), refreshToken), m_hosts(hosts) {}

DropboxRemote::Reply DropboxRemote::call(Host host, const QString &endpoint, const QByteArray &contentType,
                                         const QByteArray &body, const HttpSession::Headers &headers) {
    const QString url = (host == Api ? m_hosts.api : m_hosts.content) + "/2/" + endpoint;
    bool refreshed = false, waited = false;
    while (true) {
        if (!ensureToken()) {
            Reply failed;
            failed.networkError = m_error;
            return failed;
        }
        HttpSession::Headers all = headers;
        all.append({"Authorization", bearer()});
        const Reply reply = m_http.send("POST", url, contentType, body, all);
        if (reply.status == 401 && !refreshed) {
            refreshed = true;
            dropAccessToken();
            continue;
        }
        // One short wait for a busy Dropbox; longer ones wait for the next sync.
        if ((reply.status == 429 || reply.status >= 500) && !waited) {
            const QByteArray after = reply.header("Retry-After");
            const int seconds = after.isEmpty() ? 2 : after.toInt();
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
        *rev = QJsonDocument::fromJson(reply.header("Dropbox-API-Result")).object().value("rev").toString();
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
    : LoopbackAuth(dropboxClient(appKey, hosts), parent) {}

const QList<quint16> &DropboxAuth::ports() {
    static const QList<quint16> list{53682, 53683, 53684};
    return list;
}
