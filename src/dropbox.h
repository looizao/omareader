#pragma once

#include "syncengine.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <memory>

class QNetworkAccessManager;
class QTcpServer;
class QTcpSocket;

// Where Dropbox lives; tests point these at a server of their own.
struct DropboxHosts {
    QString api = QStringLiteral("https://api.dropboxapi.com");
    QString content = QStringLiteral("https://content.dropboxapi.com");
    QString authorize = QStringLiteral("https://www.dropbox.com/oauth2/authorize");
};

// The app key that names omareader to Dropbox, from DROPBOX_APP_KEY at build
// time; empty in a build without sync.
QString dropboxAppKey();

// The app's Dropbox folder over Dropbox's HTTP API. Each call blocks until
// Dropbox answers, so it belongs on a worker thread, and creates its network
// access on the thread that first uses it. The access token is refreshed
// from the refresh token whenever Dropbox says it has expired.
class DropboxRemote : public SyncRemote {
public:
    DropboxRemote(const QString &appKey, const QString &refreshToken, const DropboxHosts &hosts = {});
    ~DropboxRemote() override;

    Status list(QList<RemoteFile> *files) override;
    Status download(const QString &path, QByteArray *data, QString *rev) override;
    Status upload(const QString &path, const QByteArray &data, const QString &rev, QString *newRev) override;
    Status uploadFile(const QString &path, const QString &localFile, QString *newRev) override;
    Status remove(const QString &path) override;
    QString error() const override { return m_error; }

    // The connected account's name and email, for the settings.
    bool account(QString *name, QString *email);
    // Ends the connection on Dropbox's side.
    bool revoke();
    // True once Dropbox has refused the refresh token: the user revoked it
    // or removed the app, and has to connect again.
    bool tokenRevoked() const { return m_revoked; }

    void setAccessToken(const QString &token, qint64 expiresAtMs);
    QString accessToken() const { return m_accessToken; }
    qint64 accessTokenExpiry() const { return m_expiresAt; }
    // Pieces of a large upload; Dropbox takes at most 150 MiB per request.
    void setChunkSize(qint64 bytes) { m_chunk = bytes; }
    void setSingleUploadLimit(qint64 bytes) { m_singleLimit = bytes; }

private:
    struct Reply {
        int status = 0;
        QByteArray body;
        QByteArray result; // Dropbox-API-Result, for content downloads
        QByteArray retryAfter;
        QString networkError;
    };
    enum Host { Api, Content };

    Reply send(const QString &url, const QByteArray &contentType, const QByteArray &body,
               const QList<QPair<QByteArray, QByteArray>> &headers);
    // A call with the access token, refreshing it once on 401 and waiting out
    // one short 429 or 5xx.
    Reply call(Host host, const QString &endpoint, const QByteArray &contentType, const QByteArray &body,
               const QList<QPair<QByteArray, QByteArray>> &headers = {});
    Reply rpc(const QString &endpoint, const QJsonObject &arguments);
    Reply content(const QString &endpoint, const QJsonObject &arguments, const QByteArray &body = {});
    bool refresh();
    bool ensureToken();
    Status fail(const Reply &reply);
    static QString summary(const Reply &reply);
    static RemoteFile fileFrom(const QJsonObject &metadata);
    static QJsonObject writeMode(const QString &rev);

    QString m_appKey;
    QString m_refreshToken;
    DropboxHosts m_hosts;
    QString m_accessToken;
    qint64 m_expiresAt = 0;
    QString m_error;
    bool m_revoked = false;
    qint64 m_chunk = 64LL * 1024 * 1024;
    qint64 m_singleLimit = 150LL * 1024 * 1024;
    std::unique_ptr<QNetworkAccessManager> m_network;
};

// Connecting to Dropbox (docs/sync.md): OAuth 2 with PKCE, the browser sent
// back to a listener on localhost at one of three fixed ports. One request
// is accepted, and the attempt gives up after five minutes.
class DropboxAuth : public QObject {
    Q_OBJECT

public:
    explicit DropboxAuth(const QString &appKey, QObject *parent = nullptr, const DropboxHosts &hosts = {});
    ~DropboxAuth() override;

    // Starts listening and returns the page to open in the browser, or an
    // empty string when none of the ports is free.
    QString start();
    void cancel();
    bool running() const { return !m_servers.isEmpty(); }

    static const QList<quint16> &ports();
    static QString challenge(const QString &verifier);
    // Exposed for tests: the listening port, and the expected state.
    quint16 port() const { return m_port; }
    QString state() const { return m_state; }
    void setTimeout(int ms) { m_timeout.setInterval(ms); }

signals:
    // The refresh token, and an access token good for expiresIn seconds.
    void connected(const QString &refreshToken, const QString &accessToken, int expiresIn);
    void failed(const QString &error);

private:
    void onConnection();
    void onRequest(QTcpSocket *socket);
    void exchange(const QString &code);
    void stopListening();
    void finish(const QString &error);
    static QString randomText(int length);

    QString m_appKey;
    DropboxHosts m_hosts;
    QList<QTcpServer *> m_servers;
    quint16 m_port = 0;
    QString m_verifier;
    QString m_state;
    QTimer m_timeout;
    QNetworkAccessManager *m_network = nullptr;
};
