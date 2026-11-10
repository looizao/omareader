#include "sync.h"

#include "drive.h"
#include "dropbox.h"
#include "exporter.h"
#include "library.h"
#include "narrator.h"
#include "reader.h"
#include "tokenstore.h"
#include "voicesettings.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QtConcurrent>
#include <cmath>

namespace {

constexpr int SoonMs = 8000;
constexpr int PeriodMs = 15 * 60 * 1000;
constexpr qint64 StaleMs = 60 * 1000;

QJsonObject toObject(const BookState &state) { return QJsonDocument::fromJson(state.toJson()).object(); }

BookState fromObject(const QJsonObject &object) {
    BookState state;
    BookState::fromJson(QJsonDocument(object).toJson(QJsonDocument::Compact), &state);
    return state;
}

QJsonObject toObject(const SyncSettings &settings) { return QJsonDocument::fromJson(settings.toJson()).object(); }

SyncSettings settingsFrom(const QJsonObject &object) {
    SyncSettings settings;
    SyncSettings::fromJson(QJsonDocument(object).toJson(QJsonDocument::Compact), &settings);
    return settings;
}

double progressOf(const LibraryEntry &e) {
    return e.sentenceCount > 1 ? qBound(0.0, double(e.sentence) / (e.sentenceCount - 1), 1.0) : 0;
}

bool hasPosition(const LibraryEntry &e) { return e.positionAt > 0 || e.chapter > 0 || e.sentence > 0; }

}

SyncService::SyncService(Library *library, TokenStore *tokens, const QString &stateFile, QObject *parent)
    : QObject(parent), m_library(library), m_tokens(tokens),
      m_file(stateFile.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/sync.json"
                                 : stateFile) {
    load();
    m_soon.setSingleShot(true);
    m_soon.setInterval(SoonMs);
    connect(&m_soon, &QTimer::timeout, this, &SyncService::syncNow);
    m_periodic.setInterval(PeriodMs);
    connect(&m_periodic, &QTimer::timeout, this, &SyncService::syncNow);
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this] { finish(m_watcher.result()); });

    if (m_library) {
        connect(m_library, &Library::changed, this, [this] {
            if (!m_applying)
                scheduleSoon();
        });
        connect(m_library, &Library::removed, this,
                [this](const QString &, const QString &title, const QString &author) {
                    if (!available())
                        return;
                    const QString id = SyncFormat::bookId(title, author);
                    m_removed.insert(id, {title, author, Register{false, QDateTime::currentMSecsSinceEpoch(), m_device}});
                    save();
                });
    }
    if (m_connected) {
        m_periodic.start();
        // Shortly after start, once the window is up.
        QTimer::singleShot(1500, this, &SyncService::syncNow);
    }
}

SyncService::~SyncService() {
    if (m_watcher.isRunning())
        m_watcher.waitForFinished();
}

bool SyncService::dropboxAvailable() const { return !dropboxAppKey().isEmpty() || m_factory != nullptr; }

bool SyncService::driveAvailable() const {
    return (!googleClientId().isEmpty() && !googleClientSecret().isEmpty()) || m_factory != nullptr;
}

bool SyncService::available() const { return dropboxAvailable() || driveAvailable(); }

QString SyncService::nameOf(const QString &provider) {
    if (provider == Drive)
        return QStringLiteral("Google Drive");
    if (provider == Dropbox)
        return QStringLiteral("Dropbox");
    return {};
}

void SyncService::setNarrator(Narrator *narrator) {
    m_narrator = narrator;
    if (m_narrator)
        connect(m_narrator, &Narrator::speedChanged, this, [this] {
            if (m_applying)
                return;
            stampSettings();
            scheduleSoon();
        });
    stampSettings();
}

void SyncService::setReader(Reader *reader) {
    m_reader = reader;
    if (m_reader)
        connect(m_reader, &Reader::appearanceChanged, this, [this] {
            if (m_applying)
                return;
            const Register before = m_settings.value(SyncSettings::DesktopTextSize);
            stampSettings();
            if (!m_settings.value(SyncSettings::DesktopTextSize).sameChange(before))
                scheduleSoon();
        });
    stampSettings();
}

void SyncService::setExporter(Exporter *exporter) {
    if (exporter)
        connect(exporter, &Exporter::phoneBundleReady, this,
                [this](const QString &path, const QString &id, const QString &title, const QString &author) {
                    if (m_connected && m_sendExports)
                        queueUpload(path, id, title, author);
                });
}

