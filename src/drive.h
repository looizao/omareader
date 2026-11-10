#pragma once

#include "oauth.h"

#include <QHash>
#include <QJsonObject>
#include <functional>

// Where Google lives; tests point these at a server of their own.
struct DriveHosts {
    QString api = QStringLiteral("https://www.googleapis.com");
    QString oauth = QStringLiteral("https://oauth2.googleapis.com");
    QString authorize = QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth");
};

// The desktop OAuth client that names omareader to Google, from
// GOOGLE_CLIENT_ID and GOOGLE_CLIENT_SECRET at build time. Google needs both;
// a build without them has no Drive.
QString googleClientId();
QString googleClientSecret();
// Google as an OAuth provider: its loopback flow for desktop apps, on any
// free port of 127.0.0.1, asking only for the files omareader makes.
OAuthClient driveClient(const QString &clientId, const QString &clientSecret, const DriveHosts &hosts = {});

// omareader's folder in the user's Google Drive, giving the engine the same
// calls Dropbox does (docs/sync.md, "Google Drive"). Drive knows files by id
// and lets two share a name, so files carry their Dropbox-style path in a
// property, and the remote keeps the ids its last listing found.
class DriveRemote : public OAuthRemote {
public:
    DriveRemote(const QString &clientId, const QString &clientSecret, const QString &refreshToken,
                const DriveHosts &hosts = {});

    Status list(QList<RemoteFile> *files) override;
    Status download(const QString &path, QByteArray *data, QString *rev) override;
    Status upload(const QString &path, const QByteArray &data, const QString &rev, QString *newRev) override;
    Status uploadFile(const QString &path, const QString &localFile, QString *newRev) override;
    Status remove(const QString &path) override;
    bool account(QString *name, QString *email) override;
    bool revoke() override;

    // Pieces of a bundle's upload; Drive wants multiples of 256 KiB.
    void setChunkSize(qint64 bytes) { m_chunk = bytes; }
    // How long a busy Drive is waited out; tests don't wait.
    void setSleep(std::function<void(int ms)> sleep) { m_sleep = std::move(sleep); }

    static constexpr const char *FolderMime = "application/vnd.google-apps.folder";

private:
    using Reply = HttpSession::Reply;
    struct Entry {
        QString id;
        QString rev;
        QString name;
    };

    // A call with the access token: a new token once on 401, and a few short
    // waits while Drive says it is busy.
    Reply call(const QByteArray &verb, const QString &url, const QByteArray &contentType = {},
               const QByteArray &body = {}, const HttpSession::Headers &headers = {});
    Status fail(const Reply &reply);
    // Every file omareader made that answers the query, all pages.
    Status query(const QString &q, const QString &fields, QList<QJsonObject> *files);
    Status folder(QString *id);
    // The file kept for a path right now, asking Drive rather than the last
    // listing; empty when there is none.
    Status find(const QString &path, Entry *entry);
    Status currentRev(const QString &id, QString *rev);
    Status trash(const QString &id);
    QString nameFor(const QString &path, const QString &localFile = {}) const;
    QString url(const QString &path, const QList<QPair<QString, QString>> &query = {}) const;
    static bool keepFirst(const QString &path, const QJsonObject &a, const QJsonObject &b);
    static QString revOf(const QJsonObject &file);

    DriveHosts m_hosts;
    QString m_folder;
    QHash<QString, Entry> m_entries; // path to file
    qint64 m_chunk = 8LL * 1024 * 1024;
    std::function<void(int ms)> m_sleep;
};

// Connecting to Google Drive (docs/sync.md).
class DriveAuth : public LoopbackAuth {
public:
    DriveAuth(const QString &clientId, const QString &clientSecret, QObject *parent = nullptr,
              const DriveHosts &hosts = {});
};
