#include "tokenstore.h"

// GLib names a struct member "signals", which Qt defines as a keyword.
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")

namespace {

const SecretSchema *schema() {
    static const SecretSchema value = {"io.github.looizao.omareader.Dropbox",
                                       SECRET_SCHEMA_NONE,
                                       {{"service", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
                                       0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    return &value;
}

QString take(GError *gerror, QString *error) {
    const QString message = gerror ? QString::fromUtf8(gerror->message) : QString();
    if (gerror)
        g_error_free(gerror);
    if (error)
        *error = message;
    return message;
}

}

QString KeyringTokenStore::read(QString *error) {
    GError *gerror = nullptr;
    gchar *secret = secret_password_lookup_sync(schema(), nullptr, &gerror, "service", "dropbox", nullptr);
    if (gerror) {
        take(gerror, error);
        return {};
    }
    const QString token = secret ? QString::fromUtf8(secret) : QString();
    if (secret)
        secret_password_free(secret);
    return token;
}

bool KeyringTokenStore::write(const QString &token, QString *error) {
    GError *gerror = nullptr;
    const QByteArray value = token.toUtf8();
    const bool ok = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, "omareader Dropbox sync",
                                               value.constData(), nullptr, &gerror, "service", "dropbox", nullptr);
    if (gerror) {
        take(gerror, error);
        return false;
    }
    return ok;
}

bool KeyringTokenStore::clear(QString *error) {
    GError *gerror = nullptr;
    secret_password_clear_sync(schema(), nullptr, &gerror, "service", "dropbox", nullptr);
    if (gerror) {
        take(gerror, error);
        return false;
    }
    return true;
}
