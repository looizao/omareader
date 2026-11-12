#include "kokoro.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStandardPaths>
#include <csignal>
#include <unistd.h>

namespace {
const char *ModelFile = "kokoro-v1.0.onnx";
const char *VoicesFile = "voices-v1.0.bin";

QString tr(const char *text) { return QCoreApplication::translate("Kokoro", text); }
}

QStringList KokoroSetup::modelDirectories(const QString &modelDirectoryOverride) {
    QStringList directories;
    if (!modelDirectoryOverride.isEmpty())
        directories << modelDirectoryOverride;
    if (!qEnvironmentVariableIsEmpty("OMAREADER_MODELS"))
        directories << qEnvironmentVariable("OMAREADER_MODELS");
    const QString home = QDir::homePath();
    const QString dataHome = qEnvironmentVariableIsEmpty("XDG_DATA_HOME") ? home + "/.local/share"
                                                                          : qEnvironmentVariable("XDG_DATA_HOME");
    directories << dataHome + "/omareader/models" << home + "/.cache/kokoros" << "/usr/share/omareader/models"
                << "/usr/share/kokoros";
    directories.removeDuplicates();
    return directories;
}

KokoroSetup KokoroSetup::find(const QString &binaryOverride, const QString &modelDirectoryOverride) {
    KokoroSetup setup;
    if (!binaryOverride.isEmpty()) {
        const QFileInfo info(binaryOverride);
        if (info.isFile() && info.isExecutable())
            setup.binary = info.absoluteFilePath();
    } else {
        setup.binary = QStandardPaths::findExecutable("koko");
        if (setup.binary.isEmpty())
            setup.binary = QStandardPaths::findExecutable(
                "koko", {QDir::homePath() + "/.local/bin", QDir::homePath() + "/.cargo/bin"});
    }
    setup.searchedModelDirectories = modelDirectories(modelDirectoryOverride);
    for (const QString &directory : std::as_const(setup.searchedModelDirectories)) {
        const QString model = directory + "/" + ModelFile;
        const QString voices = directory + "/" + VoicesFile;
        if (setup.model.isEmpty() && QFileInfo(model).isFile())
            setup.model = model;
        if (setup.voices.isEmpty() && QFileInfo(voices).isFile())
            setup.voices = voices;
    }
    return setup;
}

QString KokoroSetup::problem() const {
    QStringList missing;
    if (binary.isEmpty())
        missing << tr("the Kokoros koko command was not found on PATH");
    if (model.isEmpty())
        missing << tr("the Kokoro model (%1) was not found").arg(ModelFile);
    if (voices.isEmpty())
        missing << tr("the Kokoro voices (%1) were not found").arg(VoicesFile);
    if (missing.isEmpty())
        return {};
    return tr("Read-aloud is unavailable: %1. Run omareader setup once to install it.").arg(missing.join("; "));
}

QJsonObject KokoroSetup::toJson() const {
    return {{"ready", ready()},
            {"koko", binary},
            {"model", model},
            {"voices", voices},
            {"searched", QJsonArray::fromStringList(searchedModelDirectories)},
            {"problem", problem()}};
}

