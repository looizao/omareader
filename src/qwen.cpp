#include "qwen.h"

#include "kokoro.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QProcessEnvironment>
#include <QStandardPaths>

// The worker's own log, off unless QT_LOGGING_RULES turns on
// "omareader.qwen.debug", for finding out why a batch was slow or failed.
Q_LOGGING_CATEGORY(qwenLog, "omareader.qwen", QtInfoMsg)

namespace {
QString tr(const char *text) { return QCoreApplication::translate("Qwen", text); }

// Requests carry up to this many sentences: batches keep the GPU busy, and
// four keep a single request short enough not to hold up the narrator.
constexpr int BatchSize = 4;

QString dataHome() {
    return qEnvironmentVariableIsEmpty("XDG_DATA_HOME") ? QDir::homePath() + "/.local/share"
                                                        : qEnvironmentVariable("XDG_DATA_HOME");
}

bool verifiedFolder(const QString &path) { return QFileInfo(path + "/" + QwenSetup::verifiedMarker()).isFile(); }
}

QString QwenSetup::runtimeDirectory() { return dataHome() + "/omareader/qwen"; }

QString QwenSetup::modelFolderName(const QString &modelSize) {
    return modelSize == "1.7b" ? "Qwen3-TTS-12Hz-1.7B-CustomVoice" : "Qwen3-TTS-12Hz-0.6B-CustomVoice";
}

QString QwenSetup::alignerFolderName() { return "Qwen3-ForcedAligner-0.6B"; }

QString QwenSetup::verifiedMarker() { return ".omareader-verified"; }

QwenSetup QwenSetup::find(const QString &modelSize, const QString &device) {
    QwenSetup setup;
    setup.modelSize = modelSize == "1.7b" ? "1.7b" : "0.6b";
    setup.device = device.isEmpty() ? "auto" : device;
    // Setup marks the environment once every package is installed, so one
    // that stopped halfway isn't mistaken for a working one.
    const QFileInfo python(runtimeDirectory() + "/venv/bin/python");
    if (python.isFile() && python.isExecutable() && verifiedFolder(runtimeDirectory() + "/venv"))
        setup.python = python.absoluteFilePath();
    for (const QString &directory : KokoroSetup::modelDirectories()) {
        const QString qwen = directory + "/qwen";
        setup.searchedModelDirectories << qwen;
        const QString model = qwen + "/" + modelFolderName(setup.modelSize);
        const QString aligner = qwen + "/" + alignerFolderName();
        if (setup.model.isEmpty() && verifiedFolder(model))
            setup.model = model;
        if (setup.aligner.isEmpty() && verifiedFolder(aligner))
            setup.aligner = aligner;
    }
    return setup;
}

QString QwenSetup::problem() const {
    QStringList missing;
    if (python.isEmpty())
        missing << tr("its Python environment was not found");
    if (model.isEmpty())
        missing << tr("the %1 model was not found").arg(modelSize == "1.7b" ? "1.7B" : "0.6B");
    if (aligner.isEmpty())
        missing << tr("the forced aligner that times each word was not found");
    if (missing.isEmpty())
        return {};
    const QString command = modelSize == "1.7b" ? "omareader setup --engine qwen --model 1.7b"
                                                : "omareader setup --engine qwen";
    return tr("Qwen3-TTS is unavailable: %1. Run %2 once to install it.").arg(missing.join("; "), command);
}

QJsonObject QwenSetup::toJson() const {
    return {{"ready", ready()},
            {"python", python},
            {"model", model},
            {"modelSize", modelSize},
            {"aligner", aligner},
            {"device", device},
            {"searched", QJsonArray::fromStringList(searchedModelDirectories)},
            {"problem", problem()}};
}

int QwenHardware::neededMiB(const QString &modelSize) {
    // Measured with batches of four sentences and the aligner on the CPU:
    // the 0.6B model read in real time with 4290 MiB free as PyTorch counts
    // it, splitting a batch that doesn't fit; the 1.7B one ran out of memory
    // with 6 GB. Below these, the worker uses the CPU.
    return modelSize == "1.7b" ? 7000 : 4000;
}

