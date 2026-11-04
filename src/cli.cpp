#include "cli.h"

#include "bookloader.h"
#include "kokoro.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTextStream>
#include <cstdio>

namespace {

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

int doctor(const QStringList &arguments) {
    Command command("doctor", "Check that read-aloud is ready.");
    command.json();
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    const KokoroSetup setup = KokoroSetup::find();
    if (command.parser.isSet("json")) {
        printJson(setup.toJson());
        return setup.ready() ? 0 : 1;
    }
    out() << "koko:   " << (setup.binary.isEmpty() ? "missing" : setup.binary) << Qt::endl;
    out() << "model:  " << (setup.model.isEmpty() ? "missing" : setup.model) << Qt::endl;
    out() << "voices: " << (setup.voices.isEmpty() ? "missing" : setup.voices) << Qt::endl;
    if (!setup.ready()) {
        out() << "searched: " << setup.searchedModelDirectories.join(", ") << Qt::endl;
        return fail(setup.problem());
    }
    out() << "Read-aloud is ready." << Qt::endl;
    return 0;
}

int setup(const QStringList &arguments) {
    Command command("setup", "Install Kokoros and the Kokoro model (uses the network once).");
    command.parser.addOption({"dry-run", "Show what would be done without changing anything"});
    if (!command.parse(arguments))
        return command.parser.isSet("help") ? 0 : 1;
    QFile script(":/setup-kokoros.sh");
    if (!script.open(QIODevice::ReadOnly))
        return fail("The setup script is missing from this build.");
    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    QStringList shellArguments{"-s", "--"};
    if (command.parser.isSet("dry-run"))
        shellArguments << "--dry-run";
    process.start("sh", shellArguments);
    if (!process.waitForStarted())
        return fail("Could not run sh.");
    process.write(script.readAll());
    process.closeWriteChannel();
    process.waitForFinished(-1);
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : 1;
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
           "  doctor [--json]                 Check that read-aloud is ready\n"
           "  setup [--dry-run]               Install Kokoros and the model (uses the network once)\n"
           "  skill [install]                 Print the skill for coding agents, or install it\n"
           "  version                         Print the version\n";
}

bool isCommand(const QString &word) {
    return QStringList{"info", "text", "doctor", "setup", "skill", "help", "--help", "-h", "version", "--version"}
        .contains(word);
}

int run(const QStringList &arguments) {
    const QString command = arguments.value(1);
    const QStringList rest = QStringList{arguments.value(0) + " " + command} + arguments.mid(2);
    if (command == "info") return info(rest);
    if (command == "text") return text(rest);
    if (command == "doctor") return doctor(rest);
    if (command == "setup") return setup(rest);
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
