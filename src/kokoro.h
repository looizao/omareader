#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>

// Where Kokoros and its model live, found on this machine. omareader never
// downloads anything: Kokoros itself would fetch a missing model from the
// network, so synthesis only starts when both files are already on disk.
struct KokoroSetup {
    QString binary;
    QString model;
    QString voices;
    QStringList searchedModelDirectories;

    bool ready() const { return !binary.isEmpty() && !model.isEmpty() && !voices.isEmpty(); }
    // What's missing, in a sentence the window can show; empty when ready.
    QString problem() const;
    QJsonObject toJson() const;

    // binaryOverride and modelDirectoryOverride come from settings; empty
    // means search the usual places.
    static KokoroSetup find(const QString &binaryOverride = {}, const QString &modelDirectoryOverride = {});
    static QStringList modelDirectories(const QString &modelDirectoryOverride = {});
};

struct Voice {
    QString id;
    QString name;
    QString language; // the espeak-ng code Kokoros takes with -l
};

namespace Voices {
QList<Voice> all();
// Kokoro voice ids start with a language letter: a American English,
// b British English, e Spanish, f French, h Hindi, i Italian, j Japanese,
// p Brazilian Portuguese, z Mandarin.
QString languageFor(const QString &voiceId);
}

// One run of `koko file`: writes the sentences one per line, lets Kokoros
// synthesise them in order with word timestamps, and reports each sentence's
// WAV and TSV as it lands. The model loads once per run.
class KokoroRunner : public QObject {
    Q_OBJECT

public:
    struct Line {
        int id;
        QString text;
    };
    struct Options {
        KokoroSetup setup;
        QString voice;
        double speed = 1.0;
    };

    explicit KokoroRunner(QObject *parent = nullptr);
    ~KokoroRunner() override;

    bool start(const Options &options, const QList<Line> &lines);
    void stop();
    // Holds the process (SIGSTOP) while playback has plenty buffered, so a
    // long chapter doesn't fill the disk and memory ahead of the listener.
    void setHeld(bool held);
    bool isRunning() const { return m_process.state() != QProcess::NotRunning; }
    bool isHeld() const { return m_held; }

    static QStringList arguments(const Options &options, const QString &input, const QString &outputPattern);

signals:
    void sentenceReady(int id, const QString &wavPath, const QString &tsvPath);
    void sentenceFailed(int id, const QString &message);
    void finished(bool ok, const QString &error);

private:
    void readStandardError();
    void handleLine(const QString &line);

    QProcess m_process;
    std::unique_ptr<QTemporaryDir> m_directory;
    QList<int> m_ids; // line number in the input file -> sentence id
    QByteArray m_errorBuffer;
    QStringList m_recentErrors;
    bool m_held = false;
    bool m_stopping = false;
};