void SyncService::setSendExports(bool send) {
    if (send == m_sendExports)
        return;
    m_sendExports = send;
    save();
    emit stateChanged();
}

// Saved state.

void SyncService::load() {
    QFile file(m_file);
    const QJsonObject root =
        file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
    m_device = root.value("device").toString();
    if (!SyncFormat::isValidId(m_device))
        m_device = SyncFormat::newDeviceId();
    m_connected = root.value("connected").toBool();
    // Dropbox came first, before there was a choice to record.
    m_provider = root.value("provider").toString(m_connected ? Dropbox : "");
    if (m_provider != Dropbox && m_provider != Drive)
        m_connected = false;
    m_account = root.value("account").toString();
    m_sendExports = root.value("sendExports").toBool(true);
    const qint64 last = root.value("lastSynced").toInteger();
    m_lastSynced = last > 0 ? QDateTime::fromMSecsSinceEpoch(last) : QDateTime();
    const QJsonObject cache = root.value("cache").toObject();
    for (auto it = cache.constBegin(); it != cache.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        m_cache.insert(it.key(), {entry.value("rev").toString(), fromObject(entry.value("state").toObject())});
    }
    m_settingsRev = root.value("settingsRev").toString();
    m_settingsCache = settingsFrom(root.value("settingsCache").toObject());
    m_settings = settingsFrom(root.value("settings").toObject());
    const QJsonObject removed = root.value("removed").toObject();
    for (auto it = removed.constBegin(); it != removed.constEnd(); ++it) {
        const QJsonObject o = it.value().toObject();
        m_removed.insert(it.key(), {o.value("title").toString(), o.value("author").toString(),
                                    Register::fromJson(o.value("present"))});
    }
    for (const QJsonValue &value : root.value("uploads").toArray()) {
        const QJsonObject o = value.toObject();
        m_uploads.append({o.value("id").toString(), o.value("title").toString(), o.value("author").toString(),
                          o.value("file").toString()});
    }
}

void SyncService::save() const {
    QJsonObject cache;
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it)
        cache.insert(it.key(), QJsonObject{{"rev", it.value().rev}, {"state", toObject(it.value().state)}});
    QJsonObject removed;
    for (auto it = m_removed.cbegin(); it != m_removed.cend(); ++it)
        removed.insert(it.key(), QJsonObject{{"title", it.value().title},
                                             {"author", it.value().author},
                                             {"present", it.value().present.toJson()}});
    QJsonArray uploads;
    for (const PendingUpload &u : m_uploads)
        uploads.append(QJsonObject{{"id", u.id}, {"title", u.title}, {"author", u.author}, {"file", u.file}});
    const QJsonObject root{{"version", 1},
                           {"device", m_device},
                           {"connected", m_connected},
                           {"provider", m_provider},
                           {"account", m_account},
                           {"sendExports", m_sendExports},
                           {"lastSynced", m_lastSynced.isValid() ? m_lastSynced.toMSecsSinceEpoch() : 0},
                           {"cache", cache},
                           {"settingsRev", m_settingsRev},
                           {"settingsCache", toObject(m_settingsCache)},
                           {"settings", toObject(m_settings)},
                           {"removed", removed},
                           {"uploads", uploads}};
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile file(m_file);
    if (!file.open(QIODevice::WriteOnly))
        return;
    // Book titles and the account are nobody else's business.
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}

// Settings.

double SyncService::speed() const {
    return m_narrator ? m_narrator->speed() : VoiceSettings::loadSelected().speed;
}

void SyncService::setSpeed(double speed) {
    if (m_narrator) {
        m_narrator->setSpeed(speed);
        return;
    }
    VoiceSettings voice = VoiceSettings::loadSelected();
    voice.speed = VoiceSettings::clampSpeed(speed);
    voice.save();
}

int SyncService::fontSize() const {
    return m_reader ? m_reader->fontSize() : qBound(12, QSettings().value("reader/fontSize", 20).toInt(), 48);
}

void SyncService::setFontSize(int size) {
    if (m_reader)
        m_reader->setFontSize(size);
    else
        QSettings().setValue("reader/fontSize", qBound(12, size, 48));
}

void SyncService::stampSetting(const QString &key, const QJsonValue &value) {
    const Register current = m_settings.value(key);
    const bool same = current.isSet() && (value.isDouble() && current.value.isDouble()
                                              ? qFuzzyCompare(1.0 + value.toDouble(), 1.0 + current.value.toDouble())
                                              : value == current.value);
    if (same)
        return;
    // A setting never stamped before counts as set at the beginning of
    // time, so any change another device made wins over it.
    const qint64 at = current.isSet() ? QDateTime::currentMSecsSinceEpoch() : 0;
    m_settings.values.insert(key, Register{value, at, m_device});
    save();
}

