#include "webimport.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QtConcurrent>

namespace {

bool hasReadableText(const QString &text) {
    for (const QChar c : text)
        if (c.isLetterOrNumber())
            return true;
    return false;
}

QByteArray mimeOf(const QByteArray &contentType) { return contentType.split(';').first().trimmed().toLower(); }

// Files that come without a content type but are plainly not a page.
bool looksBinary(const QByteArray &body) {
    if (body.startsWith("%PDF") || body.startsWith("PK\x03\x04"))
        return true;
    return body.left(1024).contains('\0');
}

} // namespace

WebImport::WebImport(QObject *parent) : QObject(parent) {
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(30000);
    connect(&m_timeout, &QTimer::timeout, this, [this] { stop(tr("%1 took too long to answer.").arg(m_host)); });
    connect(&m_watcher, &QFutureWatcher<Article>::finished, this, &WebImport::extracted);
}

WebImport::~WebImport() {
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
}

QUrl WebImport::parseAddress(const QString &input, QString *error) {
    QString ignored;
    if (!error)
        error = &ignored;
    const QString text = input.trimmed();
    static const QRegularExpression space(R"(\s)");
    static const QRegularExpression explicitScheme(R"(^([a-zA-Z][a-zA-Z0-9+.-]*)://)");
    static const QRegularExpression otherScheme(R"(^(javascript|data|file|mailto|about|blob|ftp|chrome|view-source):)",
                                                QRegularExpression::CaseInsensitiveOption);
    if (text.isEmpty() || text.contains(space)) {
        *error = tr("Enter a web address, such as https://example.com/article.");
        return {};
    }
    const auto scheme = explicitScheme.match(text);
    if ((scheme.hasMatch() && scheme.captured(1).toLower() != "http" && scheme.captured(1).toLower() != "https")
        || otherScheme.match(text).hasMatch()) {
        *error = tr("Only web pages (http:// or https://) can be added.");
        return {};
    }
    const QUrl url(scheme.hasMatch() ? text : "https://" + text, QUrl::StrictMode);
    const QString host = url.host();
    // A host has a dot in it, unless it is this computer or an IPv6 address.
    if (!url.isValid() || host.isEmpty() || (!host.contains('.') && !host.contains(':') && host != "localhost")) {
        *error = tr("Enter a web address, such as https://example.com/article.");
        return {};
    }
    return url;
}

QString WebImport::clipboardAddress() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QString text = clipboard ? clipboard->text().trimmed() : QString();
    if (!text.startsWith("http://", Qt::CaseInsensitive) && !text.startsWith("https://", Qt::CaseInsensitive))
        return {};
    return parseAddress(text).isValid() ? text : QString();
}

