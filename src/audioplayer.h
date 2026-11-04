#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QTimer>
#include <memory>

class QAudioSink;
class QIODevice;

// Plays the narration as one continuous stream: each sentence's audio is
// appended as it arrives, and the position is reported in frames on the same
// timeline, so the narrator can tell which word is being heard.
class AudioPlayer : public QObject {
    Q_OBJECT

public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

    // Opens the default output; false (with error()) when there is none.
    bool open(int sampleRate);
    void append(const QByteArray &monoFloatSamples);
    void pause();
    void resume();
    // Releases the output device and resets the timeline.
    void close();

    bool isOpen() const { return m_sink != nullptr; }
    bool isPaused() const { return m_paused; }
    int sampleRate() const { return m_sampleRate; }
    qint64 queuedFrames() const { return m_appendedFrames; }
    // Frames heard so far, on the timeline append() builds.
    qint64 playedFrames() const;
    QString error() const { return m_error; }

signals:
    void errorOccurred(const QString &message);

private:
    void pump();

    std::unique_ptr<QAudioSink> m_sink;
    QIODevice *m_device = nullptr;
    QAudioFormat m_format;
    int m_sampleRate = 0;
    QByteArray m_pending; // converted to the sink's format, not yet written
    qint64 m_appendedFrames = 0;
    qint64 m_writtenBytes = 0;
    bool m_paused = false;
    QTimer m_pump;
    QString m_error;
};
