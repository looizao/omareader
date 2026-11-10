#include "oauth.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

QString OAuthClient::redirectUri(quint16 port) const {
    return QStringLiteral("http://%1:%2%3").arg(redirectHost).arg(port).arg(redirectPath);
}

QByteArray formBody(const QList<QPair<QString, QString>> &fields) {
    QUrlQuery query;
    for (const auto &[key, value] : fields)
        query.addQueryItem(key, QString::fromLatin1(QUrl::toPercentEncoding(value)));
    return query.query(QUrl::FullyEncoded).toUtf8();
}

// HTTP.

HttpSession::HttpSession() = default;
HttpSession::~HttpSession() = default;

HttpSession::Reply HttpSession::send(const QByteArray &verb, const QString &url, const QByteArray &contentType,
                                     const QByteArray &body, const Headers &headers) {
    if (!m_network)
        m_network = std::make_unique<QNetworkAccessManager>();
    QNetworkRequest request{QUrl(url)};
    // Gives up on a connection that stops moving, not on a slow one.
    request.setTransferTimeout(120000);
    // Drive answers a piece of a resumable upload with 308, meaning "go on",
    // not a redirect; neither provider redirects an API call otherwise.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!contentType.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    for (const auto &[name, value] : headers)
        request.setRawHeader(name, value);
    QNetworkReply *reply = m_network->sendCustomRequest(request, verb, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        loop.exec();
    Reply out;
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.body = reply->readAll();
    for (const auto &[name, value] : reply->rawHeaderPairs())
        out.headers.insert(name.toLower(), value);
    if (out.status == 0)
        out.networkError = reply->errorString();
    reply->deleteLater();
    return out;
}

// The remote's tokens.

OAuthRemote::OAuthRemote(const OAuthClient &client, const QString &refreshToken)
    : m_client(client), m_refreshToken(refreshToken) {}

void OAuthRemote::setAccessToken(const QString &token, qint64 expiresAtMs) {
    m_accessToken = token;
    m_expiresAt = expiresAtMs;
}

bool OAuthRemote::refresh() {
    QList<QPair<QString, QString>> fields{
        {"grant_type", "refresh_token"}, {"refresh_token", m_refreshToken}, {"client_id", m_client.clientId}};
    if (!m_client.clientSecret.isEmpty())
        fields.append({"client_secret", m_client.clientSecret});
    const HttpSession::Reply reply =
        m_http.send("POST", m_client.tokenUrl, "application/x-www-form-urlencoded", formBody(fields));
    const QJsonObject object = QJsonDocument::fromJson(reply.body).object();
    if (reply.status == 200) {
        m_accessToken = object.value("access_token").toString();
        // A minute early, so a token never expires halfway through a call.
        m_expiresAt = QDateTime::currentMSecsSinceEpoch() + (object.value("expires_in").toInteger(3600) - 60) * 1000;
        return !m_accessToken.isEmpty();
    }
    // invalid_grant: the refresh token was revoked, expired, or the app removed.
    if ((reply.status == 400 || reply.status == 401)
        && (object.value("error").toString() == "invalid_grant" || reply.status == 401))
        m_revoked = true;
    QString reason = object.value("error_description").toString();
    if (reason.isEmpty())
        reason = object.value("error").toString();
    m_error = reply.status == 0 ? reply.networkError
                                : QStringLiteral("%1 refused the sign-in (HTTP %2: %3)")
                                      .arg(m_client.name)
                                      .arg(reply.status)
                                      .arg(reason);
    return false;
}

bool OAuthRemote::ensureToken() {
    if (!m_accessToken.isEmpty() && QDateTime::currentMSecsSinceEpoch() < m_expiresAt)
        return true;
    return refresh();
}

// Connecting.

LoopbackAuth::LoopbackAuth(const OAuthClient &client, QObject *parent) : QObject(parent), m_client(client) {
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(5 * 60 * 1000);
    connect(&m_timeout, &QTimer::timeout, this,
            [this] { finish(tr("%1 didn't answer in time. Try again.").arg(m_client.name)); });
}

LoopbackAuth::~LoopbackAuth() { stopListening(); }

QString LoopbackAuth::randomText(int length) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    QString text;
    for (int i = 0; i < length; ++i)
        text.append(QChar(alphabet[QRandomGenerator::system()->bounded(int(sizeof(alphabet) - 1))]));
    return text;
}