// Where a page sends the browser on at once with <meta http-equiv="refresh">,
// as static sites do for pages that moved; empty when it doesn't.
QUrl WebImport::refreshTarget(const QByteArray &body, const QUrl &base) {
    static const QRegularExpression meta(R"(<meta\b[^>]*>)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression refresh(R"(http-equiv\s*=\s*["']?refresh)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression content(R"re(content\s*=\s*(["'])\s*(\d+)\s*[;,]\s*url\s*=\s*['"]?([^"'>]+?)['"]?\s*\1)re",
                                            QRegularExpression::CaseInsensitiveOption);
    const QString head = QString::fromUtf8(body.left(16384));
    for (auto it = meta.globalMatch(head); it.hasNext();) {
        const QString tag = it.next().captured(0);
        if (!refresh.match(tag).hasMatch())
            continue;
        const auto match = content.match(tag);
        // A page that waits before moving on has something to show first.
        if (!match.hasMatch() || match.captured(2).toInt() > 5)
            return {};
        const QUrl target = base.resolved(QUrl(match.captured(3).trimmed()));
        const QString scheme = target.scheme().toLower();
        return (scheme == "http" || scheme == "https") && target != base ? target : QUrl();
    }
    return {};
}

void WebImport::fetch(const QString &address) {
    cancel();
    clear();
    QString reason;
    const QUrl url = parseAddress(address, &reason);
    if (!url.isValid()) {
        fail(reason);
        return;
    }
    m_refreshes = 0;
    start(url);
}

void WebImport::start(const QUrl &url) {
    m_url = url;
    m_host = url.host();
    m_stopReason = QString();

    QNetworkRequest request(url);
    // Redirects are checked one by one, so a page can't send the fetch to
    // anything but another web page, or from https down to http.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(10);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    const QString version = QCoreApplication::applicationVersion();
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QString("Mozilla/5.0 (X11; Linux x86_64) omareader/%1").arg(version.isEmpty() ? "0" : version));
    request.setRawHeader("Accept", "text/html,application/xhtml+xml;q=0.9,text/markdown;q=0.8,text/plain;q=0.8");
    // Each fetch starts without cookies, so one page can't follow the user
    // to the next.
    m_network.setCookieJar(new QNetworkCookieJar(&m_network));

    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::redirected, this, [this, reply](const QUrl &target) {
        const QString scheme = target.scheme().toLower();
        if (scheme != "http" && scheme != "https") {
            stop(tr("%1 sent the request on to %2, which isn't a web page.").arg(m_host, target.toDisplayString()));
            return;
        }
        if (reply->url().scheme().toLower() == "https" && scheme == "http") {
            stop(tr("%1 sent the request on to an address that isn't secure (http), so it wasn't followed.")
                     .arg(m_host));
            return;
        }
        emit reply->redirectAllowed();
    });
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status < 200 || status >= 300)
            return;
        const QByteArray type = reply->rawHeader("Content-Type");
        if (!ArticleExtractor::isSupported(type))
            stop(unsupported(type));
        else if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > m_maxBytes)
            stop(tr("This page is larger than %1 MB, too large to add.").arg(m_maxBytes / (1024 * 1024)));
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64) {
        m_timeout.start();
        if (received > m_maxBytes)
            stop(tr("This page is larger than %1 MB, too large to add.").arg(m_maxBytes / (1024 * 1024)));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finished(reply); });
    m_timeout.start();
    emit busyChanged();
}

void WebImport::stop(const QString &reason) {
    if (!m_reply || !m_stopReason.isNull())
        return;
    m_stopReason = reason;
    m_reply->abort();
}

void WebImport::cancel() {
    const bool wasBusy = busy();
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_timeout.stop();
    // An article still being found is left to finish and then ignored.
    ++m_generation;
    m_extracting = false;
    if (wasBusy)
        emit busyChanged();
}

void WebImport::clear() {
    const bool had = hasArticle();
    m_article = Article{};
    m_source.clear();
    if (had)
        emit articleChanged();
    if (!m_error.isEmpty()) {
        m_error.clear();
        emit errorChanged();
    }
}

void WebImport::fail(const QString &reason) {
    m_error = reason;
    emit errorChanged();
    emit failed(reason);
}

void WebImport::finished(QNetworkReply *reply) {
    reply->deleteLater();
    if (reply != m_reply)
        return;
    m_reply = nullptr;
    m_timeout.stop();
    const QString stopped = m_stopReason;
    m_stopReason = QString();

    QString reason;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray type = reply->rawHeader("Content-Type");
    QByteArray body;
    if (!stopped.isNull()) {
        reason = stopped;
    } else if (status >= 400) {
        reason = httpError(status);
    } else if (reply->error() != QNetworkReply::NoError) {
        reason = networkError(reply);
    } else {
        body = reply->readAll();
        if (body.size() > m_maxBytes)
            reason = tr("This page is larger than %1 MB, too large to add.").arg(m_maxBytes / (1024 * 1024));
        else if (!ArticleExtractor::isSupported(type) || (type.isEmpty() && looksBinary(body)))
            reason = unsupported(type);
    }
    if (!reason.isNull()) {
        emit busyChanged();
        if (!reason.isEmpty())
            fail(reason);
        return;
    }

    if (ArticleExtractor::isHtml(type) && m_refreshes < 3) {
        const QUrl target = refreshTarget(body, reply->url());
        if (target.isValid()
            && !(reply->url().scheme().toLower() == "https" && target.scheme().toLower() == "http")) {
            ++m_refreshes;
            start(target);
            return;
        }
    }

    // The address it ended up at, without anything that was only for the
    // fetch: a password in it, or the place on the page.
    m_pendingSource = reply->url().adjusted(QUrl::RemoveUserInfo | QUrl::RemoveFragment).toString();
    const QUrl url = reply->url();
    m_extracting = true;
    m_extractGeneration = m_generation;
    m_watcher.setFuture(QtConcurrent::run([body, url, type] { return ArticleExtractor::extract(body, url, type); }));
}