void SyncService::stampSettings() {
    if (!available())
        return;
    stampSetting(SyncSettings::Speed, std::round(speed() * 100) / 100);
    stampSetting(SyncSettings::DesktopTextSize, fontSize());
}

// Registers from the library.

Register SyncService::localPresence(const LibraryEntry &entry, const SyncCacheEntry &cached) const {
    const Register &known = cached.state.present;
    if (known.isSet() && known.value.toBool() && known.at == entry.addedAt)
        return known;
    return Register{true, entry.addedAt, m_device};
}

Register SyncService::localPosition(const LibraryEntry &entry, const SyncCacheEntry &cached) const {
    if (!hasPosition(entry))
        return {};
    const SyncPosition here{entry.chapter, entry.paragraph, qMax(0, entry.offset), progressOf(entry)};
    // The place last taken from the provider, unmoved since: hand back exactly
    // what came, so it never travels back as a change of this device's.
    const Register &known = cached.state.position;
    if (known.isSet() && known.at == entry.positionAt) {
        const SyncPosition there = cached.state.syncPosition();
        if (there.chapter == here.chapter && there.paragraph == here.paragraph && there.offset == here.offset)
            return known;
    }
    return Register{here.toJson(), entry.positionAt, m_device};
}

SyncInput SyncService::input() {
    stampSettings();
    SyncInput in;
    in.device = m_device;
    in.now = QDateTime::currentMSecsSinceEpoch();
    in.cache = m_cache;
    in.settingsRev = m_settingsRev;
    in.settingsCache = m_settingsCache;
    in.settings = m_settings;
    in.uploads = m_uploads;
    if (m_library)
        for (const LibraryEntry &e : m_library->entries()) {
            const QString id = SyncFormat::bookId(e.title, e.author);
            const SyncCacheEntry cached = m_cache.value(id);
            BookState &state = in.local[id];
            state.id = id;
            state.title = e.title;
            state.author = e.author;
            state.present = Register::newer(state.present, localPresence(e, cached));
            state.position = Register::newer(state.position, localPosition(e, cached));
        }
    for (auto it = m_removed.cbegin(); it != m_removed.cend(); ++it) {
        BookState &state = in.local[it.key()];
        state.id = it.key();
        if (state.title.isEmpty())
            state.title = it.value().title;
        if (state.author.isEmpty())
            state.author = it.value().author;
        state.present = Register::newer(state.present, it.value().present);
    }
    return in;
}

// Running.

namespace {

std::unique_ptr<OAuthRemote> providerRemote(const QString &provider, const QString &refreshToken) {
    if (provider == SyncService::Drive)
        return std::make_unique<DriveRemote>(googleClientId(), googleClientSecret(), refreshToken);
    return std::make_unique<DropboxRemote>(dropboxAppKey(), refreshToken);
}

}

std::unique_ptr<SyncRemote> SyncService::makeRemote(const QString &provider, const QString &refreshToken) const {
    if (m_factory)
        return m_factory(refreshToken);
    return providerRemote(provider, refreshToken);
}

void SyncService::syncNow() {
    if (!m_connected || !available())
        return;
    if (m_busy) {
        m_again = true;
        return;
    }
    m_soon.stop();
    QString keyringError;
    const QString refreshToken = m_tokens ? m_tokens->read(m_provider, &keyringError) : QString();
    if (refreshToken.isEmpty()) {
        setError(keyringError.isEmpty()
                     ? tr("The %1 sign-in is missing from the keyring. Connect again.").arg(providerName())
                     : tr("The keyring couldn't be read: %1").arg(keyringError));
        return;
    }
    m_busy = true;
    emit stateChanged();

    const SyncInput in = input();
    const QString accessToken = m_accessToken;
    const qint64 expiry = m_accessExpiry;
    const bool needAccount = m_account.isEmpty();
    std::shared_ptr<SyncRemote> remote = makeRemote(m_provider, refreshToken);
    auto work = [in, remote, accessToken, expiry, needAccount]() mutable {
        Result result;
        auto *signedIn = dynamic_cast<OAuthRemote *>(remote.get());
        if (signedIn && !accessToken.isEmpty())
            signedIn->setAccessToken(accessToken, expiry);
        result.output = SyncEngine::run(in, *remote);
        if (signedIn) {
            if (needAccount && result.output.ok) {
                QString name, email;
                if (signedIn->account(&name, &email))
                    result.accountName = email.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(name, email);
            }
            result.accessToken = signedIn->accessToken();
            result.expiresAt = signedIn->accessTokenExpiry();
            result.revoked = signedIn->tokenRevoked();
        }
        // The network access belongs to this thread; let it go here.
        remote.reset();
        return result;
    };
    if (m_synchronous) {
        finish(work());
        return;
    }
    m_watcher.setFuture(QtConcurrent::run(work));
}