QList<Voice> Voices::all() {
    // The voices in Kokoro v1.0's voices-v1.0.bin.
    static const QList<std::pair<const char *, const char *>> voices = {
        {"af_heart", "Heart"}, {"af_bella", "Bella"}, {"af_nicole", "Nicole"}, {"af_sarah", "Sarah"},
        {"af_sky", "Sky"}, {"af_nova", "Nova"}, {"af_river", "River"}, {"af_jessica", "Jessica"},
        {"af_kore", "Kore"}, {"af_aoede", "Aoede"}, {"af_alloy", "Alloy"},
        {"am_michael", "Michael"}, {"am_fenrir", "Fenrir"}, {"am_puck", "Puck"}, {"am_echo", "Echo"},
        {"am_eric", "Eric"}, {"am_liam", "Liam"}, {"am_onyx", "Onyx"}, {"am_adam", "Adam"},
        {"bf_emma", "Emma"}, {"bf_isabella", "Isabella"}, {"bf_alice", "Alice"}, {"bf_lily", "Lily"},
        {"bm_george", "George"}, {"bm_fable", "Fable"}, {"bm_lewis", "Lewis"}, {"bm_daniel", "Daniel"},
        {"ef_dora", "Dora"}, {"em_alex", "Alex"}, {"ff_siwis", "Siwis"},
        {"hf_alpha", "Alpha"}, {"hf_beta", "Beta"}, {"hm_omega", "Omega"}, {"hm_psi", "Psi"},
        {"if_sara", "Sara"}, {"im_nicola", "Nicola"},
        {"jf_alpha", "Alpha"}, {"jf_gongitsune", "Gongitsune"}, {"jm_kumo", "Kumo"},
        {"pf_dora", "Dora"}, {"pm_alex", "Alex"}, {"pm_santa", "Santa"},
        {"zf_xiaobei", "Xiaobei"}, {"zf_xiaoni", "Xiaoni"}, {"zm_yunjian", "Yunjian"}, {"zm_yunxi", "Yunxi"},
    };
    QList<Voice> out;
    for (const auto &[id, name] : voices)
        out.append({id, name, languageFor(id)});
    return out;
}

QString Voices::languageFor(const QString &voiceId) {
    switch (voiceId.isEmpty() ? 'a' : voiceId.at(0).toLatin1()) {
    case 'b': return "en-gb";
    case 'e': return "es";
    case 'f': return "fr-fr";
    case 'h': return "hi";
    case 'i': return "it";
    case 'j': return "ja";
    case 'p': return "pt-br";
    case 'z': return "cmn";
    default: return "en-us";
    }
}

KokoroRunner::KokoroRunner(QObject *parent) : QObject(parent) {
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    // Kokoros logs progress on stderr; the lines that matter are its
    // "Timestamps saved to" and "Error processing line" reports.
    connect(&m_process, &QProcess::readyReadStandardError, this, &KokoroRunner::readStandardError);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] { m_process.readAllStandardOutput(); });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        // A run that was stopped on purpose reports nothing more, not even
        // sentences still sitting in its output buffer.
        if (m_stopping) {
            m_errorBuffer.clear();
            return;
        }
        readStandardError();
        if (!m_errorBuffer.isEmpty())
            handleLine(QString::fromUtf8(m_errorBuffer));
        m_errorBuffer.clear();
        const bool ok = status == QProcess::NormalExit && code == 0;
        emit finished(ok, ok ? QString()
                             : tr("Kokoros stopped unexpectedly (exit code %1). %2")
                                   .arg(code)
                                   .arg(m_recentErrors.join(' ').trimmed()));
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && !m_stopping)
            emit finished(false, tr("Kokoros could not be started: %1").arg(m_process.errorString()));
    });
}

KokoroRunner::~KokoroRunner() { stop(); }

QStringList KokoroRunner::arguments(const Options &options, const QString &input, const QString &outputPattern) {
    return {"--model", options.setup.model,
            "--data", options.setup.voices,
            "--style", options.voice,
            "--lan", Voices::languageFor(options.voice),
            "--speed", QString::number(options.speed, 'f', 2),
            "--mono",
            "--timestamps",
            "file", input,
            "--output", outputPattern};
}

