#include "cli.h"

#include "bookloader.h"
#include "dropbox.h"
#include "exporter.h"
#include "kokoro.h"
#include "library.h"
#include "qwen.h"
#include "sync.h"
#include "synthesizer.h"
#include "tokenstore.h"
#include "voicesettings.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>
#include <QTimer>
#include <csignal>
#include <cstdio>

namespace {

volatile std::sig_atomic_t interrupted = 0;

QTextStream &out() {
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err() {
    static QTextStream stream(stderr);
    return stream;
}

int fail(const QString &message) {
    err() << "omareader: " << message << Qt::endl;
    return 1;
}

void printJson(const QJsonObject &object) {
    out() << QJsonDocument(object).toJson(QJsonDocument::Indented);
    out().flush();
}

struct Command {
    QCommandLineParser parser;
    Command(const QString &name, const QString &description) {
        parser.setApplicationDescription(description);
        parser.addHelpOption();
        Q_UNUSED(name);
    }
    void json() { parser.addOption({"json", "Print the result as JSON"}); }
    bool parse(const QStringList &arguments) {
        if (!parser.parse(arguments)) {
            fail(parser.errorText());
            return false;
        }
        if (parser.isSet("help")) {
            out() << parser.helpText();
            out().flush();
            return false;
        }
        return true;
    }
};

std::shared_ptr<Book> loadBook(const QString &path) {
    QString error;
    auto book = BookLoader::load(path, &error);
    if (!book)
        fail(error);
    return book;
}

void addVoiceOptions(QCommandLineParser &parser) {
    parser.addOption({"engine", "kokoro or qwen (the window's choice by default)", "engine"});
    parser.addOption({"voice", "One of the engine's voices, such as af_heart or ryan (see omareader voices)", "voice"});
    parser.addOption({"language", "Qwen3-TTS only: the language to read in, such as English or Portuguese", "language"});
    parser.addOption({"speed", "Speaking speed from 0.5 to 2.0", "speed"});
    parser.addOption({"tune", "Change one tuning setting for this run, such as temperature=0.6 (repeatable; "
                              "see omareader voices)",
                      "key=value"});
}

// The voice a command speaks with: the window's saved settings for the
// engine, with whatever the command line overrides. Nothing is saved.
bool voiceFromOptions(const QCommandLineParser &parser, VoiceSettings *voice) {
    QString engine = VoiceSettings::selectedEngine();
    if (parser.isSet("engine")) {
        engine = parser.value("engine").toLower();
        if (!Engine::isKnown(engine)) {
            fail("The engine is kokoro or qwen.");
            return false;
        }
    }
    *voice = VoiceSettings::load(engine);
    QList<QPair<QString, QString>> overrides;
    for (const char *key : {"voice", "language", "speed"})
        if (parser.isSet(key))
            overrides.append({key, parser.value(key)});
    for (const QString &tune : parser.values("tune")) {
        const qsizetype equals = tune.indexOf('=');
        if (equals <= 0) {
            fail("Use --tune key=value, such as --tune temperature=0.6.");
            return false;
        }
        overrides.append({tune.left(equals).trimmed(), tune.mid(equals + 1).trimmed()});
    }
    for (const auto &[key, value] : std::as_const(overrides)) {
        QString error;
        if (!voice->set(key, value, &error)) {
            fail(error);
            return false;
        }
    }
    return true;
}

QString engineOption(const QCommandLineParser &parser, bool *ok) {
    const QString engine = parser.isSet("engine") ? parser.value("engine").toLower() : VoiceSettings::selectedEngine();
    *ok = Engine::isKnown(engine);
    if (!*ok)
        fail("The engine is kokoro or qwen.");
    return engine;
}

int info(const QStringList &arguments) {
    Command command("info", "Show a book's metadata and chapters.");
    command.parser.addPositionalArgument("book", "The book file");
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const QStringList positional = command.parser.positionalArguments();
    if (positional.size() != 1)
        return fail("Use: omareader info <book> [--json]");
    const auto book = loadBook(positional.first());
    if (!book)
        return 1;

    QJsonArray chapters;
    for (int c = 0; c < book->chapters.size(); ++c) {
        const int first = book->chapterFirstSentence.at(c);
        const int last = c + 1 < book->chapters.size() ? book->chapterFirstSentence.at(c + 1) : book->sentenceCount();
        int words = 0;
        for (int s = first; s < last; ++s)
            words += int(book->sentences.at(s).words.size());
        chapters.append(QJsonObject{{"number", c + 1},
                                    {"title", book->chapters.at(c).title},
                                    {"paragraphs", int(book->chapters.at(c).paragraphs.size())},
                                    {"sentences", last - first},
                                    {"words", words}});
    }
    if (command.parser.isSet("json")) {
        printJson({{"path", book->path},
                   {"title", book->title},
                   {"author", book->author},
                   {"format", book->format},
                   {"hasCover", !book->cover.isEmpty()},
                   {"sentences", book->sentenceCount()},
                   {"words", book->wordCount()},
                   {"chapters", chapters}});
        return 0;
    }
    out() << book->title << Qt::endl;
    if (!book->author.isEmpty())
        out() << book->author << Qt::endl;
    out() << book->format.toUpper() << ", " << book->wordCount() << " words" << Qt::endl << Qt::endl;
    for (const auto &value : std::as_const(chapters)) {
        const QJsonObject c = value.toObject();
        out() << QString("%1. %2 (%3 words)").arg(c.value("number").toInt(), 3).arg(c.value("title").toString())
                     .arg(c.value("words").toInt())
              << Qt::endl;
    }
    return 0;
}

int text(const QStringList &arguments) {
    Command command("text", "Print a book's text as omareader reads it.");
    command.parser.addPositionalArgument("book", "The book file");
    command.parser.addOption({"chapter", "Only this chapter, counting from 1", "number"});
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const QStringList positional = command.parser.positionalArguments();
    if (positional.size() != 1)
        return fail("Use: omareader text <book> [--chapter N] [--json]");
    const auto book = loadBook(positional.first());
    if (!book)
        return 1;

    int from = 0, to = int(book->chapters.size());
    if (command.parser.isSet("chapter")) {
        bool ok = false;
        const int chapter = command.parser.value("chapter").toInt(&ok);
        if (!ok || chapter < 1 || chapter > book->chapters.size())
            return fail(QString("The book has chapters 1 to %1.").arg(book->chapters.size()));
        from = chapter - 1;
        to = chapter;
    }
    if (command.parser.isSet("json")) {
        QJsonArray chapters;
        for (int c = from; c < to; ++c) {
            QJsonArray paragraphs;
            for (const auto &p : book->chapters.at(c).paragraphs)
                paragraphs.append(QJsonObject{{"text", p.text}, {"heading", p.level}});
            chapters.append(QJsonObject{{"number", c + 1}, {"title", book->chapters.at(c).title}, {"paragraphs", paragraphs}});
        }
        printJson({{"title", book->title}, {"author", book->author}, {"chapters", chapters}});
        return 0;
    }
    for (int c = from; c < to; ++c) {
        for (const auto &p : book->chapters.at(c).paragraphs)
            out() << QString(p.text).replace('\n', ' ') << Qt::endl;
        if (c + 1 < to)
            out() << Qt::endl;
    }
    return 0;
}

// "3" or "2-5", counting from 1, into an inclusive 0-based range.
bool parseChapters(const QString &value, int count, int *first, int *last) {
    const QStringList parts = value.split('-');
    bool okFirst = false, okLast = false;
    const int from = parts.value(0).toInt(&okFirst);
    const int to = parts.size() == 2 ? parts.at(1).toInt(&okLast) : from;
    if (parts.size() > 2 || !okFirst || (parts.size() == 2 && !okLast) || from < 1 || to < from || to > count)
        return false;
    *first = from - 1;
    *last = to - 1;
    return true;
}

int exportAudio(const QStringList &arguments) {
    Command command("export", "Read a book aloud into audio files, for a phone or a podcast app.");
    command.parser.addPositionalArgument("book", "The book file");
    command.parser.addOption({"split", "One file per chapter in a folder named after the book"});
    command.parser.addOption({"format", "mp3 (the default), m4b, or omabook for the omareader phone app", "format", "mp3"});
    addVoiceOptions(command.parser);
    command.parser.addOption({"output", "The folder to write to (the current one by default)", "folder"});
    command.parser.addOption({"chapters", "Only these chapters, such as 3 or 2-5", "range"});
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const QStringList positional = command.parser.positionalArguments();
    if (positional.size() != 1)
        return fail("Use: omareader export <book> [--split] [--format mp3|m4b|omabook] [--engine kokoro|qwen] [--voice ID] "
                    "[--language L] [--speed X] [--tune key=value] [--output DIR] [--chapters N-M] [--json]");

    // The window's engine, voice and tuning unless the command names others.
    VoiceSettings voice;
    if (!voiceFromOptions(command.parser, &voice))
        return 1;
    Exporter::Options options;
    options.synthesis = Synthesis::optionsFor(voice);
    const QString format = command.parser.value("format").toLower();
    if (format != "mp3" && format != "m4b" && format != "omabook")
        return fail("The format is mp3, m4b or omabook.");
    options.format = format == "m4b" ? Exporter::M4b : format == "omabook" ? Exporter::Phone : Exporter::Mp3;
    options.layout = command.parser.isSet("split") ? Exporter::ChapterFiles : Exporter::SingleFile;
    options.folder = QDir(command.parser.isSet("output") ? command.parser.value("output") : QDir::currentPath())
                         .absolutePath();

    const auto book = loadBook(positional.first());
    if (!book)
        return 1;
    if (command.parser.isSet("chapters")
        && !parseChapters(command.parser.value("chapters"), int(book->chapters.size()), &options.firstChapter,
                          &options.lastChapter))
        return fail(QString("The book has chapters 1 to %1.").arg(book->chapters.size()));

    Exporter exporter;
    QEventLoop loop;
    bool ok = false;
    QString outcome;
    int reported = -1;
    QObject::connect(&exporter, &Exporter::progressChanged, &exporter, [&] {
        if (!exporter.running() || exporter.chapter() < 1 || exporter.chapter() == reported
            || exporter.chapter() > exporter.chapterCount())
            return;
        reported = exporter.chapter();
        err() << QString("[%1/%2] %3").arg(reported).arg(exporter.chapterCount()).arg(exporter.chapterTitle())
              << Qt::endl;
    });
    QObject::connect(&exporter, &Exporter::finished, &loop, [&](bool success, const QString &pathOrError) {
        ok = success;
        outcome = pathOrError;
        loop.quit();
    });
    // Ctrl+C cancels, so the half-built export is cleaned up rather than left
    // in a hidden folder beside the destination.
    interrupted = 0;
    auto onSignal = [](int) { interrupted = 1; };
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, &exporter, [&] {
        if (interrupted)
            exporter.cancel();
    });
    watch.start(200);
    if (exporter.start(book, options))
        loop.exec();
    else
        outcome = exporter.error();
    if (!ok)
        return fail(outcome);
    if (command.parser.isSet("json")) {
        printJson({{"path", outcome},
                   {"layout", options.layout == Exporter::ChapterFiles ? "chapters" : "single"},
                   {"format", format},
                   {"engine", voice.engine},
                   {"voice", voice.voice},
                   {"language", voice.language},
                   {"speed", voice.speed},
                   {"tuning", QJsonObject::fromVariantMap(voice.tuning)}});
        return 0;
    }
    out() << outcome << Qt::endl;
    return 0;
}

int doctor(const QStringList &arguments) {
    Command command("doctor", "Check that read-aloud is ready, for Kokoro and Qwen3-TTS.");
    command.parser.addOption({"engine", "The engine whose readiness sets the exit code: kokoro or qwen (the window's "
                                        "choice by default)",
                              "engine"});
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    bool engineOk = false;
    const QString engine = engineOption(command.parser, &engineOk);
    if (!engineOk)
        return 1;
    QSettings settings;
    const KokoroSetup setup = KokoroSetup::find(settings.value("voice/koko").toString(),
                                                settings.value("voice/models").toString());
    const VoiceSettings qwenVoice = VoiceSettings::load(Engine::Qwen);
    const QString modelSize = qwenVoice.text("model");
    const QwenSetup qwen = QwenSetup::find(modelSize, qwenVoice.text("device"));
    const QwenHardware hardware = QwenHardware::probe();
    const bool ready = engine == Engine::Qwen ? qwen.ready() : setup.ready();
    // ffmpeg only matters for exports, so it never makes read-aloud unready.
    const QString ffmpeg = Exporter::findFfmpeg();
    if (command.parser.isSet("json")) {
        // Kokoro's fields stay at the top level, as before Qwen3-TTS arrived.
        QJsonObject result = setup.toJson();
        result.insert("ffmpeg", ffmpeg);
        result.insert("engine", engine);
        result.insert("engineReady", ready);
        QJsonObject qwenJson = qwen.toJson();
        qwenJson.insert("hardware", hardware.toJson(modelSize));
        qwenJson.insert("problem", qwen.problem());
        result.insert("qwen", qwenJson);
        printJson(result);
        return ready ? 0 : 1;
    }
    out() << "Kokoro" << (engine == Engine::Kokoro ? " (selected)" : "") << Qt::endl;
    out() << "  koko:    " << (setup.binary.isEmpty() ? "missing" : setup.binary) << Qt::endl;
    out() << "  model:   " << (setup.model.isEmpty() ? "missing" : setup.model) << Qt::endl;
    out() << "  voices:  " << (setup.voices.isEmpty() ? "missing" : setup.voices) << Qt::endl;
    if (!setup.ready())
        out() << "  searched: " << setup.searchedModelDirectories.join(", ") << Qt::endl;
    out() << "  status:  " << (setup.ready() ? "ready" : setup.problem()) << Qt::endl;
    out() << Qt::endl;
    out() << "Qwen3-TTS" << (engine == Engine::Qwen ? " (selected)" : "") << ", " << modelSize << " model, device "
          << qwen.device << Qt::endl;
    out() << "  python:  " << (qwen.python.isEmpty() ? "missing" : qwen.python) << Qt::endl;
    out() << "  model:   " << (qwen.model.isEmpty() ? "missing" : qwen.model) << Qt::endl;
    out() << "  aligner: " << (qwen.aligner.isEmpty() ? "missing" : qwen.aligner) << Qt::endl;
    out() << "  gpu:     "
          << (hardware.nvidia ? QString("%1, %2 MiB free of %3 MiB")
                                    .arg(hardware.gpu)
                                    .arg(hardware.freeMiB)
                                    .arg(hardware.totalMiB)
                              : QString("no NVIDIA GPU found"))
          << Qt::endl;
    out() << "  needs:   about " << QwenHardware::neededMiB(modelSize)
          << " MiB of free GPU memory to read along in real time, or any CPU for slow exports" << Qt::endl;
    const QString advice = hardware.advice(modelSize);
    if (!advice.isEmpty())
        out() << "  note:    " << advice << Qt::endl;
    out() << "  status:  " << (qwen.ready() ? "ready" : qwen.problem()) << Qt::endl;
    out() << Qt::endl;
    out() << "ffmpeg: " << (ffmpeg.isEmpty() ? "missing (needed to export audio)" : ffmpeg) << Qt::endl;
    if (!ready)
        return fail((engine == Engine::Qwen ? qwen.problem() : setup.problem()));
    out() << "Read-aloud with " << Engine::displayName(engine) << " is ready." << Qt::endl;
    return 0;
}

int runSetupScript(const QString &resource, const QStringList &scriptArguments) {
    QFile script(resource);
    if (!script.open(QIODevice::ReadOnly))
        return fail("The setup script is missing from this build.");
    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.start("sh", QStringList{"-s", "--"} + scriptArguments);
    if (!process.waitForStarted())
        return fail("Could not run sh.");
    process.write(script.readAll());
    process.closeWriteChannel();
    process.waitForFinished(-1);
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : 1;
}

int setup(const QStringList &arguments) {
    Command command("setup", "Install a voice engine and its models (the one step that uses the network).");
    command.parser.addOption({"engine", "kokoro (the default) or qwen", "engine", "kokoro"});
    command.parser.addOption({"model", "Qwen3-TTS only: 0.6b (the default) or 1.7b, which needs about 7 GB of GPU "
                                       "memory",
                              "size", "0.6b"});
    command.parser.addOption({"dry-run", "Show what would be done without changing anything"});
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const QString engine = command.parser.value("engine").toLower();
    if (!Engine::isKnown(engine))
        return fail("The engine is kokoro or qwen.");
    QStringList scriptArguments;
    if (command.parser.isSet("dry-run"))
        scriptArguments << "--dry-run";
    if (engine == Engine::Kokoro) {
        if (command.parser.isSet("model"))
            return fail("--model only applies to --engine qwen.");
        return runSetupScript(":/setup-kokoros.sh", scriptArguments);
    }
    const QString model = command.parser.value("model").toLower();
    if (model != "0.6b" && model != "1.7b")
        return fail("The Qwen3-TTS model is 0.6b or 1.7b.");
    return runSetupScript(":/setup-qwen.sh", scriptArguments << "--model" << model);
}

int voices(const QStringList &arguments) {
    Command command("voices", "List an engine's voices, languages and tuning settings.");
    command.parser.addOption({"engine", "kokoro or qwen (the window's choice by default)", "engine"});
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    bool engineOk = false;
    const QString engine = engineOption(command.parser, &engineOk);
    if (!engineOk)
        return 1;
    const VoiceSettings current = VoiceSettings::load(engine);
    if (command.parser.isSet("json")) {
        QJsonArray voiceList, languageList, tuningList;
        for (const EngineVoice &voice : EngineCatalog::voices(engine))
            voiceList.append(QJsonObject{{"id", voice.id}, {"name", voice.name}, {"language", voice.language},
                                         {"description", voice.description}});
        for (const EngineLanguage &language : EngineCatalog::languages(engine))
            languageList.append(QJsonObject{{"id", language.id}, {"name", language.name}});
        for (const TuningParameter &p : Tuning::parameters(engine)) {
            QJsonObject item{{"key", p.key}, {"label", p.label}, {"description", p.description},
                             {"recommended", QJsonValue::fromVariant(p.recommended)},
                             {"current", QJsonValue::fromVariant(current.tuning.value(p.key))}};
            if (p.kind == TuningParameter::Number) {
                item.insert("minimum", p.minimum);
                item.insert("maximum", p.maximum);
            } else if (p.kind == TuningParameter::Choice) {
                item.insert("choices", QJsonArray::fromStringList(p.choices));
            }
            tuningList.append(item);
        }
        printJson({{"engine", engine}, {"current", QJsonObject::fromVariantMap(current.toMap())}, {"voices", voiceList},
                   {"languages", languageList}, {"tuning", tuningList}});
        return 0;
    }
    out() << Engine::displayName(engine) << " voices:" << Qt::endl;
    for (const EngineVoice &voice : EngineCatalog::voices(engine))
        out() << "  " << voice.id.leftJustified(12) << " " << voice.name
              << (voice.description.isEmpty() ? QString() : ": " + voice.description)
              << (voice.id == current.voice ? "  (current)" : "") << Qt::endl;
    if (engine == Engine::Qwen) {
        out() << Qt::endl << "Languages (--language):" << Qt::endl;
        for (const EngineLanguage &language : EngineCatalog::languages(engine))
            out() << "  " << language.id << (language.id == current.language ? "  (current)" : "") << Qt::endl;
    }
    out() << Qt::endl << "Tuning (--tune key=value):" << Qt::endl;
    for (const TuningParameter &p : Tuning::parameters(engine)) {
        QString range;
        if (p.kind == TuningParameter::Number)
            range = QString("%1 to %2%3").arg(p.minimum).arg(p.maximum).arg(p.unit.isEmpty() ? QString() : " " + p.unit);
        else if (p.kind == TuningParameter::Choice)
            range = p.choices.join(" | ").replace(QRegularExpression("^ \\| "), "(none) | ");
        else
            range = "text";
        out() << "  " << p.key.leftJustified(18) << " " << range << ", now " << current.tuning.value(p.key).toString()
              << Qt::endl
              << "  " << QString(18, ' ') << " " << p.description << Qt::endl;
    }
    return 0;
}

QJsonObject syncStatus(const SyncService &sync) {
    return QJsonObject{{"available", sync.available()},
                       {"connected", sync.connected()},
                       {"provider", sync.provider()},
                       {"account", sync.account()},
                       {"lastSynced", sync.lastSynced().isValid() ? sync.lastSynced().toString(Qt::ISODate) : QString()},
                       {"pendingUploads", sync.pendingUploads()},
                       {"error", sync.error()}};
}

int syncCommand(const QStringList &arguments) {
    Command command("sync", "Sync through Dropbox or Google Drive: books for the phone, reading places, removals "
                            "and settings.");
    command.parser.addPositionalArgument("action",
                                         "now (the default), status, connect [dropbox|drive], disconnect, or send "
                                         "<bundle.omabook>",
                                         "[action]");
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const QStringList positional = command.parser.positionalArguments();
    const QString action = positional.value(0, "now");
    const bool json = command.parser.isSet("json");

    Library library;
    KeyringTokenStore keyring;
    SyncService sync(&library, &keyring);
    sync.setSynchronous(true);
    if (!sync.available())
        return fail("This build of omareader has no sync: it was built without a Dropbox or Google key.");
    auto report = [&](int status) {
        if (json) {
            printJson(syncStatus(sync));
        } else if (status == 0) {
            out() << (sync.connected() ? "Connected to " + sync.providerName()
                                             + (sync.account().isEmpty() ? QString() : " as " + sync.account())
                                       : QString("Not connected"))
                  << Qt::endl;
            if (sync.lastSynced().isValid())
                out() << "Last synced " << sync.lastSynced().toString(Qt::ISODate) << Qt::endl;
            if (sync.pendingUploads() > 0)
                out() << sync.pendingUploads() << " book(s) waiting to go to the phone" << Qt::endl;
        }
        if (!sync.error().isEmpty())
            err() << "omareader: " << sync.error() << Qt::endl;
        return status;
    };

    if (action == "status")
        return report(0);
    if (action == "disconnect") {
        sync.disconnectSync();
        return report(0);
    }
    if (action == "connect") {
        QEventLoop loop;
        QObject::connect(&sync, &SyncService::stateChanged, &loop, [&] {
            if (!sync.connecting())
                loop.quit();
        });
        // With both providers built in, the user says which.
        QString provider = positional.value(1);
        if (provider.isEmpty())
            provider = sync.dropboxAvailable() && !sync.driveAvailable() ? SyncService::Dropbox
                       : sync.driveAvailable() && !sync.dropboxAvailable() ? SyncService::Drive
                                                                            : QString();
        if (provider.isEmpty())
            return fail("Say which: omareader sync connect dropbox, or omareader sync connect drive.");
        if (provider != SyncService::Dropbox && provider != SyncService::Drive)
            return fail(provider + " isn't a provider omareader knows. Use dropbox or drive.");
        if (!sync.connectProvider(provider)) {
            if (sync.error().isEmpty())
                return fail("This build of omareader has no " + SyncService::nameOf(provider) + " sync.");
            return report(1);
        }
        err() << "Open this page to connect " << SyncService::nameOf(provider)
              << " (it should have opened already):\n"
              << sync.authorizeUrl() << Qt::endl;
        loop.exec();
        return report(sync.connected() && sync.error().isEmpty() ? 0 : 1);
    }
    if (!sync.connected())
        return fail("Sync isn't connected. Run omareader sync connect first.");
    if (action == "send") {
        const QString path = positional.value(1);
        const QJsonObject manifest = SyncFormat::bundleManifest(path);
        if (manifest.isEmpty())
            return fail(path.isEmpty() ? "Name the .omabook to send." : path + " isn't an omareader bundle.");
        sync.queueUpload(path, manifest.value("id").toString(), manifest.value("title").toString(),
                         manifest.value("author").toString());
        return report(sync.error().isEmpty() && sync.pendingUploads() == 0 ? 0 : 1);
    }
    if (action != "now")
        return fail("Use omareader sync [now|status|connect|disconnect|send <bundle>].");
    sync.syncNow();
    return report(sync.error().isEmpty() ? 0 : 1);
}

int skill(const QStringList &arguments) {
    Command command("skill", "Print the agent skill, or install it with: skill install.");
    command.parser.addPositionalArgument("install", "Copy to ~/.agents/skills/omareader and link into ~/.claude/skills",
                                         "[install]");
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    QFile bundled(":/skill.md");
    if (!bundled.open(QIODevice::ReadOnly))
        return fail("The skill is missing from this build.");
    const QByteArray contents = bundled.readAll();
    const QStringList positional = command.parser.positionalArguments();
    if (positional.isEmpty()) {
        out() << QString::fromUtf8(contents);
        out().flush();
        return 0;
    }
    if (positional != QStringList{"install"})
        return fail("Use omareader skill to print the skill, or omareader skill install.");

    const QDir home = QDir::home();
    const QString directory = home.filePath(".agents/skills/omareader");
    if (!QDir().mkpath(directory))
        return fail("Could not create " + directory);
    QFile target(directory + "/SKILL.md");
    if (!target.open(QIODevice::WriteOnly) || target.write(contents) != contents.size())
        return fail("Could not write " + target.fileName());
    target.close();
    const QString link = home.filePath(".claude/skills/omareader");
    if (!QFileInfo::exists(link) && !QFileInfo(link).isSymLink()) {
        if (!home.mkpath(".claude/skills") || !QFile::link("../../.agents/skills/omareader", link))
            return fail("Could not link " + link);
    }
    out() << "Installed the omareader skill in " << directory << Qt::endl;
    return 0;
}

}