void SyncService::finish(const Result &result) {
    m_busy = false;
    if (result.revoked) {
        // The provider no longer knows this sign-in; nothing more can sync
        // until the user connects again.
        if (m_tokens)
            m_tokens->clear(m_provider);
        m_connected = false;
        m_periodic.stop();
        m_accessToken.clear();
        setError(tr("%1 disconnected omareader. Connect again to keep syncing.").arg(providerName()));
        save();
        emit synced(false);
        return;
    }
    m_accessToken = result.accessToken;
    m_accessExpiry = result.expiresAt;
    if (!result.accountName.isEmpty())
        m_account = result.accountName;
    apply(result);
    m_lastOutput = result.output;
    if (result.output.ok) {
        m_lastSynced = QDateTime::currentDateTime();
        m_error.clear();
    } else {
        m_error = result.output.error;
    }
    save();
    emit stateChanged();
    emit synced(result.output.ok);
    if (m_again) {
        m_again = false;
        scheduleSoon();
    }
}

void SyncService::apply(const Result &result) {
    const SyncOutput &out = result.output;
    m_applying = true;
    m_cache = out.cache;
    m_settingsRev = out.settingsRev;
    m_settingsCache = out.settingsCache;
    for (const QString &id : out.uploadsDone)
        m_uploads.removeIf([&id](const PendingUpload &u) { return u.id == id; });

    if (m_library) {
        // The library as it is now, which may have moved on while the sync
        // was busy; a change made meanwhile is newer and stays.
        QHash<QString, QList<LibraryEntry>> byId;
        for (const LibraryEntry &e : m_library->entries())
            byId[SyncFormat::bookId(e.title, e.author)].append(e);
        for (auto it = out.merged.cbegin(); it != out.merged.cend(); ++it) {
            const BookState &merged = it.value();
            const SyncCacheEntry cached = m_cache.value(it.key());
            const QList<LibraryEntry> entries = byId.value(it.key());
            if (merged.isRemoved()) {
                for (const LibraryEntry &e : entries)
                    if (merged.present.beats(localPresence(e, cached)))
                        m_library->forget(e.path);
                const Register before = m_removed.value(it.key()).present;
                if (merged.present.beats(before) || !before.isSet())
                    m_removed.insert(it.key(), {merged.title, merged.author, merged.present});
            } else if (merged.isPresent()) {
                m_removed.remove(it.key());
            }
            if (!merged.hasPosition())
                continue;
            const SyncPosition p = merged.syncPosition();
            for (const LibraryEntry &e : entries)
                if (merged.position.beats(localPosition(e, cached)) && !merged.isRemoved())
                    m_library->applySyncedPosition(e.path, p.chapter, p.paragraph, p.offset, p.progress,
                                                   merged.position.at);
        }
    }

    const Register speedNow = out.settings.value(SyncSettings::Speed);
    if (speedNow.beats(m_settings.value(SyncSettings::Speed)) && speedNow.value.isDouble()) {
        m_settings.values.insert(SyncSettings::Speed, speedNow);
        setSpeed(VoiceSettings::clampSpeed(speedNow.value.toDouble()));
    }
    const Register sizeNow = out.settings.value(SyncSettings::DesktopTextSize);
    if (sizeNow.beats(m_settings.value(SyncSettings::DesktopTextSize)) && sizeNow.value.isDouble()) {
        m_settings.values.insert(SyncSettings::DesktopTextSize, sizeNow);
        setFontSize(sizeNow.value.toInt());
    }
    // Keys for other devices, kept so they're written back unchanged.
    for (auto it = out.settings.values.cbegin(); it != out.settings.values.cend(); ++it)
        if (it.key() != SyncSettings::Speed && it.key() != SyncSettings::DesktopTextSize)
            m_settings.values.insert(it.key(), it.value());
    m_applying = false;
}

