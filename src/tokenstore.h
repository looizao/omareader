#pragma once

#include <QHash>
#include <QString>

// Where each provider's refresh token is kept, under the provider's name
// ("dropbox", "drive"). The desktop uses the Secret Service keyring
// (gnome-keyring on Omarchy), so a token never sits in a plain file; tests
// use one kept in memory.
class TokenStore {
public:
    virtual ~TokenStore() = default;
    // The token, or an empty string when there is none. error is set when the
    // keyring couldn't be asked at all.
    virtual QString read(const QString &service, QString *error = nullptr) = 0;
    virtual bool write(const QString &service, const QString &token, QString *error = nullptr) = 0;
    virtual bool clear(const QString &service, QString *error = nullptr) = 0;
};

class KeyringTokenStore : public TokenStore {
public:
    QString read(const QString &service, QString *error = nullptr) override;
    bool write(const QString &service, const QString &token, QString *error = nullptr) override;
    bool clear(const QString &service, QString *error = nullptr) override;
};

class MemoryTokenStore : public TokenStore {
public:
    QString read(const QString &service, QString * = nullptr) override { return m_tokens.value(service); }
    bool write(const QString &service, const QString &token, QString * = nullptr) override {
        m_tokens.insert(service, token);
        return true;
    }
    bool clear(const QString &service, QString * = nullptr) override {
        m_tokens.remove(service);
        return true;
    }

private:
    QHash<QString, QString> m_tokens;
};