QString QwenHardware::advice(const QString &modelSize) const {
    if (!nvidia)
        return tr("No NVIDIA GPU was found, so Qwen3-TTS runs on the CPU at about a third of real time: fine for "
                  "an export left running, too slow to listen along. Kokoro reads in real time on any CPU.");
    if (freeMiB >= neededMiB(modelSize))
        return {};
    if (modelSize == "1.7b" && freeMiB >= neededMiB("0.6b"))
        return tr("%1 has %2 MB free and the 1.7B model needs about %3 MB, so it would run on the CPU, about "
                  "three times slower than real time. The 0.6B model fits.")
            .arg(gpu)
            .arg(freeMiB)
            .arg(neededMiB(modelSize));
    return tr("%1 has %2 MB free and the %3 model needs about %4 MB, so it would run on the CPU, about three "
              "times slower than real time. Closing other programs that use the GPU may free enough.")
        .arg(gpu)
        .arg(freeMiB)
        .arg(modelSize == "1.7b" ? "1.7B" : "0.6B")
        .arg(neededMiB(modelSize));
}

QJsonObject QwenHardware::toJson(const QString &modelSize) const {
    return {{"nvidia", nvidia},
            {"gpu", gpu},
            {"totalMiB", totalMiB},
            {"freeMiB", freeMiB},
            {"neededMiB", neededMiB(modelSize)},
            {"advice", advice(modelSize)}};
}

QwenHardware QwenHardware::probe(const QString &nvidiaSmi) {
    QwenHardware hardware;
    const QString program = nvidiaSmi.isEmpty() ? QStandardPaths::findExecutable("nvidia-smi") : nvidiaSmi;
    if (program.isEmpty())
        return hardware;
    QProcess process;
    process.start(program, {"--query-gpu=name,memory.total,memory.free", "--format=csv,noheader,nounits"});
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        process.kill();
        return hardware;
    }
    const QString line = QString::fromUtf8(process.readAllStandardOutput()).split('\n').value(0).trimmed();
    const QStringList fields = line.split(',');
    if (fields.size() < 3)
        return hardware;
    hardware.nvidia = true;
    hardware.gpu = fields.at(0).trimmed();
    hardware.totalMiB = fields.at(1).trimmed().toInt();
    hardware.freeMiB = fields.at(2).trimmed().toInt();
    return hardware;
}

QwenWorker *QwenWorker::shared() {
    static QPointer<QwenWorker> worker;
    if (!worker)
        worker = new QwenWorker(QCoreApplication::instance());
    return worker;
}

QwenWorker::QwenWorker(QObject *parent) : QObject(parent) {
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    m_idle.setSingleShot(true);
    m_idle.setInterval(IdleMilliseconds);
    connect(&m_idle, &QTimer::timeout, this, &QwenWorker::shutdown);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &QwenWorker::readOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        // Only the end matters: it says why the process failed, if it did.
        const QByteArray text = m_process.readAllStandardError();
        if (qwenLog().isDebugEnabled())
            for (const QByteArray &line : text.split('\n'))
                if (!line.trimmed().isEmpty())
                    qCDebug(qwenLog).noquote() << QString::fromUtf8(line);
        m_errors += text;
        if (m_errors.size() > 4000)
            m_errors = m_errors.right(4000);
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readOutput();
        if (m_stopping)
            return;
        processFinished(tr("Qwen3-TTS stopped unexpectedly (%1). %2")
                            .arg(status == QProcess::CrashExit ? tr("it crashed") : tr("exit code %1").arg(code))
                            .arg(QString::fromUtf8(m_errors).trimmed().right(300)));
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && !m_stopping)
            processFinished(tr("Qwen3-TTS could not be started: %1").arg(m_process.errorString()));
    });
}

QwenWorker::~QwenWorker() {
    m_stopping = true;
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(2000);
    }
}

QStringList QwenWorker::arguments(const Config &config, const QString &script) {
    // -I keeps the user's site-packages and the working folder out of Python's path.
    const QString modelSize = QFileInfo(config.model).fileName().contains("1.7B") ? "1.7b" : "0.6b";
    return {"-I",      script,
            "--model", config.model,
            "--aligner", config.aligner,
            "--device", config.device.isEmpty() ? "auto" : config.device,
            "--gpu-mib", QString::number(QwenHardware::neededMiB(modelSize))};
}

