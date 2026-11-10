#include "tokenstore.h"

// GLib names a struct member "signals", which Qt defines as a keyword.
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")

namespace {

const SecretSchema *schema() {
    // The schema kept its first name when Google Drive joined Dropbox; the
    // service attribute tells the two sign-ins apart.
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

QString KeyringTokenStore::read(const QString &service, QString *error) {
    const QByteArray name = service.toUtf8();
    GError *gerror = nullptr;
    gchar *secret = secret_password_lookup_sync(schema(), nullptr, &gerror, "service", name.constData(), nullptr);
    if (gerror) {
        take(gerror, error);
        return {};
    }
    const QString token = secret ? QString::fromUtf8(secret) : QString();
    if (secret)
        secret_password_free(secret);
    return token;
}

bool KeyringTokenStore::write(const QString &service, const QString &token, QString *error) {
    const QByteArray name = service.toUtf8();
    const QByteArray label = "omareader " + name + " sync";
    GError *gerror = nullptr;
    const QByteArray value = token.toUtf8();
    const bool ok = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, label.constData(),
                                               value.constData(), nullptr, &gerror, "service", name.constData(), nullptr);
    if (gerror) {
        take(gerror, error);
        return false;
    }
    return ok;
}

bool KeyringTokenStore::clear(const QString &service, QString *error) {
    const QByteArray name = service.toUtf8();
    GError *gerror = nullptr;
    secret_password_clear_sync(schema(), nullptr, &gerror, "service", name.constData(), nullptr);
    if (gerror) {
        take(gerror, error);
        return false;
    }
    return true;
}
