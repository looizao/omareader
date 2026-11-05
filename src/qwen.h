#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

// Where Qwen3-TTS lives on this machine: the Python environment `omareader
// setup --engine qwen` builds, the CustomVoice model and the forced aligner
// that times each word. Like Kokoro, nothing is ever downloaded outside
// setup; a model folder only counts once setup has verified its checksums.
struct QwenSetup {
    QString python;
    QString model;
    QString aligner;
    QString modelSize = QStringLiteral("0.6b");
    QString device = QStringLiteral("auto");
    QStringList searchedModelDirectories;

    bool ready() const { return !python.isEmpty() && !model.isEmpty() && !aligner.isEmpty(); }
    QString problem() const;
    QJsonObject toJson() const;

    static QwenSetup find(const QString &modelSize = QStringLiteral("0.6b"), const QString &device = QStringLiteral("auto"));
    // The folder holding the Python environment, under XDG_DATA_HOME.
    static QString runtimeDirectory();
    static QString modelFolderName(const QString &modelSize);
    static QString alignerFolderName();
    // Written by setup into a model folder once every file matched its checksum.
    static QString verifiedMarker();
};

// What the machine offers Qwen3-TTS, for doctor and the voice panel. Only
// NVIDIA GPUs are used (PyTorch's CUDA build); anything else runs on the CPU.
struct QwenHardware {
    bool nvidia = false;
    QString gpu;
    int totalMiB = 0;
    int freeMiB = 0;
    // A sentence about what to expect, empty when the GPU is comfortably big enough.
    QString advice(const QString &modelSize) const;
    QJsonObject toJson(const QString &modelSize) const;
    // Asks nvidia-smi; returns no GPU when it's missing or slow to answer.
    static QwenHardware probe(const QString &nvidiaSmi = {});
    // Rough free GPU memory each model needs, including the aligner's share.
    static int neededMiB(const QString &modelSize);
};

class QwenClient;

// A sentence for Qwen3-TTS to speak, and where its audio and timings go.
struct QwenItem {
    int id = 0;
    QString text;
    QString wav;
    QString tsv;
};

// One Qwen3-TTS process for the whole app, since a laptop GPU has room for
// the model once. The narrator, the preview and exports each are a client
// with a priority: whatever someone is listening to goes first, an export
// fills the gaps. The worker loads its models on first use and exits after a
// while with nothing to do, giving the GPU memory back.
class QwenWorker : public QObject {
    Q_OBJECT

public:
    struct Config {
        QString python;
        QString model;
        QString aligner;
        QString device;
        bool operator==(const Config &other) const {
            return python == other.python && model == other.model && aligner == other.aligner && device == other.device;
        }
        bool operator!=(const Config &other) const { return !(*this == other); }
    };

    static QwenWorker *shared();
    explicit QwenWorker(QObject *parent = nullptr);
    ~QwenWorker() override;

    void attach(QwenClient *client);
    void detach(QwenClient *client);
    // Called when a client's queue or hold changes.
    void schedule();
    // Tells the worker to stop on the client's requests from before its
    // latest submit or cancel, so it moves on to the new ones sooner.
    void cancelStale(QwenClient *client);
    void shutdown();
    bool isRunning() const { return m_process.state() != QProcess::NotRunning; }
    bool isLoaded() const { return m_ready; }

    // How long the worker stays loaded with nothing to do.
    static constexpr int IdleMilliseconds = 180000;
    // Requests the worker may hold at once: one being spoken, one being timed.
    static constexpr int InFlight = 2;
    void setIdleMilliseconds(int milliseconds) { m_idle.setInterval(milliseconds); }

    static QStringList arguments(const Config &config, const QString &script);

signals:
    void loadedChanged();

private:
    struct Sent {
        QPointer<QwenClient> client;
        quint64 epoch = 0;
        QList<QwenItem> items;
        bool cancelled = false;
    };

    void sendCancel(qint64 id, Sent &sent);

    bool startProcess(const Config &config);
    void readOutput();
    void handleMessage(const QJsonObject &message);
    void processFinished(const QString &error);
    void failAll(const QString &error);

    QProcess m_process;
    std::unique_ptr<QTemporaryDir> m_directory;
    Config m_config;
    bool m_ready = false;
    bool m_stopping = false;
    QByteArray m_output;
    QByteArray m_errors;
    QList<QPointer<QwenClient>> m_clients;
    QHash<qint64, Sent> m_sent;
    qint64 m_nextRequest = 1;
    QTimer m_idle;
};

// A client of the shared worker: a queue of sentences with one voice, which
// a Synthesizer fills and empties.
class QwenClient : public QObject {
    Q_OBJECT

public:
    using Item = QwenItem;

    QwenClient(int priority, QwenWorker *worker = nullptr, QObject *parent = nullptr);
    ~QwenClient() override;

    // Replaces whatever was queued; results of earlier requests still in the
    // worker are dropped when they arrive.
    void submit(const QwenWorker::Config &config, const QJsonObject &voice, const QList<Item> &items);
    void cancel();
    void setHeld(bool held);
    bool isHeld() const { return m_held; }
    bool busy() const { return !m_queue.isEmpty() || m_outstanding > 0; }
    int priority() const { return m_priority; }

signals:
    void itemDone(int id, bool ok, const QString &wav, const QString &tsv, const QString &error);
    void drained();
    void failed(const QString &error);

private:
    friend class QwenWorker;
    QwenWorker *m_worker;
    int m_priority;
    bool m_held = false;
    quint64 m_epoch = 0;
    QwenWorker::Config m_config;
    QJsonObject m_voice;
    QList<Item> m_queue;
    int m_outstanding = 0; // items sent and not yet answered
    bool m_firstBatch = true;
};