bool KokoroRunner::start(const Options &options, const QList<Line> &lines) {
    stop();
    m_stopping = false;
    m_recentErrors.clear();
    m_ids.clear();
    if (!options.setup.ready()) {
        emit finished(false, options.setup.problem());
        return false;
    }

    // Audio lands in a private temporary directory that goes away with the run.
    m_directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/omareader-XXXXXX");
    if (!m_directory->isValid()) {
        emit finished(false, tr("Could not create a temporary directory for the voice's audio."));
        return false;
    }
    QFile input(m_directory->filePath("sentences.txt"));
    if (!input.open(QIODevice::WriteOnly)) {
        emit finished(false, tr("Could not write the sentences for the voice."));
        return false;
    }
    for (const Line &line : lines) {
        QString text = line.text;
        text.replace('\n', ' ');
        text = text.simplified();
        if (text.isEmpty())
            continue;
        m_ids.append(line.id);
        input.write(text.toUtf8());
        input.write("\n");
    }
    input.close();
    if (m_ids.isEmpty()) {
        emit finished(true, {});
        return true;
    }

    m_held = false;
    m_process.setWorkingDirectory(m_directory->path());
    if (options.background)
        m_process.setChildProcessModifier([] { (void)::nice(10); });
    else
        m_process.setChildProcessModifier({});
    m_process.start(options.setup.binary,
                    arguments(options, input.fileName(), m_directory->filePath("s_{line}.wav")));
    return true;
}

void KokoroRunner::stop() {
    if (m_process.state() != QProcess::NotRunning) {
        m_stopping = true;
        if (m_held)
            ::kill(pid_t(m_process.processId()), SIGCONT);
        m_process.kill();
        m_process.waitForFinished(2000);
    }
    m_held = false;
    m_errorBuffer.clear();
    m_directory.reset();
}

void KokoroRunner::setHeld(bool held) {
    if (held == m_held || m_process.state() != QProcess::Running)
        return;
    m_held = held;
    ::kill(pid_t(m_process.processId()), held ? SIGSTOP : SIGCONT);
}

void KokoroRunner::readStandardError() {
    m_errorBuffer += m_process.readAllStandardError();
    if (m_stopping) {
        m_errorBuffer.clear();
        return;
    }
    int newline;
    while ((newline = int(m_errorBuffer.indexOf('\n'))) >= 0) {
        handleLine(QString::fromUtf8(m_errorBuffer.left(newline)));
        m_errorBuffer.remove(0, newline + 1);
    }
}

void KokoroRunner::handleLine(const QString &rawLine) {
    static const QRegularExpression ansi(R"(\x1B\[[0-9;]*[A-Za-z])");
    const QString line = QString(rawLine).remove(ansi).trimmed();
    if (line.isEmpty())
        return;
    static const QRegularExpression saved(R"(Timestamps saved to (.*s_(\d+)\.tsv)\s*$)");
    static const QRegularExpression failed(R"((?:Error processing line|No audio produced for line) (\d+):?\s*(.*)$)");
    const auto savedMatch = saved.match(line);
    if (savedMatch.hasMatch()) {
        const int lineNumber = savedMatch.captured(2).toInt();
        if (lineNumber >= 0 && lineNumber < m_ids.size()) {
            QString tsv = savedMatch.captured(1);
            if (QFileInfo(tsv).isRelative() && m_directory)
                tsv = QDir(m_process.workingDirectory()).filePath(tsv);
            QString wav = tsv;
            wav.replace(wav.size() - 4, 4, ".wav");
            emit sentenceReady(m_ids.at(lineNumber), wav, tsv);
        }
        return;
    }
    const auto failedMatch = failed.match(line);
    if (failedMatch.hasMatch()) {
        const int lineNumber = failedMatch.captured(1).toInt() - 1; // Kokoros counts from 1 here
        if (lineNumber >= 0 && lineNumber < m_ids.size())
            emit sentenceFailed(m_ids.at(lineNumber), failedMatch.captured(2));
        return;
    }
    if (line.contains("error", Qt::CaseInsensitive) || line.contains("panicked")) {
        m_recentErrors.append(line.right(300));
        if (m_recentErrors.size() > 3)
            m_recentErrors.removeFirst();
    }
}

// Compiled as part of this file, not on its own; see omareader.pro.
#include "moc_kokoro.cpp"
