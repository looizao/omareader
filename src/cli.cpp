#include "cli.h"

#include "annotations.h"
#include "bookloader.h"
#include "dictionary.h"
#include "exporter.h"
#include "kokoro.h"
#include "language.h"
#include "library.h"
#include "pronunciation.h"
#include "qwen.h"
#include "rsvp.h"
#include "synthesizer.h"
#include "voicesettings.h"
#include "webimport.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>
#include <QTimer>
#include <csignal>
#include <cstdio>
#include <unistd.h>

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
    // Recognising a scanned PDF takes a while, so say how far it has got:
    // in place on a terminal, every tenth of the way otherwise. Ctrl+C
    // stops it, and the pages done are kept for the next run.
    QMutex mutex;
    int shown = -1;
    bool lineOpen = false;
    const bool terminal = isatty(fileno(stderr));
    PdfOcr::Control control;
    control.progress = [&](int done, int total) {
        QMutexLocker lock(&mutex);
        const int step = terminal ? done : done * 10 / qMax(1, total);
        if (step == shown)
            return;
        if (shown < 0)
            err() << "Recognising the text of this scanned PDF with Tesseract; it is kept, so this runs once."
                  << Qt::endl;
        shown = step;
        const QString line = QString("Page %1 of %2").arg(done).arg(total);
        if (terminal) {
            err() << '\r' << line << (done == total ? "\n" : "");
            lineOpen = done < total;
        } else
            err() << line << '\n';
        err().flush();
    };
    auto book = BookLoader::load(path, &error, control);
    if (lineOpen)
        err() << Qt::endl;
    if (!book)
        fail(error);
    return book;
}

void addVoiceOptions(QCommandLineParser &parser) {
    parser.addOption({"engine", "kokoro or qwen (the window's choice by default)", "engine"});
    parser.addOption({"voice", "One of the engine's voices, such as af_heart or ryan (see omareader voices)", "voice"});
    parser.addOption({"language", "Qwen3-TTS only: the language to read in, such as English or Portuguese", "language"});
    parser.addOption({"speed", "Speaking speed from 0.5 to 2.0", "speed"});
    parser.addOption({"dialogue-voice", "Another of the engine's voices for what the characters say, in quotation "
                                        "marks or after a dialogue dash; off for the narration's voice",
                      "voice"});
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
    if (parser.isSet("dialogue-voice"))
        overrides.append({"dialogueVoice", parser.value("dialogue-voice")});
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
    Command command("info", "Show a book's metadata, language and chapters.");
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
                   {"source", book->source},
                   {"language", book->language},
                   {"languageSource", book->languageSource},
                   {"hasCover", !book->cover.isEmpty()},
                   {"recognised", book->recognised},
                   {"sentences", book->sentenceCount()},
                   {"words", book->wordCount()},
                   {"chapters", chapters}});
        return 0;
    }
    out() << book->title << Qt::endl;
    if (!book->author.isEmpty())
        out() << book->author << Qt::endl;
    if (!book->source.isEmpty())
        out() << book->source << Qt::endl;
    out() << book->format.toUpper() << ", " << book->wordCount() << " words"
          << (book->recognised ? ", text recognised from scanned pages (OCR)" : "") << Qt::endl;
    if (!book->language.isEmpty())
        out() << "Language: " << Language::name(book->language) << " ("
              << (book->languageSource == "metadata" ? "from the book's metadata" : "guessed from the text") << ")"
              << Qt::endl;
    out() << Qt::endl;
    for (const auto &value : std::as_const(chapters)) {
        const QJsonObject c = value.toObject();
        out() << QString("%1. %2 (%3 words)").arg(c.value("number").toInt(), 3).arg(c.value("title").toString())
                     .arg(c.value("words").toInt())
              << Qt::endl;
    }
    return 0;
}