void SyncService::scheduleSoon() {
    if (m_connected)
        m_soon.start();
}

void SyncService::windowActivated() {
    if (m_connected && (!m_lastSynced.isValid() || m_lastSynced.msecsTo(QDateTime::currentDateTime()) > StaleMs))
        syncNow();
}

void SyncService::queueUpload(const QString &file, const QString &id, const QString &title, const QString &author) {
    m_uploads.removeIf([&id](const PendingUpload &u) { return u.id == id; });
    m_uploads.append({id, title, author, QFileInfo(file).absoluteFilePath()});
    save();
    emit stateChanged();
    syncNow();
}

void SyncService::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit stateChanged();
}

// Connecting.

bool SyncService::connectProvider(const QString &provider) {
    if (provider == Dropbox ? !dropboxAvailable() : provider == Drive ? !driveAvailable() : true)
        return false;
    cancelConnect();
    if (provider == Drive)
        m_auth = new DriveAuth(googleClientId(), googleClientSecret(), this);
    else
        m_auth = new DropboxAuth(dropboxAppKey(), this);
    m_connectingTo = provider;
    connect(m_auth, &LoopbackAuth::connected, this,
            [this, provider](const QString &refresh, const QString &access, int expiresIn) {
                m_auth->deleteLater();
                m_auth = nullptr;
                connectWith(provider, refresh, access, expiresIn);
            });
    connect(m_auth, &LoopbackAuth::failed, this, [this](const QString &error) {
        m_auth->deleteLater();
        m_auth = nullptr;
        m_error = error;
        emit stateChanged();
    });
    m_authorizeUrl = m_auth->start();
    if (m_authorizeUrl.isEmpty()) {
        delete m_auth;
        m_auth = nullptr;
        setError(provider == Dropbox
                     ? tr("omareader couldn't listen for Dropbox's answer: ports 53682 to 53684 are all in use.")
                     : tr("omareader couldn't listen for %1's answer.").arg(nameOf(provider)));
        return false;
    }
    m_error.clear();
    emit stateChanged();
    emit authorizeUrlReady(m_authorizeUrl);
    QDesktopServices::openUrl(QUrl(m_authorizeUrl));
    return true;
}

void SyncService::cancelConnect() {
    if (!m_auth)
        return;
    m_auth->cancel();
    m_auth->deleteLater();
    m_auth = nullptr;
    emit stateChanged();
}

bool SyncService::connectWith(const QString &provider, const QString &refreshToken, const QString &accessToken,
                              int expiresIn) {
    if (provider != Dropbox && provider != Drive)
        return false;
    // One provider at a time: connecting another one ends the first.
    if (m_connected && m_provider != provider)
        disconnectSync();
    QString error;
    if (!m_tokens || !m_tokens->write(provider, refreshToken, &error)) {
        setError(tr("The keyring couldn't keep the %1 sign-in: %2").arg(nameOf(provider), error));
        return false;
    }
    m_connected = true;
    m_provider = provider;
    m_accessToken = accessToken;
    m_accessExpiry = accessToken.isEmpty() ? 0 : QDateTime::currentMSecsSinceEpoch() + (qint64(expiresIn) - 60) * 1000;
    m_account.clear();
    m_error.clear();
    // Another account's folder holds other files; read everything afresh.
    m_cache.clear();
    m_settingsRev.clear();
    m_settingsCache = {};
    save();
    m_periodic.start();
    emit stateChanged();
    syncNow();
    return true;
}

void SyncService::disconnectSync() {
    cancelConnect();
    if (!m_connected)
        return;
    const QString provider = m_provider;
    const QString refreshToken = m_tokens ? m_tokens->read(provider) : QString();
    if (!refreshToken.isEmpty() && !m_factory) {
        // Ending the sign-in on the provider's side is a courtesy; forgetting
        // it here is what matters, so it doesn't wait for an answer.
        const QString access = m_accessToken;
        const qint64 expiry = m_accessExpiry;
        (void)QtConcurrent::run([provider, refreshToken, access, expiry] {
            const std::unique_ptr<OAuthRemote> remote = providerRemote(provider, refreshToken);
            if (!access.isEmpty())
                remote->setAccessToken(access, expiry);
            remote->revoke();
        });
    }
    if (m_tokens)
        m_tokens->clear(provider);
    m_connected = false;
    m_provider.clear();
    m_periodic.stop();
    m_soon.stop();
    m_accessToken.clear();
    m_account.clear();
    m_error.clear();
    save();
    emit stateChanged();
}
