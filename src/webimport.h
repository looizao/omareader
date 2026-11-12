#pragma once

#include "article.h"

#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

class QNetworkReply;

// Adds a web page to the library from its address. The page is fetched
// once, only when the user asks, and its article found for the user to
// review; the library then keeps that text and never fetches it again.
// Nothing the page links to (images, styles, scripts) is loaded and none of
// its scripts run. Exposed to QML as `webImport`.
class WebImport : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // The site being fetched, while busy.
    Q_PROPERTY(QString fetching READ fetching NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool hasArticle READ hasArticle NOTIFY articleChanged)
    Q_PROPERTY(QString title READ title NOTIFY articleChanged)
    Q_PROPERTY(QString author READ author NOTIFY articleChanged)
    Q_PROPERTY(QString site READ site NOTIFY articleChanged)
    Q_PROPERTY(QString text READ text NOTIFY articleChanged)
    Q_PROPERTY(QString source READ source NOTIFY articleChanged)

public:
    explicit WebImport(QObject *parent = nullptr);
    ~WebImport() override;

    // The address typed or pasted, as an http or https URL; one without a
    // scheme is taken as https. Invalid, with the reason, when it isn't a
    // web address.
    static QUrl parseAddress(const QString &input, QString *error = nullptr);
    // Where a page sends the browser on at once (meta refresh), if anywhere.
    static QUrl refreshTarget(const QByteArray &body, const QUrl &base);
    // The clipboard's text when it is a web address and nothing else, so
    // pasting a link in the library offers to add its page.
    Q_INVOKABLE QString clipboardAddress() const;

    bool busy() const { return m_reply || m_extracting; }
    QString fetching() const { return busy() ? m_host : QString(); }
    QString error() const { return m_error; }
    bool hasArticle() const { return !m_source.isEmpty(); }
    QString title() const { return m_article.title; }
    QString author() const { return m_article.author; }
    QString site() const { return m_article.site; }
    QString text() const { return m_article.text; }
    QString source() const { return m_source; }

    // How long the site may go without sending anything, and how much of
    // it is read at most.
    void setTimeout(int milliseconds) { m_timeout.setInterval(milliseconds); }
    void setMaxBytes(qint64 bytes) { m_maxBytes = bytes; }

    // Fetches the page and finds its article; answers with fetched() or
    // failed(). A fetch still running is dropped first.
    Q_INVOKABLE void fetch(const QString &address);
    // Stops a fetch without an error, keeping nothing of it.
    Q_INVOKABLE void cancel();
    // Forgets the article and any error, once the dialog is gone.
    Q_INVOKABLE void clear();

signals:
    void busyChanged();
    void errorChanged();
    void articleChanged();
    void fetched();
    void failed(const QString &reason);

private:
    void start(const QUrl &url);
    void finished(QNetworkReply *reply);
    void extracted();
    void stop(const QString &reason);
    void fail(const QString &reason);
    QString networkError(QNetworkReply *reply) const;
    QString httpError(int status) const;
    QString unsupported(const QByteArray &contentType) const;

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QFutureWatcher<Article> m_watcher;
    bool m_extracting = false;
    // Moves on with each cancel, so an article found for a fetch that was
    // cancelled meanwhile is ignored.
    int m_generation = 0;
    int m_extractGeneration = 0;
    // Why the running fetch was stopped: empty for a cancel, null when it
    // wasn't.
    QString m_stopReason;
    QTimer m_timeout;
    qint64 m_maxBytes = 15 * 1024 * 1024;
    QString m_host;
    int m_refreshes = 0;
    QUrl m_url;
    QString m_error;
    Article m_article;
    QString m_source;
    QString m_pendingSource;
};
