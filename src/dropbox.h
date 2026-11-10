#pragma once

#include "oauth.h"

#include <QJsonObject>

// Where Dropbox lives; tests point these at a server of their own.
struct DropboxHosts {
    QString api = QStringLiteral("https://api.dropboxapi.com");
    QString content = QStringLiteral("https://content.dropboxapi.com");
    QString authorize = QStringLiteral("https://www.dropbox.com/oauth2/authorize");
};

// The app key that names omareader to Dropbox, from DROPBOX_APP_KEY at build
// time; empty in a build without Dropbox.
QString dropboxAppKey();
// Dropbox as an OAuth provider: PKCE without a secret, and the browser sent
// back to one of three fixed ports on localhost, which is all Dropbox allows.
OAuthClient dropboxClient(const QString &appKey, const DropboxHosts &hosts = {});

// The app's Dropbox folder over Dropbox's HTTP API. Each call blocks until
// Dropbox answers, so it belongs on a worker thread.
class DropboxRemote : public OAuthRemote {
public:
    DropboxRemote(const QString &appKey, const QString &refreshToken, const DropboxHosts &hosts = {});

    Status list(QList<RemoteFile> *files) override;
    Status download(const QString &path, QByteArray *data, QString *rev) override;
    Status upload(const QString &path, const QByteArray &data, const QString &rev, QString *newRev) override;
    Status uploadFile(const QString &path, const QString &localFile, QString *newRev) override;
    Status remove(const QString &path) override;
    bool account(QString *name, QString *email) override;
    bool revoke() override;

    // Pieces of a large upload; Dropbox takes at most 150 MiB per request.
    void setChunkSize(qint64 bytes) { m_chunk = bytes; }
    void setSingleUploadLimit(qint64 bytes) { m_singleLimit = bytes; }

private:
    enum Host { Api, Content };
    using Reply = HttpSession::Reply;

    // A call with the access token, refreshing it once on 401 and waiting out
    // one short 429 or 5xx.
    Reply call(Host host, const QString &endpoint, const QByteArray &contentType, const QByteArray &body,
               const HttpSession::Headers &headers = {});
    Reply rpc(const QString &endpoint, const QJsonObject &arguments);
    Reply content(const QString &endpoint, const QJsonObject &arguments, const QByteArray &body = {});
    Status fail(const Reply &reply);
    static QString summary(const Reply &reply);
    static RemoteFile fileFrom(const QJsonObject &metadata);
    static QJsonObject writeMode(const QString &rev);

    DropboxHosts m_hosts;
    qint64 m_chunk = 64LL * 1024 * 1024;
    qint64 m_singleLimit = 150LL * 1024 * 1024;
};

// Connecting to Dropbox (docs/sync.md).
class DropboxAuth : public LoopbackAuth {
public:
    explicit DropboxAuth(const QString &appKey, QObject *parent = nullptr, const DropboxHosts &hosts = {});
    static const QList<quint16> &ports();
};