void QwenWorker::attach(QwenClient *client) {
    if (!m_clients.contains(client))
        m_clients.append(client);
}

void QwenWorker::detach(QwenClient *client) {
    m_clients.removeAll(client);
    for (auto it = m_sent.begin(); it != m_sent.end(); ++it)
        if (it->client == client) {
            it->client = nullptr;
            sendCancel(it.key(), it.value());
        }
    schedule();
}

void QwenWorker::cancelStale(QwenClient *client) {
    for (auto it = m_sent.begin(); it != m_sent.end(); ++it)
        if (it->client == client && it->epoch != client->m_epoch)
            sendCancel(it.key(), it.value());
}

void QwenWorker::sendCancel(qint64 id, Sent &sent) {
    // The worker still answers a cancelled request, so it stays counted as
    // in flight until then; it just answers much sooner.
    if (sent.cancelled || m_process.state() != QProcess::Running)
        return;
    sent.cancelled = true;
    m_process.write(QJsonDocument(QJsonObject{{"op", "cancel"}, {"req", id}}).toJson(QJsonDocument::Compact) + "\n");
}

bool QwenWorker::startProcess(const Config &config) {
    m_stopping = false;
    m_ready = false;
    m_output.clear();
    m_errors.clear();
    m_sent.clear();
    m_config = config;
    m_directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/omareader-qwen-XXXXXX");
    QFile bundled(":/qwen-worker.py");
    QFile script(m_directory->isValid() ? m_directory->filePath("qwen-worker.py") : QString());
    if (!m_directory->isValid() || !bundled.open(QIODevice::ReadOnly) || !script.open(QIODevice::WriteOnly)
        || script.write(bundled.readAll()) < 0) {
        failAll(tr("Could not prepare the Qwen3-TTS worker."));
        return false;
    }
    script.close();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("HF_HUB_OFFLINE", "1");
    environment.insert("TRANSFORMERS_OFFLINE", "1");
    environment.insert("PYTHONUNBUFFERED", "1");
    // Fewer, larger blocks leave a small GPU less fragmented.
    environment.insert("PYTORCH_CUDA_ALLOC_CONF", "expandable_segments:True");
    m_process.setProcessEnvironment(environment);
    m_process.setWorkingDirectory(m_directory->path());
    m_process.start(config.python, arguments(config, script.fileName()));
    emit loadedChanged();
    return true;
}

void QwenWorker::shutdown() {
    m_idle.stop();
    if (m_process.state() == QProcess::NotRunning)
        return;
    m_stopping = true;
    m_process.write("{\"op\":\"quit\"}\n");
    if (!m_process.waitForFinished(3000)) {
        m_process.kill();
        m_process.waitForFinished(2000);
    }
    m_ready = false;
    // Whatever was in flight is gone; its clients are told nothing more,
    // since nobody shuts the worker down while someone is waiting.
    for (const Sent &sent : std::as_const(m_sent))
        if (sent.client)
            sent.client->m_outstanding = qMax(0, sent.client->m_outstanding - int(sent.items.size()));
    m_sent.clear();
    m_directory.reset();
    emit loadedChanged();
}

void QwenWorker::schedule() {
    m_clients.removeAll(nullptr);
    auto best = [this]() -> QwenClient * {
        QwenClient *chosen = nullptr;
        for (const auto &client : std::as_const(m_clients))
            if (client && !client->m_held && !client->m_queue.isEmpty()
                && (!chosen || client->m_priority > chosen->m_priority))
                chosen = client;
        return chosen;
    };
    QwenClient *next = best();
    if (!next) {
        if (m_sent.isEmpty() && m_process.state() != QProcess::NotRunning && !m_idle.isActive())
            m_idle.start();
        return;
    }
    m_idle.stop();
    if (m_process.state() == QProcess::NotRunning) {
        startProcess(next->m_config);
        return;
    }
    if (!m_ready)
        return; // ready will schedule again
    if (next->m_config != m_config) {
        // Another model or device: wait for what's in flight, then reload.
        if (!m_sent.isEmpty())
            return;
        m_stopping = true;
        m_process.kill();
        m_process.waitForFinished(2000);
        startProcess(next->m_config);
        return;
    }
    while (m_sent.size() < InFlight && (next = best()) && next->m_config == m_config) {
        // The first batch is a single sentence, so listening starts as soon
        // as possible; after that, batches keep the GPU busy.
        const int count = qMin(next->m_firstBatch ? 1 : BatchSize, int(next->m_queue.size()));
        next->m_firstBatch = false;
        QJsonObject request = next->m_voice;
        const qint64 id = m_nextRequest++;
        request.insert("op", "synth");
        request.insert("req", id);
        QJsonArray items;
        Sent sent{next, next->m_epoch, {}};
        for (int i = 0; i < count; ++i) {
            const QwenClient::Item item = next->m_queue.takeFirst();
            items.append(QJsonObject{{"text", item.text}, {"wav", item.wav}, {"tsv", item.tsv}});
            sent.items.append(item);
        }
        request.insert("items", items);
        next->m_outstanding += count;
        m_sent.insert(id, sent);
        m_process.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n");
    }
}