int notes(const QStringList &arguments) {
    Command command("notes", "List a book's bookmarks, highlights and notes, as the window keeps them.");
    command.parser.addPositionalArgument("book", "The book file");
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const QStringList positional = command.parser.positionalArguments();
    if (positional.size() != 1)
        return fail("Use: omareader notes <book> [--json]");
    const auto book = loadBook(positional.first());
    if (!book)
        return 1;

    // Read as the window would find them, the sync folder's included, but
    // nothing is written.
    QList<Annotation> items = Annotations::stored(Annotations::fingerprint(book->path), {}, Annotations::savedSyncFolder());
    Annotations::place(items, *book);
    items.removeIf([](const Annotation &a) { return a.deleted; });
    std::stable_sort(items.begin(), items.end(), [](const Annotation &a, const Annotation &b) {
        return std::tie(a.chapter, a.paragraph, a.start) < std::tie(b.chapter, b.paragraph, b.start);
    });

    if (command.parser.isSet("json")) {
        QJsonArray list;
        for (const Annotation &a : std::as_const(items)) {
            QJsonObject o{{"kind", a.kind},
                          {"chapter", a.chapter + 1},
                          {"chapterTitle", a.chapter < book->chapters.size() ? book->chapters.at(a.chapter).title : QString()},
                          {"text", a.text},
                          {"placed", a.placed},
                          {"created", a.created.toString(Qt::ISODateWithMs)},
                          {"modified", a.modified.toString(Qt::ISODateWithMs)}};
            if (!a.note.isEmpty())
                o.insert("note", a.note);
            list.append(o);
        }
        printJson({{"title", book->title}, {"author", book->author}, {"annotations", list}});
        return 0;
    }
    out() << book->title << Qt::endl;
    if (items.isEmpty()) {
        out() << "No bookmarks or highlights." << Qt::endl;
        return 0;
    }
    int chapter = -1;
    for (const Annotation &a : std::as_const(items)) {
        if (a.chapter != chapter) {
            chapter = a.chapter;
            const QString title = chapter < book->chapters.size() ? book->chapters.at(chapter).title : QString();
            out() << Qt::endl << QString("Chapter %1%2").arg(chapter + 1).arg(title.isEmpty() ? QString() : " · " + title) << Qt::endl;
        }
        const QString text = QString(a.text).replace('\n', ' ');
        out() << (a.isHighlight() ? "  Highlight: " : "  Bookmark: ") << text << (a.placed ? "" : " (not found in this copy)")
              << Qt::endl;
        if (!a.note.isEmpty())
            out() << "    Note: " << QString(a.note).replace('\n', "\n          ") << Qt::endl;
    }
    return 0;
}

