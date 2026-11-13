#pragma once

#include <QObject>
#include <QUrl>

// The open and folder dialogs behind an interface, so tests can inject a fake picker.
// Follows Omacut (MIT, David Heinemeier Hansson).
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openBook() = 0;
    // Asks for a folder: "export" for where audio exports go, "sync" for
    // the folder bookmarks and notes are kept in step through. The answer
    // carries the same purpose, so only the one who asked takes it.
    virtual void chooseFolder(const QString &current, const QString &purpose) {
        Q_UNUSED(current);
        Q_UNUSED(purpose);
    }

signals:
    void openSelected(const QUrl &url);
    void folderSelected(const QUrl &url, const QString &purpose);
    void failed(const QString &message);
};