void QwenWorker::readOutput() {
    m_output += m_process.readAllStandardOutput();
    int newline;
    while ((newline = int(m_output.indexOf('\n'))) >= 0) {
        const QByteArray line = m_output.left(newline).trimmed();
        m_output.remove(0, newline + 1);
        if (line.isEmpty())
            continue;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (document.isObject())
            handleMessage(document.object());
    }
}

void QwenWorker::handleMessage(const QJsonObject &message) {
    const QString event = message.value("event").toString();
    if (event == "ready") {
        m_ready = true;
        emit loadedChanged();
        schedule();
        return;
    }
    if (event == "fatal") {
        m_stopping = true;
        m_process.kill();
        m_process.waitForFinished(2000);
        processFinished(message.value("message").toString());
        return;
    }
    if (event != "done")
        return;
    const qint64 id = message.value("req").toInteger();
    if (!m_sent.contains(id))
        return;
    const Sent sent = m_sent.take(id);
    const QJsonArray results = message.value("items").toArray();
    QPointer<QwenClient> client = sent.client;
    if (client)
        client->m_outstanding = qMax(0, client->m_outstanding - int(sent.items.size()));
    for (int i = 0; i < sent.items.size(); ++i) {
        // Stale results, from before the client moved on, are dropped; their
        // files go with the client's folder.
        if (!client || sent.epoch != client->m_epoch)
            break;
        const QwenItem &item = sent.items.at(i);
        const QJsonObject result = results.at(i).toObject();
        emit client->itemDone(item.id, result.value("ok").toBool(), item.wav, item.tsv,
                              result.value("error").toString());
    }
    if (client && sent.epoch == client->m_epoch && client->m_queue.isEmpty() && client->m_outstanding == 0)
        emit client->drained();
    schedule();
}

void QwenWorker::processFinished(const QString &error) {
    m_ready = false;
    m_sent.clear();
    m_directory.reset();
    emit loadedChanged();
    failAll(error);
}

void QwenWorker::failAll(const QString &error) {
    // Every client with work learns why; none of them falls back to another
    // engine on its own.
    const auto clients = m_clients;
    for (const auto &client : clients) {
        if (!client || !client->busy())
            continue;
        client->m_queue.clear();
        client->m_outstanding = 0;
        emit client->failed(error);
    }
}

QwenClient::QwenClient(int priority, QwenWorker *worker, QObject *parent)
    : QObject(parent), m_worker(worker ? worker : QwenWorker::shared()), m_priority(priority) {
    m_worker->attach(this);
}

QwenClient::~QwenClient() {
    if (m_worker)
        m_worker->detach(this);
}

void QwenClient::submit(const QwenWorker::Config &config, const QJsonObject &voice, const QList<Item> &items) {
    ++m_epoch;
    m_config = config;
    m_voice = voice;
    m_queue = items;
    m_held = false;
    m_firstBatch = true;
    m_worker->cancelStale(this);
    m_worker->schedule();
}

void QwenClient::cancel() {
    ++m_epoch;
    m_queue.clear();
    m_held = false;
    m_worker->cancelStale(this);
    m_worker->schedule();
}

void QwenClient::setHeld(bool held) {
    if (held == m_held)
        return;
    m_held = held;
    m_worker->schedule();
}

// Compiled as part of this file, not on its own; see omareader.pro.
#include "moc_qwen.cpp"