int text(const QStringList &arguments) {
    Command command("text", "Print a book's text as omareader reads it.");
    command.parser.addPositionalArgument("book", "The book file");
    command.parser.addOption({"chapter", "Only this chapter, counting from 1", "number"});
    command.parser.addOption({"spoken", "The text as the voice reads it: without footnotes, citation markers, "
                                        "links and other clutter, with your pronunciations respelled, one sentence "
                                        "per line"});
    command.parser.addOption({"dialogue", "With --spoken: mark what a dialogue voice would read, between « and »"});
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const QStringList positional = command.parser.positionalArguments();
    if (positional.size() != 1)
        return fail("Use: omareader text <book> [--chapter N] [--spoken [--dialogue]] [--json]");
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
    if (command.parser.isSet("spoken")) {
        const int first = book->chapterFirstSentence.at(from);
        const int last = to < book->chapters.size() ? book->chapterFirstSentence.at(to) : book->sentenceCount();
        const Lexicon lexicon = Lexicon::load();
        // Any second voice marks the dialogue; which one doesn't change where it is.
        SynthesisOptions options;
        options.lexicon = lexicon;
        if (command.parser.isSet("dialogue")) {
            options.voice = VoiceSettings::recommended(Engine::Kokoro);
            options.voice.dialogueVoice = options.voice.voice == "am_adam" ? "af_heart" : "am_adam";
        }
        QList<int> speakable;
        for (int s = first; s < last; ++s)
            if (book->sentences.at(s).speakable())
                speakable.append(s);
        const QList<Synthesizer::Line> lines = Synthesis::lines(*book, speakable, options);
        QJsonArray sentences;
        int paragraph = -1, chapter = from;
        for (const Synthesizer::Line &line : lines) {
            const Sentence &sentence = book->sentences.at(line.id);
            QString text = line.text;
            QJsonArray pieces;
            if (!line.pieces.isEmpty()) {
                QStringList marked;
                for (const auto &piece : line.pieces) {
                    marked.append(piece.dialogue ? "«" + piece.text + "»" : piece.text);
                    pieces.append(QJsonObject{{"text", piece.text}, {"dialogue", piece.dialogue}});
                }
                text = marked.join(' ');
            }
            if (command.parser.isSet("json")) {
                QJsonObject entry{{"chapter", sentence.chapter + 1}, {"text", line.text}};
                if (command.parser.isSet("dialogue"))
                    entry.insert("pieces", pieces.isEmpty() ? QJsonArray{QJsonObject{{"text", line.text},
                                                                                       {"dialogue", false}}}
                                                            : pieces);
                sentences.append(entry);
                continue;
            }
            // A blank line between paragraphs, and two between chapters.
            if (paragraph >= 0 && (sentence.chapter != chapter || sentence.paragraph != paragraph))
                out() << Qt::endl << (sentence.chapter != chapter ? "\n" : "");
            chapter = sentence.chapter;
            paragraph = sentence.paragraph;
            out() << text << Qt::endl;
        }
        if (command.parser.isSet("json"))
            printJson({{"title", book->title}, {"author", book->author}, {"sentences", sentences}});
        return 0;
    }
    if (command.parser.isSet("json")) {
        QJsonArray chapters;
        for (int c = from; c < to; ++c) {
            QJsonArray paragraphs;
            for (const auto &p : book->chapters.at(c).paragraphs) {
                QJsonObject paragraph{{"text", p.text}, {"heading", p.level}};
                if (p.note)
                    paragraph.insert("note", true);
                paragraphs.append(paragraph);
            }
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

int rsvp(const QStringList &arguments) {
    Command command("rsvp", "Print the flashes RSVP shows a book in, one per line: how long each stays up, in "
                            "milliseconds, and its text with the recognition point in brackets.");
    command.parser.addPositionalArgument("book", "The book file");
    command.parser.addOption({"chapter", "Only this chapter, counting from 1", "number"});
    command.parser.addOption({"wpm", QString("Words a minute, from %1 to %2 (the window's by default)")
                                         .arg(Rsvp::MinWpm).arg(Rsvp::MaxWpm),
                              "number"});
    command.parser.addOption({"size", QString("Words in a flash, at most, from 1 to %1 (the window's by default)")
                                          .arg(Rsvp::MaxSize),
                              "number"});
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const QStringList positional = command.parser.positionalArguments();
    if (positional.size() != 1)
        return fail("Use: omareader rsvp <book> [--chapter N] [--wpm N] [--size N] [--json]");

    const QSettings settings;
    bool ok = true;
    const int wpm = command.parser.isSet("wpm") ? command.parser.value("wpm").toInt(&ok)
                                                : qBound(Rsvp::MinWpm, settings.value("rsvp/wpm", Rsvp::DefaultWpm).toInt(), Rsvp::MaxWpm);
    if (!ok || wpm < Rsvp::MinWpm || wpm > Rsvp::MaxWpm)
        return fail(QString("The rate is %1 to %2 words a minute.").arg(Rsvp::MinWpm).arg(Rsvp::MaxWpm));
    const int size = command.parser.isSet("size") ? command.parser.value("size").toInt(&ok)
                                                  : qBound(1, settings.value("rsvp/size", 1).toInt(), Rsvp::MaxSize);
    if (!ok || size < 1 || size > Rsvp::MaxSize)
        return fail(QString("A flash holds 1 to %1 words.").arg(Rsvp::MaxSize));

    const auto book = loadBook(positional.first());
    if (!book)
        return 1;
    int chapter = -1;
    if (command.parser.isSet("chapter")) {
        chapter = command.parser.value("chapter").toInt(&ok) - 1;
        if (!ok || chapter < 0 || chapter >= book->chapters.size())
            return fail(QString("The book has chapters 1 to %1.").arg(book->chapters.size()));
    }

    // Timed over the whole book, so a chapter reads as it does in the window.
    const QList<Flash> flashes = Flashes::of(*book, size);
    const double unit = Flashes::unitMs(flashes, wpm);
    const bool json = command.parser.isSet("json");
    QJsonArray list;
    int shown = 0;
    for (const Flash &flash : flashes) {
        const Sentence &sentence = book->sentences.at(flash.sentence);
        if (chapter >= 0 && sentence.chapter != chapter)
            continue;
        const QString text = Flashes::text(*book, flash);
        const int point = Flashes::recognitionPoint(text);
        // The clock starts at the first one shown, slowly.
        const int ms = Flashes::durationMs(flash, unit, shown++);
        if (json) {
            list.append(QJsonObject{{"text", text},
                                    {"recognition", point},
                                    {"ms", ms},
                                    {"chapter", sentence.chapter + 1},
                                    {"sentence", flash.sentence}});
            continue;
        }
        QString marked = text;
        if (point >= 0) {
            const int length = text.at(point).isHighSurrogate() ? 2 : 1;
            marked = text.left(point) + '[' + text.mid(point, length) + ']' + text.mid(point + length);
        }
        out() << ms << "  " << marked << '\n';
    }
    if (json)
        printJson({{"title", book->title}, {"wpm", wpm}, {"size", size}, {"flashes", list}});
    out().flush();
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
    command.parser.addOption({"format", "mp3 (the default) or m4b", "format", "mp3"});
    addVoiceOptions(command.parser);
    command.parser.addOption({"output", "The folder to write to (the current one by default)", "folder"});
    command.parser.addOption({"chapters", "Only these chapters, such as 3 or 2-5", "range"});
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const QStringList positional = command.parser.positionalArguments();
    if (positional.size() != 1)
        return fail("Use: omareader export <book> [--split] [--format mp3|m4b] [--engine kokoro|qwen] [--voice ID] "
                    "[--language L] [--speed X] [--tune key=value] [--output DIR] [--chapters N-M] [--json]");

    // The window's engine, voice and tuning unless the command names others.
    VoiceSettings voice;
    if (!voiceFromOptions(command.parser, &voice))
        return 1;
    Exporter::Options options;
    options.synthesis = Synthesis::optionsFor(voice);
    const QString format = command.parser.value("format").toLower();
    if (format != "mp3" && format != "m4b")
        return fail("The format is mp3 or m4b.");
    options.format = format == "m4b" ? Exporter::M4b : Exporter::Mp3;
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
    Command command("setup", "Install a voice engine and its models, or a dictionary (the steps that use the "
                             "network).");
    command.parser.addOption({"engine", "kokoro (the default) or qwen", "engine", "kokoro"});
    command.parser.addOption({"model", "Qwen3-TTS only: 0.6b (the default) or 1.7b, which needs about 7 GB of GPU "
                                       "memory",
                              "size", "0.6b"});
    command.parser.addOption({"dictionary", "Install one of FreeDict's dictionaries instead, such as eng-por for "
                                            "English to Portuguese (see omareader dictionaries --available)",
                              "name"});
    command.parser.addOption({"dry-run", "Show what would be done without changing anything"});
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    QStringList scriptArguments;
    if (command.parser.isSet("dry-run"))
        scriptArguments << "--dry-run";
    if (command.parser.isSet("dictionary")) {
        if (command.parser.isSet("engine") || command.parser.isSet("model"))
            return fail("--dictionary installs a dictionary, not a voice engine; run them separately.");
        const QString name = command.parser.value("dictionary");
        const CatalogDictionary dictionary = DictionaryCatalog::find(name);
        if (dictionary.name.isEmpty())
            return fail(QString("FreeDict has no dictionary %1. omareader dictionaries --available lists them.")
                            .arg(name));
        return runSetupScript(":/setup-dictionary.sh",
                              scriptArguments << dictionary.name << dictionary.url << dictionary.sha512);
    }
    const QString engine = command.parser.value("engine").toLower();
    if (!Engine::isKnown(engine))
        return fail("The engine is kokoro or qwen.");
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

QString noDictionaries() {
    return "No dictionaries are installed. Install one with omareader setup --dictionary, such as eng-por for "
           "English to Portuguese; omareader dictionaries --available lists them all.";
}

int pronounce(const QStringList &arguments) {
    Command command("pronounce", "List, add or remove how names and terms are said when reading aloud.");
    command.parser.addPositionalArgument("written", "The word or phrase as books write it", "[written");
    command.parser.addPositionalArgument("spoken", "How it sounds, spelled out, such as Her-my-oh-nee", "spoken]");
    command.parser.addOption({"remove", "Forget how this word or phrase is said", "written"});
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const QStringList positional = command.parser.positionalArguments();
    QString error;
    if (command.parser.isSet("remove")) {
        if (!positional.isEmpty())
            return fail("Use: omareader pronounce --remove <written>");
        const QString written = command.parser.value("remove");
        bool missing = false;
        if (!Lexicon::update({}, [&](Lexicon &lexicon) { return lexicon.remove(written) || (missing = true, false); },
                             nullptr, &error))
            return fail(missing ? QString("No pronunciation is saved for \"%1\".").arg(written) : error);
    } else if (positional.size() == 2) {
        if (!Lexicon::valid(positional.at(0), positional.at(1)))
            return fail("Both the written form and how it sounds need a letter or a digit, and the written form "
                        "at most six words.");
        if (!Lexicon::update({}, [&](Lexicon &lexicon) { return lexicon.set(positional.at(0), positional.at(1)); },
                             nullptr, &error))
            return fail(error);
    } else if (!positional.isEmpty()) {
        return fail("Use: omareader pronounce [<written> <spoken>] [--remove <written>] [--json]");
    }

    Pronunciations list(Lexicon::defaultFile());
    if (command.parser.isSet("json")) {
        QJsonArray entries;
        for (const QVariant &entry : list.entries())
            entries.append(QJsonObject::fromVariantMap(entry.toMap()));
        printJson({{"file", Lexicon::defaultFile()}, {"entries", entries}});
        return 0;
    }
    for (const QVariant &entry : list.entries()) {
        const QVariantMap map = entry.toMap();
        out() << map.value("written").toString() << "\t" << map.value("spoken").toString() << Qt::endl;
    }
    return 0;
}

int lookupWord(const QStringList &arguments) {
    Command command("lookup", "Look a word up in the installed dictionaries, offline.");
    command.parser.addPositionalArgument("word", "The word, or a phrase such as \"a lot of\"");
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const QString word = command.parser.positionalArguments().join(' ').trimmed();
    if (word.isEmpty())
        return fail("Use: omareader lookup <word> [--json]");
    DictionaryShelf shelf(DictionaryShelf::defaultDirectories());
    const DictionaryShelf::Result result = shelf.lookup(word);
    if (result.dictionaries == 0)
        return fail(noDictionaries());
    if (command.parser.isSet("json")) {
        QJsonArray entries;
        for (const DictionaryEntry &entry : result.entries)
            entries.append(QJsonObject{{"dictionary", entry.dictionary}, {"headword", entry.headword},
                                       {"text", entry.text}});
        printJson({{"word", word}, {"dictionaries", result.dictionaries}, {"entries", entries}});
        return result.entries.isEmpty() ? 1 : 0;
    }
    if (result.entries.isEmpty())
        return fail(QString("None of the %1 dictionaries has %2.").arg(result.dictionaries).arg(word));
    for (qsizetype i = 0; i < result.entries.size(); ++i) {
        const DictionaryEntry &entry = result.entries.at(i);
        if (i > 0)
            out() << Qt::endl;
        out() << entry.headword << " (" << entry.dictionary << ")" << Qt::endl;
        for (const QString &line : entry.text.split('\n'))
            out() << "  " << line << Qt::endl;
    }
    return 0;
}

int dictionaries(const QStringList &arguments) {
    Command command("dictionaries", "List the installed dictionaries, or those omareader setup --dictionary can "
                                    "install.");
    command.parser.addOption({"available", "List FreeDict's dictionaries that setup can install"});
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const bool json = command.parser.isSet("json");
    if (command.parser.isSet("available")) {
        const QString installed = DictionaryShelf::installDirectory();
        QJsonArray list;
        for (const CatalogDictionary &d : DictionaryCatalog::all()) {
            const bool here = QFileInfo::exists(installed + "/" + d.name + "/.sha512");
            if (json) {
                list.append(QJsonObject{{"name", d.name}, {"from", DictionaryCatalog::languageName(d.from())},
                                        {"to", DictionaryCatalog::languageName(d.to())}, {"version", d.version},
                                        {"headwords", d.headwords}, {"bytes", d.bytes}, {"installed", here}});
                continue;
            }
            out() << d.name << "  " << d.title().leftJustified(36) << " "
                  << QString("%1 words").arg(d.headwords).rightJustified(13) << "  "
                  << QString("%1 MB").arg(double(d.bytes) / 1e6, 0, 'f', 1).rightJustified(8)
                  << (here ? "  (installed)" : "") << Qt::endl;
        }
        if (json)
            printJson({{"dictionaries", list}});
        else
            out() << Qt::endl << "Install one with: omareader setup --dictionary <name>" << Qt::endl;
        return 0;
    }
    DictionaryShelf shelf(DictionaryShelf::defaultDirectories());
    const auto installed = shelf.dictionaries();
    if (json) {
        QJsonArray list;
        for (const auto &d : installed)
            list.append(QJsonObject{{"name", d->name()},
                                    {"format", d->format() == Dictionary::StarDict ? "stardict" : "dictd"},
                                    {"headwords", d->wordCount()},
                                    {"path", d->path()}});
        printJson({{"dictionaries", list}, {"searched", QJsonArray::fromStringList(shelf.directories())}});
        return 0;
    }
    if (installed.isEmpty()) {
        out() << noDictionaries() << Qt::endl
              << "omareader also reads StarDict and dictd dictionaries from: " << shelf.directories().join(", ")
              << Qt::endl;
        return 0;
    }
    for (const auto &d : installed)
        out() << d->name() << Qt::endl
              << "  " << (d->format() == Dictionary::StarDict ? "StarDict" : "dictd") << ", " << d->wordCount()
              << " headwords, " << d->path() << Qt::endl;
    return 0;
}

// Fetches a page and prints the article the window would offer to add,
// without adding it: the window owns the library while it is open.
int fetchPage(const QStringList &arguments) {
    Command command("fetch", "Fetch a web page and print its article as omareader would add it (uses the network).");
    command.parser.addPositionalArgument("url", "The page's address");
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const QStringList positional = command.parser.positionalArguments();
    if (positional.size() != 1)
        return fail("Use: omareader fetch <url> [--json]");
    WebImport page;
    QEventLoop loop;
    QObject::connect(&page, &WebImport::fetched, &loop, &QEventLoop::quit);
    QObject::connect(&page, &WebImport::failed, &loop, &QEventLoop::quit);
    page.fetch(positional.first());
    if (page.busy())
        loop.exec();
    if (!page.hasArticle())
        return fail(page.error());
    if (command.parser.isSet("json")) {
        printJson({{"title", page.title()},
                   {"author", page.author()},
                   {"site", page.site()},
                   {"source", page.source()},
                   {"text", page.text()}});
        return 0;
    }
    out() << page.title() << Qt::endl;
    if (!page.author().isEmpty())
        out() << page.author() << Qt::endl;
    out() << page.source() << Qt::endl << Qt::endl << page.text() << Qt::endl;
    return 0;
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
           "  info <book> [--json]            Show a book's metadata, language and chapters\n"
           "  text <book> [--chapter N]       Print a book's text as omareader shows it, or --spoken as heard\n"
           "  rsvp <book> [--wpm N]           Print the flashes RSVP shows, with how long each stays up\n"
           "  notes <book> [--json]           List a book's bookmarks, highlights and notes\n"
           "  export <book> [--split]         Read a book aloud into MP3 or M4B files\n"
           "  fetch <url> [--json]            Print the article a web page adds (uses the network)\n"
           "  doctor [--engine E] [--json]    Check that read-aloud is ready, with hardware advice\n"
           "  lookup <word> [--json]          Look a word up in the installed dictionaries\n"
           "  dictionaries [--available]      List the installed dictionaries, or those setup can add\n"
           "  pronounce [written spoken]      List or teach how names and terms are said (--remove W)\n"
           "  setup [--engine kokoro|qwen]    Install a voice engine and its models (uses the network)\n"
           "  setup --dictionary eng-por      Install a FreeDict dictionary (uses the network)\n"
           "  voices [--engine E] [--json]    List an engine's voices, languages and tuning\n"
           "  skill [install]                 Print the skill for coding agents, or install it\n"
           "  version                         Print the version\n";
}

bool isCommand(const QString &word) {
    return QStringList{"info", "text", "rsvp", "notes", "export", "fetch", "doctor", "setup", "voices", "lookup", "dictionaries", "pronounce", "skill", "help", "--help", "-h", "version", "--version"}
        .contains(word);
}

int run(const QStringList &arguments) {
    const QString command = arguments.value(1);
    const QStringList rest = QStringList{arguments.value(0) + " " + command} + arguments.mid(2);
    if (command == "info") return info(rest);
    if (command == "text") return text(rest);
    if (command == "rsvp") return rsvp(rest);
    if (command == "notes") return notes(rest);
    if (command == "export") return exportAudio(rest);
    if (command == "fetch") return fetchPage(rest);
    if (command == "doctor") return doctor(rest);
    if (command == "setup") return setup(rest);
    if (command == "voices") return voices(rest);
    if (command == "lookup") return lookupWord(rest);
    if (command == "dictionaries") return dictionaries(rest);
    if (command == "pronounce") return pronounce(rest);
    if (command == "skill") return skill(rest);
    if (command == "version" || command == "--version") {
        out() << "omareader " << QCoreApplication::applicationVersion() << Qt::endl;
        return 0;
    }
    out() << usage();
    out().flush();
    return 0;
}

}
