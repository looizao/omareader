#pragma once

#include "syncengine.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTimer>
#include <memory>

class QNetworkAccessManager;
class QTcpServer;
class QTcpSocket;

// What tells one OAuth provider from another: where to send the user, where
// to trade codes for tokens, and where the browser comes back to. Dropbox
// and Google Drive are each one of these (docs/sync.md).
struct OAuthClient {
    QString name; // shown to the user: "Dropbox", "Google Drive"
    QString clientId;
    QString clientSecret; // Google's desktop clients have one; Dropbox's PKCE app doesn't
    QString authorizeUrl;
    QString tokenUrl;
    QString redirectHost; // "localhost" or "127.0.0.1", as the provider registered it
    QString redirectPath; // "/dropbox", "/drive"
    QList<quint16> ports; // fixed ports to try in turn, or {0} for any free one
    QList<QPair<QString, QString>> authorizeExtras;

    QString redirectUri(quint16 port) const;
};

// A form body, every value percent-encoded.
QByteArray formBody(const QList<QPair<QString, QString>> &fields);

// One HTTP exchange, blocking until it ends. Each session creates its network
// access on the thread that first uses it, so a session belongs to one
// worker thread.
class HttpSession {
public:
    struct Reply {
        int status = 0;
        QByteArray body;
        QHash<QByteArray, QByteArray> headers; // names in lower case
        QString networkError;

        QByteArray header(const QByteArray &name) const { return headers.value(name.toLower()); }
    };
    using Headers = QList<QPair<QByteArray, QByteArray>>;

    HttpSession();
    ~HttpSession();
    Reply send(const QByteArray &verb, const QString &url, const QByteArray &contentType, const QByteArray &body,
               const Headers &headers = {});

private:
    std::unique_ptr<QNetworkAccessManager> m_network;
};

// A sync folder behind an OAuth sign-in. It keeps the refresh token, trades
// it for access tokens as they expire, and says when the provider has
// stopped honouring it.
class OAuthRemote : public SyncRemote {
public:
    OAuthRemote(const OAuthClient &client, const QString &refreshToken);

    QString error() const override { return m_error; }
    // The connected account's name and email, for the settings.
    virtual bool account(QString *name, QString *email) = 0;
    // Ends the connection on the provider's side.
    virtual bool revoke() = 0;
    // True once the provider has refused the refresh token: the user revoked
    // it or removed the app, and has to connect again.
    bool tokenRevoked() const { return m_revoked; }

    void setAccessToken(const QString &token, qint64 expiresAtMs);
    QString accessToken() const { return m_accessToken; }
    qint64 accessTokenExpiry() const { return m_expiresAt; }

protected:
    bool ensureToken();
    bool refresh();
    void dropAccessToken() { m_accessToken.clear(); }
    QByteArray bearer() const { return "Bearer " + m_accessToken.toUtf8(); }

    OAuthClient m_client;
    QString m_refreshToken;
    QString m_accessToken;
    qint64 m_expiresAt = 0;
    QString m_error;
    bool m_revoked = false;
    HttpSession m_http;
};

// Connecting (docs/sync.md): OAuth 2 with PKCE, the browser sent back to a
// listener on the loopback address. One answer is accepted, and the attempt
// gives up after five minutes.
class LoopbackAuth : public QObject {
    Q_OBJECT

public:
    explicit LoopbackAuth(const OAuthClient &client, QObject *parent = nullptr);
    ~LoopbackAuth() override;

    // Starts listening and returns the page to open in the browser, or an
    // empty string when no port is free.
    QString start();
    void cancel();
    bool running() const { return !m_servers.isEmpty(); }
    QString providerName() const { return m_client.name; }

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

    OAuthClient m_client;
    QList<QTcpServer *> m_servers;
    quint16 m_port = 0;
    QString m_verifier;
    QString m_state;
    QTimer m_timeout;
    QNetworkAccessManager *m_network = nullptr;
};