QString LoopbackAuth::challenge(const QString &verifier) {
    return QString::fromLatin1(QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
                                   .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString LoopbackAuth::start() {
    cancel();
    // Only ever on the loopback address, never on the network. A redirect to
    // "localhost" may resolve to either loopback address, so that one also
    // listens on IPv6 when it can; a missing IPv6 is fine.
    for (const quint16 port : std::as_const(m_client.ports)) {
        auto *v4 = new QTcpServer(this);
        if (!v4->listen(QHostAddress::LocalHost, port)) {
            delete v4;
            continue;
        }
        m_servers.append(v4);
        m_port = v4->serverPort();
        if (m_client.redirectHost == "localhost") {
            auto *v6 = new QTcpServer(this);
            if (v6->listen(QHostAddress::LocalHostIPv6, m_port))
                m_servers.append(v6);
            else
                delete v6;
        }
        break;
    }
    if (m_servers.isEmpty())
        return {};
    for (QTcpServer *server : std::as_const(m_servers))
        connect(server, &QTcpServer::newConnection, this, &LoopbackAuth::onConnection);
    m_verifier = randomText(64);
    m_state = randomText(32);
    m_timeout.start();

    QUrlQuery query;
    query.addQueryItem("client_id", m_client.clientId);
    query.addQueryItem("response_type", "code");
    query.addQueryItem("redirect_uri", m_client.redirectUri(m_port));
    query.addQueryItem("code_challenge", challenge(m_verifier));
    query.addQueryItem("code_challenge_method", "S256");
    query.addQueryItem("state", m_state);
    for (const auto &[key, value] : std::as_const(m_client.authorizeExtras))
        query.addQueryItem(key, QString::fromLatin1(QUrl::toPercentEncoding(value)));
    QUrl url(m_client.authorizeUrl);
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

void LoopbackAuth::cancel() {
    m_timeout.stop();
    stopListening();
    m_verifier.clear();
    m_state.clear();
}

void LoopbackAuth::stopListening() {
    for (QTcpServer *server : std::as_const(m_servers)) {
        server->close();
        server->deleteLater();
    }
    m_servers.clear();
}

void LoopbackAuth::onConnection() {
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

void LoopbackAuth::onRequest(QTcpSocket *socket) {
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
    // Browsers also ask for a favicon; anything but the answer is turned
    // away and the listener keeps waiting.
    if (parts.value(0) != "GET" || url.path() != m_client.redirectPath) {
        respond(404, tr("Not here"), tr("omareader is waiting for %1.").arg(m_client.name));
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
        respond(200, tr("%1 isn't connected").arg(m_client.name),
                tr("You can close this page and try again in omareader."));
        finish(reason.isEmpty() ? tr("%1 wasn't connected.").arg(m_client.name) : reason);
        return;
    }
    respond(200, tr("%1 is connected").arg(m_client.name), tr("You can close this page and go back to omareader."));
    stopListening();
    exchange(code);
}

void LoopbackAuth::exchange(const QString &code) {
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    QNetworkRequest request{QUrl(m_client.tokenUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setTransferTimeout(60000);
    QList<QPair<QString, QString>> fields{{"code", code},
                                          {"grant_type", "authorization_code"},
                                          {"code_verifier", m_verifier},
                                          {"client_id", m_client.clientId},
                                          {"redirect_uri", m_client.redirectUri(m_port)}};
    if (!m_client.clientSecret.isEmpty())
        fields.append({"client_secret", m_client.clientSecret});
    QNetworkReply *reply = m_network->post(request, formBody(fields));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
        const QString refresh = object.value("refresh_token").toString();
        if (status != 200 || refresh.isEmpty()) {
            QString reason = object.value("error_description").toString();
            if (reason.isEmpty())
                reason = status == 200 ? tr("no lasting sign-in came back") : QString::number(status);
            finish(status == 0 ? reply->errorString() : tr("%1 refused the sign-in: %2").arg(m_client.name, reason));
            return;
        }
        m_timeout.stop();
        m_verifier.clear();
        m_state.clear();
        emit connected(refresh, object.value("access_token").toString(),
                       int(object.value("expires_in").toInteger(3600)));
    });
}

void LoopbackAuth::finish(const QString &error) {
    cancel();
    emit failed(error);
}
