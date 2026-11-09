#pragma once

#include "syncengine.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>

class DropboxAuth;
class Exporter;
class Library;
struct LibraryEntry;
class Narrator;
class Reader;
class TokenStore;

// Sync through the user's Dropbox (docs/sync.md), exposed to QML as `sync`.
// It turns the library and settings into registers, runs the engine on a
// worker thread, and applies what came back. Off until the user connects;
// then it syncs at start, when the window comes back, a few seconds after a
// change, and every 15 minutes. Its own records live in sync.json beside the
// library; the Dropbox sign-in lives in the keyring.
class SyncService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString account READ account NOTIFY stateChanged)
    Q_PROPERTY(QDateTime lastSynced READ lastSynced NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(int pendingUploads READ pendingUploads NOTIFY stateChanged)
    Q_PROPERTY(bool sendExports READ sendExports WRITE setSendExports NOTIFY stateChanged)

public:
    using RemoteFactory = std::function<std::unique_ptr<SyncRemote>(const QString &refreshToken)>;

    SyncService(Library *library, TokenStore *tokens, const QString &stateFile = {}, QObject *parent = nullptr);
    ~SyncService() override;

    // The pieces of the window whose settings sync; any may be missing, as
    // on the command line, where the saved settings are used directly.
    void setNarrator(Narrator *narrator);
    void setReader(Reader *reader);
    void setExporter(Exporter *exporter);
    // Tests talk to a folder in memory and wait for each sync.
    void setRemoteFactory(RemoteFactory factory) { m_factory = std::move(factory); }
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }

    bool available() const;
    bool connected() const { return m_connected; }
    bool connecting() const { return m_auth != nullptr; }
    bool busy() const { return m_busy; }
    QString account() const { return m_account; }
    QDateTime lastSynced() const { return m_lastSynced; }
    QString error() const { return m_error; }
    int pendingUploads() const { return int(m_uploads.size()); }
    bool sendExports() const { return m_sendExports; }
    void setSendExports(bool send);
    QString deviceId() const { return m_device; }
    SyncOutput lastOutput() const { return m_lastOutput; }

    // Opens Dropbox in the browser to connect; false if no port was free.
    Q_INVOKABLE bool connectDropbox();
    Q_INVOKABLE void cancelConnect();
    Q_INVOKABLE void disconnectDropbox();
    Q_INVOKABLE void syncNow();
    // The window came back to the front: sync if it's been a while.
    Q_INVOKABLE void windowActivated();
    // Sends a phone bundle with the next sync.
    void queueUpload(const QString &file, const QString &id, const QString &title, const QString &author);
    // Takes a sign-in made elsewhere (the command line, tests).
    bool connectWith(const QString &refreshToken, const QString &accessToken = {}, int expiresIn = 0);
    QString authorizeUrl() const { return m_authorizeUrl; }

signals:
    void stateChanged();
    void synced(bool ok);
    // The browser page to open, for the command line to print.
    void authorizeUrlReady(const QString &url);

private:
    struct Removed {
        QString title;
        QString author;
        Register present;
    };
    struct Result {
        SyncOutput output;
        QString accessToken;
        qint64 expiresAt = 0;
        bool revoked = false;
        QString accountName;
    };

    void load();
    void save() const;
    SyncInput input();
    Register localPosition(const LibraryEntry &entry, const SyncCacheEntry &cached) const;
    Register localPresence(const LibraryEntry &entry, const SyncCacheEntry &cached) const;
    void apply(const Result &result);
    void finish(const Result &result);
    void scheduleSoon();
    void stampSetting(const QString &key, const QJsonValue &value);
    void stampSettings();
    double speed() const;
    void setSpeed(double speed);
    int fontSize() const;
    void setFontSize(int size);
    void setError(const QString &error);
    std::unique_ptr<SyncRemote> makeRemote(const QString &refreshToken) const;

    Library *m_library;
    TokenStore *m_tokens;
    Narrator *m_narrator = nullptr;
    Reader *m_reader = nullptr;
    QString m_file;
    QString m_device;
    bool m_connected = false;
    bool m_busy = false;
    bool m_again = false;
    bool m_applying = false;
    bool m_synchronous = false;
    bool m_sendExports = true;
    QString m_account;
    QString m_error;
    QDateTime m_lastSynced;
    QString m_accessToken;
    qint64 m_accessExpiry = 0;
    QHash<QString, SyncCacheEntry> m_cache;
    QString m_settingsRev;
    SyncSettings m_settingsCache;
    SyncSettings m_settings;
    QHash<QString, Removed> m_removed;
    QList<PendingUpload> m_uploads;
    SyncOutput m_lastOutput;
    RemoteFactory m_factory;
    DropboxAuth *m_auth = nullptr;
    QString m_authorizeUrl;
    QTimer m_soon;
    QTimer m_periodic;
    QFutureWatcher<Result> m_watcher;
};
