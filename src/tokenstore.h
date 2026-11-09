#pragma once

#include <QString>

// Where the Dropbox refresh token is kept. The desktop uses the Secret
// Service keyring (gnome-keyring on Omarchy), so the token never sits in a
// plain file; tests use one kept in memory.
class TokenStore {
public:
    virtual ~TokenStore() = default;
    // The token, or an empty string when there is none. error is set when the
    // keyring couldn't be asked at all.
    virtual QString read(QString *error = nullptr) = 0;
    virtual bool write(const QString &token, QString *error = nullptr) = 0;
    virtual bool clear(QString *error = nullptr) = 0;
};

class KeyringTokenStore : public TokenStore {
public:
    QString read(QString *error = nullptr) override;
    bool write(const QString &token, QString *error = nullptr) override;
    bool clear(QString *error = nullptr) override;
};

class MemoryTokenStore : public TokenStore {
public:
    QString read(QString * = nullptr) override { return m_token; }
    bool write(const QString &token, QString * = nullptr) override {
        m_token = token;
        return true;
    }
    bool clear(QString * = nullptr) override {
        m_token.clear();
        return true;
    }

private:
    QString m_token;
};
