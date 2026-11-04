#pragma once

#include <QObject>
#include <QUrl>

// The open dialog behind an interface, so tests can inject a fake picker.
// Follows Omacut (MIT, David Heinemeier Hansson).
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openBook() = 0;

signals:
    void openSelected(const QUrl &url);
    void failed(const QString &message);
};
