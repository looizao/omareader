#pragma once

#include <QVariantMap>

#include "filepicker.h"

class QDBusPendingCallWatcher;

// The xdg-desktop-portal FileChooser, never QFileDialog, so the dialog is the
// desktop's own. Follows Omacut (MIT, David Heinemeier Hansson).
class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    void openBook() override;
    void chooseFolder(const QString &current, const QString &purpose) override;

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    void requestFile(const QString &method, const QString &title, QVariantMap options);
    bool connectToRequestPath(const QString &path);
    void clearPending();
    // Best-effort Close of the pending portal request, if there is one.
    void closePendingRequest();

    QString m_pendingPath;
    bool m_pendingFolder = false; // the pending request picks a folder
    QString m_pendingPurpose;
    bool m_pending = false;
    // Bumped per request, so a late reply for a replaced request is ignored.
    int m_pendingGeneration = 0;
    QDBusPendingCallWatcher *m_pendingWatcher = nullptr;
};