void WebImport::extracted() {
    if (!m_extracting || m_extractGeneration != m_generation)
        return;
    m_extracting = false;
    const Article article = m_watcher.result();
    if (!hasReadableText(article.text)) {
        emit busyChanged();
        fail(tr("No readable text was found on this page. Pages that need a login, or that build their text with scripts, can't be added."));
        return;
    }
    m_article = article;
    m_source = m_pendingSource;
    emit articleChanged();
    emit busyChanged();
    emit fetched();
}

QString WebImport::networkError(QNetworkReply *reply) const {
    switch (reply->error()) {
    case QNetworkReply::HostNotFoundError:
        return tr("%1 couldn't be found. Check the address and your connection.").arg(m_host);
    case QNetworkReply::TimeoutError:
        return tr("%1 took too long to answer.").arg(m_host);
    case QNetworkReply::SslHandshakeFailedError:
        return tr("The secure connection to %1 couldn't be verified, so the page wasn't fetched.").arg(m_host);
    case QNetworkReply::TooManyRedirectsError:
        return tr("%1 sent the request on too many times.").arg(m_host);
    case QNetworkReply::ProtocolUnknownError: {
        // Only a redirect can lead somewhere other than http or https, since
        // the address typed was checked.
        const QByteArray location = reply->rawHeader("Location");
        if (location.isEmpty())
            return tr("The page couldn't be fetched: %1").arg(reply->errorString());
        const QUrl target = reply->url().resolved(QUrl::fromEncoded(location));
        return tr("%1 sent the request on to %2, which isn't a web page.").arg(m_host, target.toDisplayString());
    }
    case QNetworkReply::InsecureRedirectError:
        return tr("%1 sent the request on to an address that isn't secure (http), so it wasn't followed.").arg(m_host);
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
        return tr("%1 couldn't be reached. Check your connection and try again.").arg(m_host);
    default:
        return tr("The page couldn't be fetched: %1").arg(reply->errorString());
    }
}

QString WebImport::httpError(int status) const {
    if (status == 401 || status == 402 || status == 403 || status == 407)
        return tr("%1 wouldn't send the page (HTTP %2). Pages behind a login or a paywall can't be added.")
            .arg(m_host)
            .arg(status);
    if (status == 404 || status == 410)
        return tr("There is no page at this address (HTTP %1).").arg(status);
    if (status == 429)
        return tr("%1 is getting too many requests. Try again later (HTTP %2).").arg(m_host).arg(status);
    if (status >= 500)
        return tr("%1 had a problem sending the page (HTTP %2). Try again later.").arg(m_host).arg(status);
    return tr("%1 answered with an error (HTTP %2).").arg(m_host).arg(status);
}

QString WebImport::unsupported(const QByteArray &contentType) const {
    const QByteArray mime = mimeOf(contentType);
    if (mime == "application/pdf")
        return tr("This address leads to a %1 file, not a web page. Download it and open it as a book instead.")
            .arg("PDF");
    if (mime == "application/epub+zip")
        return tr("This address leads to a %1 file, not a web page. Download it and open it as a book instead.")
            .arg("EPUB");
    if (mime.isEmpty())
        return tr("This address leads to a file, not a web page with text to read.");
    return tr("This address leads to %1 content, not a web page with text to read.").arg(QString::fromLatin1(mime));
}