namespace Cli {

QString usage() {
    return "Usage: omareader [book]\n"
           "       omareader <command> [options]\n\n"
           "Commands:\n"
           "  open [book]                     Open the window (the default)\n"
           "  info <book> [--json]            Show a book's metadata and chapters\n"
           "  text <book> [--chapter N]       Print a book's text as omareader reads it\n"
           "  export <book> [--split]         Read a book aloud into MP3 or M4B files\n"
           "  doctor [--engine E] [--json]    Check that read-aloud is ready, with hardware advice\n"
           "  setup [--engine kokoro|qwen]    Install a voice engine and its models (uses the network)\n"
           "  voices [--engine E] [--json]    List an engine's voices, languages and tuning\n"
           "  sync [now|status|connect|...]   Sync through Dropbox or Google Drive (uses the network once connected)\n"
           "  skill [install]                 Print the skill for coding agents, or install it\n"
           "  version                         Print the version\n";
}

bool isCommand(const QString &word) {
    return QStringList{"info", "text", "export", "doctor", "setup", "voices", "sync", "skill", "help", "--help", "-h", "version", "--version"}
        .contains(word);
}

int run(const QStringList &arguments) {
    const QString command = arguments.value(1);
    const QStringList rest = QStringList{arguments.value(0) + " " + command} + arguments.mid(2);
    if (command == "info") return info(rest);
    if (command == "text") return text(rest);
    if (command == "export") return exportAudio(rest);
    if (command == "doctor") return doctor(rest);
    if (command == "setup") return setup(rest);
    if (command == "voices") return voices(rest);
    if (command == "skill") return skill(rest);
    if (command == "sync") return syncCommand(rest);
    if (command == "version" || command == "--version") {
        out() << "omareader " << QCoreApplication::applicationVersion() << Qt::endl;
        return 0;
    }
    out() << usage();
    out().flush();
    return 0;
}

}
