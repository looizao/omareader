// Test code gains nothing from the optimiser, and this file is by far the
// slowest to compile, so it is built without it. The code under test in
// src/ keeps the flags it is built with.
#pragma GCC optimize("O0")

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QClipboard>
#include <QGuiApplication>
#include <QProcess>
#include <functional>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QQmlContext>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QTextCursor>
#include <QTextDocument>
#include <QPainter>
#include <QPdfWriter>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <zlib.h>

#include "annotations.h"
#include "article.h"
#include "audioplayer.h"
#include "book.h"
#include "bookloader.h"
#include "dictionary.h"
#include "exporter.h"
#include "filepicker.h"
#include "fixtures.h"
#include "highlight.h"
#include "kokoro.h"
#include "pronunciation.h"
#include "library.h"
#include "narrator.h"
#include "qwen.h"
#include "reader.h"
#include "speech.h"
#include "synthesizer.h"
#include "theme.h"
#include "titleagent.h"
#include "translator.h"
#include "voicesettings.h"
#include "webimport.h"

// The real window (qrc:/Main.qml) with everything main() gives it, drawn
// offscreen, so tests can check what is actually on screen. Set
// OMAREADER_SCREENSHOTS to a folder to keep each shot for a look.
class AppWindow {
public:
    AppWindow(const QString &dir)
        : library(dir + "/library.json", dir + "/covers"), reader(&library, &narrator, nullptr),
          annotations(dir + "/annotations", "this-computer"), lookup(QStringList{dir + "/dictionaries"}),
          pronunciations(dir + "/pronunciations.json") {
        reader.setExporter(&exporter);
        reader.setAnnotations(&annotations);
        qmlRegisterType<ReadingHighlight>("Omareader", 1, 0, "ReadingHighlight");
        qmlRegisterUncreatableType<Narrator>("Omareader", 1, 0, "Narrator", "Use the narrator context property.");
        qmlRegisterUncreatableType<Exporter>("Omareader", 1, 0, "Exporter", "Use the exporter context property.");
        QQuickStyle::setStyle("Material");
        auto *context = engine.rootContext();
        context->setContextProperty("translator", &translator);
        context->setContextProperty("theme", &theme);
        context->setContextProperty("library", &library);
        context->setContextProperty("narrator", &narrator);
        context->setContextProperty("reader", &reader);
        context->setContextProperty("exporter", &exporter);
        context->setContextProperty("annotations", &annotations);
        context->setContextProperty("titleAgent", &titleAgent);
        context->setContextProperty("webImport", &webImport);
        context->setContextProperty("lookup", &lookup);
        context->setContextProperty("pronunciations", &pronunciations);
    }

    bool show(QSize size = {1100, 720}) {
        engine.load(QUrl("qrc:/Main.qml"));
        window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (!window)
            return false;
        window->resize(size);
        window->show();
        return QTest::qWaitForWindowExposed(window);
    }

    // Every visible item with this objectName, including those in views'
    // delegates, which findChild() can't see.
    QList<QQuickItem *> items(const QString &name) const {
        QList<QQuickItem *> found;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
            if (item->objectName() == name && item->isVisible())
                found.append(item);
            for (QQuickItem *child : item->childItems())
                walk(child);
        };
        walk(window->contentItem());
        return found;
    }

    QImage grab(const QString &name) {
        QTest::qWait(300);
        const QImage image = window->grabWindow();
        const QString folder = qEnvironmentVariable("OMAREADER_SCREENSHOTS");
        if (!folder.isEmpty()) {
            QDir().mkpath(folder);
            image.save(folder + "/" + name + ".png");
        }
        return image;
    }

    // An item's area of a grab, in the grab's pixels.
    QRect area(const QImage &image, QQuickItem *item) const {
        const qreal ratio = image.devicePixelRatio();
        const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        return QRect(qRound(r.x() * ratio), qRound(r.y() * ratio), qRound(r.width() * ratio), qRound(r.height() * ratio));
    }

    Translator translator;
    Theme theme;
    Library library;
    Narrator narrator;
    Reader reader;
    Annotations annotations;
    Exporter exporter;
    TitleAgent titleAgent;
    WebImport webImport;
    WordLookup lookup;
    Pronunciations pronunciations;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
};

// Where the light marks inside a round control sit: the bounding box of the
// pixels brighter than the threshold within the circle inscribed in area,
// so whatever shows past the circle's edge is left out.
static QRect brightBoxInCircle(const QImage &image, const QRect &area, int threshold = 190) {
    const QPointF centre = QRectF(area).center();
    const qreal radius = qMin(area.width(), area.height()) / 2.0 - 2;
    QRect box;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            if (QLineF(centre, QPointF(x + 0.5, y + 0.5)).length() <= radius && qGray(image.pixel(x, y)) > threshold)
                box |= QRect(x, y, 1, 1);
    return box;
}

// How many pixels in area are light, as text is on the dark theme.
static int brightPixels(const QImage &image, const QRect &area, int threshold = 150) {
    int count = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            count += qGray(image.pixel(x, y)) > threshold;
    return count;
}

// How many pixels in area are the error box's red border or text.
static int redPixels(const QImage &image, const QRect &area) {
    int count = 0;
    for (int y = qMax(0, area.top()); y <= qMin(image.height() - 1, area.bottom()); ++y)
        for (int x = qMax(0, area.left()); x <= qMin(image.width() - 1, area.right()); ++x) {
            const QColor c = image.pixelColor(x, y);
            count += c.red() > 140 && c.red() > c.green() + 40 && c.red() > c.blue() + 40;
        }
    return count;
}

// Every book here is made up inside the test, so nothing personal or
// copyrighted is checked in.
class OmareaderTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // Tests never see the real home: no saved library, no installed model.
        qputenv("HOME", m_home.path().toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + "/.local/share").toUtf8());
        qputenv("XDG_CACHE_HOME", (m_home.path() + "/.cache").toUtf8());
        qunsetenv("OMAREADER_MODELS");
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_settings.path());
        QCoreApplication::setOrganizationName("omareader-tests");
        QCoreApplication::setApplicationName("omareader-tests");
    }

    // Theme and language, from the Omarchy app template.

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding() {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");
        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})");
        write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})");
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
    }

    void shippedCatalogsAreValidJson() {
        const QDir dir(QFINDTESTDATA("../src/i18n"));
        QVERIFY(dir.exists());
        for (const QString &name : dir.entryList({"*.json"})) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QJsonParseError error;
            QJsonDocument::fromJson(file.readAll(), &error);
            QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(name + ": " + error.errorString()));
        }
    }

    // Splitting text into sentences and words.

    void sentencesSplitAndKeepAbbreviations() {
        const QString text = "Mr. Smith arrived. He sat down! Did he stay? Yes.";
        const auto ranges = Text::sentenceRanges(text);
        QCOMPARE(ranges.size(), 4);
        QCOMPARE(text.mid(ranges[0].first, ranges[0].second - ranges[0].first), QString("Mr. Smith arrived."));
        QCOMPARE(text.mid(ranges[3].first, ranges[3].second - ranges[3].first), QString("Yes."));
    }

    void longSentencesBreakAtClauses() {
        QString text;
        for (int i = 0; i < 40; ++i)
            text += QString("clause number %1, ").arg(i);
        text += "end.";
        const auto ranges = Text::sentenceRanges(text, 120);
        QVERIFY(ranges.size() > 4);
        for (const auto &r : ranges) {
            QVERIFY(r.second - r.first <= 120);
            QVERIFY(!text.at(r.first).isSpace());
        }
        QCOMPARE(ranges.last().second, int(text.size()));
    }

    void wordsKeepApostrophesAndHyphens() {
        const QString text = QString("“Don’t worry,” said the well-known café owner ") + QChar(0x2014) + " 1984.";
        QStringList words;
        for (const auto &w : Text::words(text, 0, int(text.size())))
            words << text.mid(w.start, w.length);
        QCOMPARE(words, (QStringList{"Don’t", "worry", "said", "the", "well-known", "café", "owner", "1984"}));
        QCOMPARE(Text::normalizeWord("Café’s"), QString("cafes"));
    }

    void bookFindsSentenceAtOffset() {
        Book book;
        book.chapters = {{"One", {{"First. Second.", 0}, {"Third.", 0}}}, {"Two", {{"Fourth.", 0}}}};
        book.segment();
        QCOMPARE(book.sentenceCount(), 4);
        QCOMPARE(book.chapterFirstSentence, (QList<int>{0, 3}));
        QCOMPARE(book.sentenceAt(0, 0, 0), 0);
        QCOMPARE(book.sentenceAt(0, 0, 8), 1);
        QCOMPARE(book.sentenceAt(0, 1, 0), 2);
        QCOMPARE(book.sentenceAt(1, 0, 0), 3);
        QCOMPARE(book.sentenceText(1), QString("Second."));
        QCOMPARE(book.sentenceWords(3), QStringList{"Fourth"});
    }

    // What clutter() leaves out, as the pieces of text it covers.
    static QStringList clutterIn(const QString &text, bool pdf = false) {
        QStringList pieces;
        for (const auto &range : Text::clutter(text, pdf))
            pieces << text.mid(range.first, range.second - range.first);
        return pieces;
    }

    void clutterFindsMarkersLinksAndAsides() {
        // Bare footnote numbers only in a PDF's text; other formats mark
        // their notes up, and the loader drops them.
        QCOMPARE(clutterIn("A single point of failure.1 While replication helps,23 it costs.", true),
                 (QStringList{"1", "23"}));
        QCOMPARE(clutterIn("This is a common pattern1 . For example", true), QStringList{"1"});
        QVERIFY(clutterIn("A single point of failure.1 While").isEmpty());
        QCOMPARE(clutterIn("It gained a cult following,[5] and wide use [12, 14]. Others agree [3–7]."),
                 (QStringList{"[5]", "[12, 14]", "[3–7]"}));
        QCOMPARE(clutterIn("Good enough is best [You95], as argued before [GHJV95; KP99]."),
                 (QStringList{"[You95]", "[GHJV95; KP99]"}));
        QCOMPARE(clutterIn("He wrote it [sic] himself [citation needed]. Then [...] it ended.[a]"),
                 (QStringList{"[sic]", "[citation needed]", "[...]", "[a]"}));
        QCOMPARE(clutterIn("See https://example.com/a(b)c, or (www.example.org/x). Read oreil.ly/2R1CB."),
                 (QStringList{"https://example.com/a(b)c", "www.example.org/x", "oreil.ly/2R1CB"}));
        // A PDF breaks long addresses across lines, after a hyphen or inside the path.
        QCOMPARE(clutterIn("At https://aws.amazon.com/builders- library/avoiding-overload. Then"),
                 QStringList{"https://aws.amazon.com/builders- library/avoiding-overload"});
        QCOMPARE(clutterIn("At https://brooker.co.za/blog/co ntrol.html today"),
                 QStringList{"https://brooker.co.za/blog/co ntrol.html"});
        QCOMPARE(clutterIn("A sidecar (see Figure 6-5) runs beside it (Chapter 11), as in (pp. 12–15)."),
                 (QStringList{"(see Figure 6-5)", "(Chapter 11)", "(pp. 12–15)"}));
        QCOMPARE(clutterIn("It was trained from scratch (Zhu et al., 2017) and tuned (Smith, 2020; Lee and Park, 2019)."),
                 (QStringList{"(Zhu et al., 2017)", "(Smith, 2020; Lee and Park, 2019)"}));
        // A footnote number after a reference that goes, goes with it.
        QCOMPARE(clutterIn("by one pixel (see Figure 4-12).49 Then", true), (QStringList{"(see Figure 4-12)", "49"}));
        // An address stops before a quote, and before a word with a slash.
        QCOMPARE(clutterIn("See https://example.com/path- \"the tutorial\" now"),
                 QStringList{"https://example.com/path-"});
        QCOMPARE(clutterIn("Use https://example.org/a and/or ask your admin."), QStringList{"https://example.org/a"});
        QCOMPARE(clutterIn("Go to https://example.org/a. Then docs/index.html says more."),
                 QStringList{"https://example.org/a"});
    }

    void clutterLeavesCodeNumbersAndWords() {
        for (const QString &text : {
                 QString("Read argv[2] and x[i] from the list."),
                 QString("Scale it to the range [0, 1] first."),
                 QString("The logits computed from the last layer are [1, 2]."),
                 QString("The tensor has a shape of [4]."),
                 QString("Tokens: Hello [11] --> , [1644] --> Ar"),
                 QString(">jobs [1] 5036 Running foo"),
                 QString("GPT4 and x86 and base64 stay whole words."),
                 QString("As Appendix A.1 shows, and Fig.1 too, Python 3.10 is out."),
                 QString("It shipped with (Windows 2000) back then."),
                 QString("Children [with ADHD] do well with structure."),
                 QString("For more, see examples (see examples above) and oreilly.com today."),
                 QString("For a complete list, see [110]."),
                 QString("Write to support@example.com for help."),
                 QString("Keep server:443 open, at a ratio of 3:1.")})
            QVERIFY2(Text::clutter(text, true).isEmpty(), qPrintable(text + " → " + clutterIn(text, true).join(" | ")));
    }

    // Pronunciations: respellings the voice says in place of the written words.

    void lexiconMatchesWholeWordsAndKeepsCapitalsApart() {
        Lexicon lexicon;
        QVERIFY(lexicon.set("Hermione", "Her-my-oh-nee"));
        QVERIFY(lexicon.set("US", "you ess"));
        QVERIFY(lexicon.set("New York", "Noo York"));
        QVERIFY(lexicon.set("New York City", "the big apple"));
        QVERIFY(lexicon.set("Ender's", "Ender-z"));
        auto said = [&](const QString &text) {
            QString out = text;
            const auto found = lexicon.find(text);
            for (int i = int(found.size()) - 1; i >= 0; --i)
                out.replace(found[i].start, found[i].end - found[i].start, found[i].spoken);
            return out;
        };
        // Any case, whole words only, and a possessive keeps its ending.
        QCOMPARE(said("HERMIONE and hermione's cat"), QString("Her-my-oh-nee and Her-my-oh-nee's cat"));
        QCOMPARE(said("Hermiones and xHermione"), QString("Hermiones and xHermione"));
        // An entry in capitals leaves the ordinary word alone.
        QCOMPARE(said("Tell us about the US."), QString("Tell us about the you ess."));
        // The longest entry wins, and a phrase may break across lines.
        QCOMPARE(said("New York City, New\nYork"), QString("the big apple, Noo York"));
        // Either apostrophe matches.
        QCOMPARE(said("Ender’s Game"), QString("Ender-z Game"));
        QVERIFY(lexicon.find("nothing here").isEmpty());
        QVERIFY(Lexicon().find("Hermione").isEmpty());
    }

    void lexiconKeepsOneEntryPerWordAndSavesIt() {
        QTemporaryDir dir;
        const QString file = dir.filePath("sub/pronunciations.json");
        Lexicon lexicon;
        QVERIFY(lexicon.set("  Saoirse   Ronan ", "Sur-sha Ronan"));
        QCOMPARE(lexicon.entries().first().written, QString("Saoirse Ronan"));
        // The same word in another case replaces it; capitals are their own.
        QVERIFY(lexicon.set("kubectl", "cube control"));
        QVERIFY(lexicon.set("Kubectl", "cube cuddle"));
        QVERIFY(lexicon.set("SQL", "sequel"));
        QVERIFY(lexicon.set("sql", "squeal"));
        QCOMPARE(lexicon.entries().size(), 4);
        QCOMPARE(lexicon.entries().value(lexicon.indexOf("kubectl")).spoken, QString("cube cuddle"));
        QCOMPARE(lexicon.entries().value(lexicon.indexOf("SQL")).spoken, QString("sequel"));
        QCOMPARE(lexicon.entries().value(lexicon.indexOf("sql")).spoken, QString("squeal"));
        QCOMPARE(lexicon.indexOf("KUBECTL"), -1); // in capitals, it would be an entry of its own
        // Each side needs something to say.
        QVERIFY(!lexicon.set("", "x"));
        QVERIFY(!lexicon.set("word", " ... "));
        QVERIFY(!lexicon.set("one two three four five six seven", "x"));
        QVERIFY(lexicon.save(file));
        Lexicon loaded = Lexicon::load(file);
        QCOMPARE(loaded.entries().size(), 4);
        QVERIFY(loaded.remove("sql"));
        QVERIFY(!loaded.remove("sql"));
        QCOMPARE(loaded.find("SQL and sql").size(), 1);
        QVERIFY(Lexicon::load(dir.filePath("missing.json")).isEmpty());

        // The window's list reads the file again before each change, so an
        // entry added from the command line meanwhile is kept.
        Pronunciations list(file);
        QSignalSpy changed(&list, &Pronunciations::changed);
        QCOMPARE(list.count(), 4);
        Lexicon outside = Lexicon::load(file);
        outside.set("Nguyen", "Win");
        QVERIFY(outside.save(file));
        QVERIFY(list.set("Hermione", "Her-my-oh-nee"));
        QCOMPARE(list.count(), 6);
        QCOMPARE(list.spokenFor("nguyen"), QString("Win"));
        // Editing an entry under a new written form replaces it.
        QVERIFY(list.set("Hermione Granger", "Her-my-oh-nee Granger", "Hermione"));
        QCOMPARE(list.spokenFor("Hermione"), QString());
        QCOMPARE(list.count(), 6);
        QVERIFY(list.remove("Hermione Granger"));
        QVERIFY(!list.set("x", ""));
        QCOMPARE(changed.size(), 3);
        const QVariantList entries = list.entries();
        QCOMPARE(entries.first().toMap().value("written").toString(), QString("Kubectl"));
        QCOMPARE(Lexicon::load(file).entries().size(), 5);
    }

    void lexiconStaysWholeWhateverItHolds() {
        // An entry with an emoji, or a very long list, leaves every other
        // entry working.
        Lexicon lexicon;
        QVERIFY(lexicon.set("Hermione", "Her-my-oh-nee"));
        QVERIFY(lexicon.set(QString::fromUtf8("A\xF0\x9F\x98\x80"), "ay"));
        auto said = [&](const QString &text) {
            QString out = text;
            const auto found = lexicon.find(text);
            for (int i = int(found.size()) - 1; i >= 0; --i)
                out.replace(found[i].start, found[i].end - found[i].start, found[i].spoken);
            return out;
        };
        QCOMPARE(said(QString::fromUtf8("Hermione and A\xF0\x9F\x98\x80 here")), QString("Her-my-oh-nee and ay here"));
        for (int i = 0; i < 1001; ++i)
            QVERIFY(lexicon.set(QString("term%1").arg(i), QString("word %1").arg(i)));
        QCOMPARE(said("term0, term999 and term1000 with Hermione"),
                 QString("word 0, word 999 and word 1000 with Her-my-oh-nee"));

        // An entry in capitals wins over the same word in any case, whichever came first.
        Lexicon cases;
        QVERIFY(cases.set("us", "uss"));
        QVERIFY(cases.set("US", "you ess"));
        QVERIFY(cases.set("Don", "Dawn"));
        QString text = "Tell us about the US. Don't go, Don's car, Don. O'Don stays.";
        const auto found = cases.find(text);
        for (int i = int(found.size()) - 1; i >= 0; --i)
            text.replace(found[i].start, found[i].end - found[i].start, found[i].spoken);
        // A contraction is one word, and stays; a possessive keeps its ending.
        QCOMPARE(text, QString("Tell uss about the you ess. Don't go, Dawn's car, Dawn. O'Don stays."));
    }

    void pronunciationsNeverSaveOverWhatTheyCantRead() {
        QTemporaryDir dir;
        const QString file = dir.filePath("pronunciations.json");
        const QByteArray broken = "{\"entries\": [{\"written\": \"Hermione\", \"spoken\": \"Her-my-oh-nee\"}";
        write(file, broken);
        Pronunciations list(file);
        QVERIFY(!list.set("SQL", "sequel"));
        QVERIFY(!list.remove("Hermione"));
        QFile in(file);
        QVERIFY(in.open(QIODevice::ReadOnly));
        QCOMPARE(in.readAll(), broken);
        in.close();
        Lexicon lexicon;
        QString error;
        QVERIFY(!Lexicon::read(file, &lexicon, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(Lexicon::read(dir.filePath("missing.json"), &lexicon));
        QVERIFY(lexicon.isEmpty());

        // While another change holds the file, this one waits, then gives
        // up rather than saving over it.
        QFile::remove(file);
        QLockFile other(file + ".lock");
        QVERIFY(other.tryLock(0));
        QVERIFY(!list.set("SQL", "sequel"));
        other.unlock();
        QVERIFY(list.set("SQL", "sequel"));
        QVERIFY(list.set("Hermione", "Her-my-oh-nee"));
        QCOMPARE(Lexicon::load(file).entries().size(), 2);
    }

    void utteranceRespellsTheTextAndKeepsThePageWords() {
        Book book;
        book.format = "pdf";
        const QString text = "Hermione’s wand [12] met the US Navy in New York City today.";
        book.chapters = {{"One", {{text, 0}, {"Nothing to respell here.", 0}}}};
        book.segment();
        Lexicon lexicon;
        lexicon.set("Hermione", "Her my oh nee");
        lexicon.set("US", "you ess");
        lexicon.set("New York City", "Gotham");
        const Utterance u = Pronounce::utterance(book, 0, lexicon);
        // The citation is still left out, and the respellings are in.
        QCOMPARE(u.text, QString("Her my oh nee’s wand met the you ess Navy in Gotham today."));
        QCOMPARE(u.words, (QStringList{"Her", "my", "oh", "nee’s", "wand", "met", "the", "you", "ess", "Navy",
                                       "in", "Gotham", "today"}));
        const QStringList page = book.sentenceWords(0);
        QCOMPARE(page, (QStringList{"Hermione’s", "wand", "met", "the", "US", "Navy", "in", "New", "York", "City", "today"}));
        QCOMPARE(u.heard.size(), page.size());
        QCOMPARE(u.heard.at(0), qMakePair(0, 3));  // Hermione's is heard as four words
        QCOMPARE(u.heard.at(1), qMakePair(4, 4));
        QCOMPARE(u.heard.at(4), qMakePair(7, 8));  // US
        QCOMPARE(u.heard.at(7), qMakePair(11, 11)); // New York City, all three as one word
        QCOMPARE(u.heard.at(9), qMakePair(11, 11));
        QCOMPARE(u.heard.at(10), qMakePair(12, 12));
        // Without a match, or a lexicon, the sentence is said as written.
        const Utterance plain = Pronounce::utterance(book, 1, lexicon);
        QCOMPARE(plain.text, book.sentenceText(1));
        QCOMPARE(plain.words, book.sentenceWords(1));
        QCOMPARE(Pronounce::utterance(book, 0, Lexicon()).text, book.sentenceText(0));
        QVERIFY(Pronounce::utterance(book, 9, lexicon).words.isEmpty());
    }

    void pronunciationTimingsLightThePageWords() {
        Book book;
        book.chapters = {{"One", {{"Ask Hermione in New York now.", 0}}}};
        book.segment();
        Lexicon lexicon;
        lexicon.set("Hermione", "Her-my-oh-nee");
        lexicon.set("New York", "Gotham");
        const Utterance u = Pronounce::utterance(book, 0, lexicon);
        QCOMPARE(u.text, QString("Ask Her-my-oh-nee in Gotham now."));
        // The engine reports the respelling in pieces; the page word is lit
        // from the first to the last.
        const QList<SpokenWord> spoken{{"Ask", 0.0, 0.3}, {"Her", 0.3, 0.5}, {"my", 0.5, 0.7}, {"oh", 0.7, 0.8},
                                       {"nee", 0.8, 1.0}, {"in", 1.0, 1.2}, {"Gotham", 1.2, 1.8}, {"now", 1.8, 2.0}};
        const QStringList page = book.sentenceWords(0);
        const auto timings = Pronounce::timings(u, page, spoken, 2.0);
        QCOMPARE(timings.size(), page.size());
        QCOMPARE(timings[0].start, 0.0);
        QCOMPARE(timings[1].start, 0.3);
        QCOMPARE(timings[1].end, 1.0);
        QCOMPARE(timings[2].start, 1.0);
        // "New York" shares Gotham's time by length: 4 to 5.
        QCOMPARE(timings[3].start, 1.2);
        QVERIFY(qAbs(timings[3].end - (1.2 + 0.6 * 4 / 9)) < 1e-9);
        QCOMPARE(timings[4].end, 1.8);
        QCOMPARE(timings[5].start, 1.8);
    }

    void clutterIsShownButNotRead() {
        Book book;
        book.format = "pdf";
        const QString text = "Replication helps.1 See https://example.com/x for more [12].";
        book.chapters = {{"One", {{text, 0}, {"https://example.com/only", 0}, {"1 A footnote the voice skips.", 0, {}, true}}}};
        book.segment();
        QCOMPARE(book.chapters[0].paragraphs[0].text, text); // the text shown is untouched
        QCOMPARE(book.sentenceCount(), 4);
        // The footnote number ends the first sentence, as the voice hears it.
        QCOMPARE(book.sentenceText(0), QString("Replication helps."));
        QCOMPARE(book.sentenceText(1), QString("See for more."));
        QCOMPARE(book.sentenceWords(1), (QStringList{"See", "for", "more"}));
        // The words keep their places in the text, for the highlight.
        const WordSpan more = book.sentences[1].words.last();
        QCOMPARE(text.mid(more.start, more.length), QString("more"));
        QCOMPARE(book.sentences[1].skipped.size(), 2);
        // A sentence of nothing but clutter, and a footnote, aren't read at all.
        QVERIFY(!book.sentences[2].speakable());
        QVERIFY(!book.sentences[3].speakable());
        QCOMPARE(book.wordCount(), 5);
        // Sentences with nothing skipped read as they are shown.
        Book plain;
        plain.chapters = {{"One", {{"It was, as they say ( quietly ) , over.", 0}}}};
        plain.segment();
        QCOMPARE(plain.sentenceText(0), QString("It was, as they say ( quietly ) , over."));
    }

    // Formats.

    void htmlBecomesParagraphs() {
        QString title;
        const auto paragraphs = BookLoader::htmlParagraphs(
            "<html><head><title>T</title><style>p{}</style></head><body>"
            "<h2>Chapter&nbsp;One</h2><p>It was a <i>dark</i> &amp; stormy\nnight<sup>1</sup>.</p>"
            "<p>Line one<br/>line two</p><script>ignored()</script><div><p>Caf&eacute; &#8212; &#x41;</p></div>"
            "<p>   </p></body></html>",
            &title);
        QCOMPARE(title, QString("T"));
        QCOMPARE(paragraphs.size(), 4);
        QCOMPARE(paragraphs[0].level, 2);
        QCOMPARE(paragraphs[0].text, QString("Chapter One"));
        QCOMPARE(paragraphs[1].text, QString("It was a dark & stormy night."));
        QCOMPARE(paragraphs[2].text, QString("Line one\nline two"));
        QCOMPARE(paragraphs[3].text, QString("Café ") + QChar(0x2014) + " A");
    }

    void plainTextFindsChaptersAndDropsGutenbergWrapper() {
        const QString text =
            "Licence header\n*** START OF THE PROJECT GUTENBERG EBOOK TEST ***\n\n"
            "CHAPTER I.\n\nThe first line\nwraps here.\n\nSecond paragraph.\n\n"
            "CHAPTER II.\n\nAnother chapter.\n\n*** END OF THE PROJECT GUTENBERG EBOOK TEST ***\nfooter";
        const auto chapters = BookLoader::textChapters(text);
        QCOMPARE(chapters.size(), 2);
        QCOMPARE(chapters[0].title, QString("CHAPTER I."));
        QCOMPARE(chapters[0].paragraphs.size(), 3);
        QCOMPARE(chapters[0].paragraphs[1].text, QString("The first line wraps here."));
        QCOMPARE(chapters[1].paragraphs.last().text, QString("Another chapter."));
    }

    void markdownHeadingsMakeChapters() {
        const QString text = "---\ntitle: X\n---\n# Intro\n\nSome *emphasis* and a [link](http://x).\n\n"
                             "```\ncode()\n```\n\n## Next\n\n- item one\n- item two\n";
        const auto chapters = BookLoader::markdownChapters(text);
        QCOMPARE(chapters.size(), 2);
        QCOMPARE(chapters[0].title, QString("Intro"));
        QCOMPARE(chapters[0].paragraphs[1].text, QString("Some emphasis and a link."));
        QCOMPARE(chapters[1].paragraphs.size(), 3);
        QCOMPARE(chapters[1].paragraphs[2].text, QString("item two"));
    }

    void markdownInlineKeepsStylesAndDropsMarks() {
        QList<TextStyle> styles;
        QCOMPARE(BookLoader::markdownInline("Some **bold text here** and *slanted* words.", &styles),
                 QString("Some bold text here and slanted words."));
        QCOMPARE(styles.size(), 2);
        QCOMPARE(styles[0], (TextStyle{5, 14, TextStyle::Bold}));
        QCOMPARE(styles[1], (TextStyle{24, 7, TextStyle::Italic}));

        QCOMPARE(BookLoader::markdownInline("***both*** __strong__ _em_ ~~gone~~ `x * y`", &styles),
                 QString("both strong em gone x * y"));
        QCOMPARE(styles,
                 (QList<TextStyle>{{0, 4, TextStyle::Bold | TextStyle::Italic},
                                   {5, 6, TextStyle::Bold},
                                   {12, 2, TextStyle::Italic},
                                   {15, 4, TextStyle::Strike},
                                   {20, 5, TextStyle::Code}}));

        // Emphasis nests, and a link reads as its text, styles and all.
        QCOMPARE(BookLoader::markdownInline("**bold with *italic* inside** and [a **link**](http://x \"t\").", &styles),
                 QString("bold with italic inside and a link."));
        QCOMPARE(styles,
                 (QList<TextStyle>{{0, 10, TextStyle::Bold},
                                   {10, 6, TextStyle::Bold | TextStyle::Italic},
                                   {16, 7, TextStyle::Bold},
                                   {30, 4, TextStyle::Bold}}));

        // What isn't emphasis stays as written.
        QCOMPARE(BookLoader::markdownInline("2 * 3 * 4 = 24, snake_case_name, ~5 min, \\*not*", &styles),
                 QString("2 * 3 * 4 = 24, snake_case_name, ~5 min, *not*"));
        QVERIFY(styles.isEmpty());
        QCOMPARE(BookLoader::markdownInline("\\*literal\\* and an ![image](i.png) gone"),
                 QString("*literal* and an gone"));
        QCOMPARE(BookLoader::markdownInline("a lone * star and **unclosed"), QString("a lone * star and **unclosed"));
    }

    void pastedMarkdownReadsWithoutMarks() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.json"), dir.filePath("covers"));
        const QString path = library.addText(
            {}, "# **Shopping** list\n\nBuy *bread* and **milk** today.\n\n- one `item`\n- two\n\n```\ncode()\n```\n");
        QCOMPARE(library.entry(path).title, QString("Shopping list"));
        QCOMPARE(PastedTitle::fromText("Buy **fresh** bread, `now`."), QString("Buy fresh bread, now."));

        QString error;
        const auto book = BookLoader::load(path, &error);
        QVERIFY2(book, qPrintable(error));
        const auto &paragraphs = book->chapters.first().paragraphs;
        QCOMPARE(paragraphs.size(), 4);
        QCOMPARE(paragraphs[0].text, QString("Shopping list"));
        QCOMPARE(paragraphs[0].level, 1);
        QCOMPARE(paragraphs[0].styles, (QList<TextStyle>{{0, 8, TextStyle::Bold}}));
        QCOMPARE(paragraphs[1].text, QString("Buy bread and milk today."));
        QCOMPARE(paragraphs[1].styles,
                 (QList<TextStyle>{{4, 5, TextStyle::Italic}, {14, 4, TextStyle::Bold}}));
        QCOMPARE(paragraphs[2].text, QString("one item"));
        QCOMPARE(paragraphs[3].text, QString("two"));
        // The voice gets the words, never the marks.
        for (int i = 0; i < book->sentenceCount(); ++i) {
            QVERIFY2(!book->sentenceText(i).contains('*'), qPrintable(book->sentenceText(i)));
            QVERIFY2(!book->sentenceText(i).contains('`'), qPrintable(book->sentenceText(i)));
        }

        // Headings in pasted text stay headings, but the text stays one chapter.
        const QString sections = library.addText(
            "Sections", "# One\n\nFirst part.\n\n## Two\n\nSecond part.\n\n# Three\n\nThird part.");
        const auto whole = BookLoader::load(sections, &error);
        QVERIFY2(whole, qPrintable(error));
        QCOMPARE(whole->chapters.size(), 1);
        QCOMPARE(whole->chapters.first().title, QString("Sections"));
        QCOMPARE(whole->chapters.first().paragraphs.size(), 6);
        QCOMPARE(whole->chapters.first().paragraphs[2].text, QString("Two"));
        QCOMPARE(whole->chapters.first().paragraphs[2].level, 2);

        // Pasted lines with no blank line between them stay separate paragraphs.
        const auto lines = BookLoader::markdownChapters("First line.\n*Second* line.\nThird line.");
        QCOMPARE(lines.first().paragraphs.size(), 3);
        QCOMPARE(lines.first().paragraphs[1].text, QString("Second line."));
    }

    void readerListsParagraphStyles() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.json"), dir.filePath("covers"));
        Reader reader(&library, nullptr, nullptr);
        QVERIFY(reader.addText("Styled", "Plain and **bold**."));
        QTRY_VERIFY(reader.hasBook());
        const QVariantMap paragraph = reader.paragraphs().first().toMap();
        QCOMPARE(paragraph.value("text").toString(), QString("Plain and bold."));
        const QVariantList styles = paragraph.value("styles").toList();
        QCOMPARE(styles.size(), 1);
        QCOMPARE(styles.first().toMap().value("start").toInt(), 10);
        QCOMPARE(styles.first().toMap().value("length").toInt(), 4);
        QCOMPARE(styles.first().toMap().value("flags").toInt(), int(TextStyle::Bold));
        QVERIFY(library.books().first().toMap().value("pasted").toBool());
        reader.closeBook();
    }

    void highlightKeepsInlineStyles() {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nTextEdit { text: \"Plain and bold words.\" }", QUrl());
        std::unique_ptr<QObject> edit(component.create());
        QVERIFY2(edit, qPrintable(component.errorString()));
        auto *quickDocument = edit->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(quickDocument);
        QTextDocument *document = quickDocument->textDocument();

        ReadingHighlight highlight;
        highlight.setStyles({QVariantMap{{"start", 10}, {"length", 4}, {"flags", int(TextStyle::Bold)}}});
        highlight.setDocument(quickDocument);
        auto weightAt = [&](int position) {
            QTextCursor cursor(document);
            cursor.setPosition(position + 1);
            return cursor.charFormat().fontWeight();
        };
        QCOMPARE(weightAt(11), int(QFont::Bold));
        QVERIFY(weightAt(2) != int(QFont::Bold));
        // The highlight passes over the bold word and leaves it bold.
        highlight.setWordStart(10);
        highlight.setWordEnd(14);
        QCOMPARE(weightAt(11), int(QFont::Bold));
        highlight.setWordStart(15);
        highlight.setWordEnd(20);
        QCOMPARE(weightAt(11), int(QFont::Bold));
        QVERIFY(weightAt(16) != int(QFont::Bold));
    }

    void highlightFadesWhatWasReadAndQuietsTheRest() {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        //                                               0         1         2         3
        //                                               0123456789012345678901234567890123456
        component.setData("import QtQuick\nTextEdit { text: \"One done. Two being read. Three next.\" }", QUrl());
        std::unique_ptr<QObject> edit(component.create());
        QVERIFY2(edit, qPrintable(component.errorString()));
        auto *quickDocument = edit->property("textDocument").value<QQuickTextDocument *>();
        QTextDocument *document = quickDocument->textDocument();
        auto formatAt = [&](int position) {
            QTextCursor cursor(document);
            cursor.setPosition(position + 1);
            return cursor.charFormat();
        };
        auto foregroundAt = [&](int position) {
            const QTextCharFormat format = formatAt(position);
            return format.hasProperty(QTextFormat::ForegroundBrush) ? format.foreground().color() : QColor();
        };

        const QColor read("#202020"), quiet("#404040"), word("#ffd60a"), wordText("#101010");
        ReadingHighlight highlight;
        highlight.setDocument(quickDocument);
        highlight.setSentenceStart(10);
        highlight.setSentenceEnd(25);
        highlight.setWordStart(14);
        highlight.setWordEnd(19);
        highlight.setWordColor(word);
        highlight.setWordTextColor(wordText);
        highlight.setReadColor(read);
        highlight.setQuietColor(quiet);
        highlight.setReadEnd(14);
        // Before the word: read. The word: highlighted. After it, in the
        // sentence: untouched. Past the sentence: quiet.
        QCOMPARE(foregroundAt(2), read);
        QCOMPARE(foregroundAt(11), read);
        QCOMPARE(foregroundAt(15), wordText);
        QCOMPARE(formatAt(15).background().color(), word);
        QVERIFY(!foregroundAt(21).isValid());
        QCOMPARE(foregroundAt(30), quiet);

        // Without a quiet colour, only what was read changes.
        highlight.setQuietColor(Qt::transparent);
        QVERIFY(!foregroundAt(30).isValid());
        QCOMPARE(foregroundAt(2), read);
        // A transparent word colour leaves the word's background alone and
        // marks it with the text colour only.
        highlight.setWordColor(Qt::transparent);
        QVERIFY(!formatAt(15).hasProperty(QTextFormat::BackgroundBrush));
        QCOMPARE(foregroundAt(15), wordText);
        // When reading stops, every colour is taken off again.
        highlight.setSentenceStart(-1);
        highlight.setSentenceEnd(-1);
        highlight.setWordStart(-1);
        highlight.setWordEnd(-1);
        highlight.setReadEnd(-1);
        for (int position : {2, 11, 15, 21, 30})
            QVERIFY2(!foregroundAt(position).isValid(), qPrintable(QString::number(position)));
    }

    void pdfTextReflows() {
        const QString output = "The quick brown fox jumps over the lazy dog and keeps run-\n"
                               "ning far away.\n\n12\n\fit continues on the next page.\nA new one.\n";
        const auto chapters = BookLoader::pdfChapters(output, {}, 10);
        QCOMPARE(chapters.size(), 1);
        QCOMPARE(chapters[0].title, QString("Pages 1–2"));
        QCOMPARE(chapters[0].paragraphs.size(), 2);
        QCOMPARE(chapters[0].paragraphs[0].text,
                 QString("The quick brown fox jumps over the lazy dog and keeps running far away."));
    }

    void pdfOutlineParses() {
        const auto items = BookLoader::pdfOutline(
            "<pdf2xml><page number=\"1\"></page><outline><item page=\"1\">Cover</item>"
            "<item page=\"3\">1 Fish &amp; Chips</item><outline><item page=\"4\">1.1 Batter</item></outline>"
            "<item page=\"6\">2 Peas</item></outline></pdf2xml>");
        QCOMPARE(items.size(), 4);
        QCOMPARE(items[1].title, QString("1 Fish & Chips"));
        QCOMPARE(items[1].page, 3);
        QCOMPARE(items[2].level, 1);
        QCOMPARE(items[3].level, 0);
    }

    void pdfChaptersFollowTheOutlineAndDropFurniture() {
        // Six pages; every page carries a running head, and page 4 a figure.
        QStringList pages;
        for (int page = 1; page <= 6; ++page) {
            QString text = QString("%1\n\nCHAPTER %2\n\n").arg(page).arg(page < 4 ? 1 : 2);
            if (page == 1)
                text += "1 Opening\n\nThe story starts on the first page.\n";
            else if (page == 4)
                text += "2 Middle\n\nInput\n0.25\nOutput layer\nHidden layer\n\nThe middle chapter begins here.\n";
            else
                text += QStringList{"", "", "Rain fell on the second page.", "Wind rose by the third.", "",
                                    "Snow came with the fifth page.", "Sun returned on the last one."}.at(page) + "\n";
            pages << text;
        }
        const QList<PdfOutlineItem> outline{{1, 0, "1 Opening"}, {4, 0, "2 Middle"}, {2, 1, "Chapter 2"}};
        const auto chapters = BookLoader::pdfChapters(pages.join('\f'), outline);
        QCOMPARE(chapters.size(), 2);
        QCOMPARE(chapters[0].title, QString("1 Opening"));
        QCOMPARE(chapters[1].title, QString("2 Middle"));
        QStringList second;
        for (const auto &p : chapters[1].paragraphs)
            second << p.text;
        QCOMPARE(second, (QStringList{"2 Middle", "The middle chapter begins here.",
                                      "Snow came with the fifth page.", "Sun returned on the last one."}));
        QCOMPARE(chapters[1].paragraphs[0].level, 2);
    }

    void pdfFootnotesAreKeptAsNotesTheVoiceSkips() {
        // A page whose paragraph runs over the break, with two footnotes at
        // its foot, and a numbered heading low on the next page.
        QStringList pages;
        pages << "Replication keeps a copy of the data on several machines, which\n"
                 "removes a single point of failure.1 While replication helps,2 it\n"
                 "also brings problems of its own, which the rest of this chap-\n"
                 "ter describes in some detail, starting with the leader and its\n"
                 "followers.\n"
                 "The leader takes every write and sends it on to the others, so\n"
                 "the followers apply the writes in the same order as the leader\n"
                 "does, and readers can ask any of them\n"
                 "1 Some systems call it a primary, and the\n"
                 "followers secondaries.\n"
                 "2 See the next chapter.\n"
                 "7\n";
        pages << "for the latest data they have.\n"
                 "Another paragraph follows here on the second page, and it is\n"
                 "long enough to read as plain text.\n"
                 "Some more text comes on this page, and then a heading after it.\n"
                 "It runs on for a few lines, with nothing like a note marker here.\n"
                 "One more line of reading text.\n"
                 "2 Leaders and Followers\n"
                 "The heading above is in the outline, and stays a heading.\n";
        const QList<PdfOutlineItem> outline{{1, 0, "Replication"}, {2, 1, "2 Leaders and Followers"}};
        const auto chapters = BookLoader::pdfChapters(pages.join('\f'), outline);
        QCOMPARE(chapters.size(), 1);
        const auto &paragraphs = chapters[0].paragraphs;
        QStringList notes, text;
        for (const auto &p : paragraphs)
            (p.note ? notes : text) << p.text;
        QCOMPARE(notes, (QStringList{"1 Some systems call it a primary, and the followers secondaries.",
                                     "2 See the next chapter."}));
        QVERIFY2(text.at(1).endsWith("readers can ask any of them for the latest data they have."), qPrintable(text.at(1)));
        QVERIFY(text.contains("2 Leaders and Followers"));
        QVERIFY(text.contains("The heading above is in the outline, and stays a heading."));

        Book book;
        book.format = "pdf";
        book.chapters = chapters;
        book.segment();
        for (int i = 0; i < book.sentenceCount(); ++i) {
            const Sentence &s = book.sentences.at(i);
            QCOMPARE(s.speakable(), !paragraphs.at(s.paragraph).note);
        }
        QVERIFY2(book.sentenceText(0).endsWith("removes a single point of failure."), qPrintable(book.sentenceText(0)));
        QVERIFY2(book.sentenceText(1).startsWith("While replication helps, it also"), qPrintable(book.sentenceText(1)));
    }

    void pdfFootnotesNeedAMarkerTheyAnswer() {
        auto notesIn = [](const QString &page) {
            QStringList notes;
            for (const auto &chapter : BookLoader::pdfChapters(page, {}))
                for (const auto &p : chapter.paragraphs)
                    if (p.note)
                        notes << p.text;
            return notes;
        };
        const QString lead = "The deployment script is written for Python3 and runs on every host we\n"
                             "manage, so the steps below apply to each of them in the same order\n"
                             "without any change at all, whatever the host is used for.\n"
                             "1 Copy the build to the host and unpack it in place.\n"
                             "2 Restart the service and wait for it to come back.\n";
        // A word's last digit is no footnote marker: the steps stay text.
        QVERIFY(notesIn(lead + "3 Verify that the deployment is healthy and serving.\n").isEmpty());
        // A marker in brackets is.
        const QString cited = "Take care when updating a foreign key in a large table [7]. The lock it\n"
                              "takes can hold up every write to that table for as long as it lasts,\n"
                              "which is why the migration runs at night when the traffic is low.\n"
                              "It runs in batches, each one small enough to finish in seconds.\n"
                              "7 The database lock can last for ten minutes on the largest tables.\n";
        QCOMPARE(notesIn(cited), QStringList{"7 The database lock can last for ten minutes on the largest tables."});
        // A line that carries on a sentence is text, cited number or not,
        // and so is a table after an index ("a[3]").
        const QString scaled = "Ideally, if you have ten times as many nodes, you can handle about\n"
                               "ten times the load, as the partitioning papers show [10]. With\n"
                               "every node taking an equal share, there is no hot spot at all, and\n"
                               "the system scales as far as the hardware lets it scale,\n"
                               "10 times as much data and 10 times the read and write throughput.\n";
        QVERIFY(notesIn(scaled).isEmpty());
        const QString table = "The cache holds a[3] and a[4] after the loop has run once over the\n"
                              "array, with each line holding four of its elements in a row and the\n"
                              "rest of the lines left empty until the next pass reaches them at last.\n"
                              "Element Address Hit or miss\n"
                              "3 [h]\n4 [h]\n";
        QVERIFY(notesIn(table).isEmpty());
    }

    void pdfDropsPageNumbersAndRunningHeadsNamingTheSection() {
        // The book's chapters are its parts; the running heads name the
        // section ("CHAPTER 22. CONTROL PLANES") a level down.
        QStringList pages;
        pages << "Part one\nThe part starts with words on its first page.\n";
        for (int page = 2; page <= 5; ++page)
            pages << QString("CHAPTER 22. CONTROL PLANES AND DATA PLANES\n%1 fill this page of the section.\n%2\n")
                         .arg(QStringList{"", "", "Words", "Lines", "Figures", "Tables"}.at(page))
                         .arg(page == 2 ? "Page 2 of 9" : page == 3 ? "- 3 -" : QString::number(page));
        const QList<PdfOutlineItem> outline{{1, 0, "Part one"}, {2, 1, "Control planes and data planes"}};
        const auto chapters = BookLoader::pdfChapters(pages.join('\f'), outline);
        QStringList text;
        for (const auto &p : chapters.last().paragraphs)
            text << p.text;
        QCOMPARE(text, (QStringList{"Part one", "The part starts with words on its first page.",
                                    "Words fill this page of the section.", "Lines fill this page of the section.",
                                    "Figures fill this page of the section.", "Tables fill this page of the section."}));
        // A line of code that merely names a short section stays.
        QStringList code{"Intro\nThe spec starts here.\n",
                         "OpenAPI\nFor example, this is how part of the endpoint could be defined in the spec\n"
                         "openapi: 3.0.0\ninfo: version 1 of the catalog service and its endpoints.\n"};
        const auto spec = BookLoader::pdfChapters(code.join('\f'), {{1, 0, "Intro"}, {2, 1, "OpenAPI"}});
        bool kept = false;
        for (const auto &p : spec.last().paragraphs)
            kept = kept || p.text.contains("openapi: 3.0.0");
        QVERIFY(kept);
    }

    void epubLoadsWithTocTitlesAndCover() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book.epub");
        QImage cover(40, 60, QImage::Format_RGB32);
        cover.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        cover.save(&buffer, "PNG");
        Fixtures::writeEpub(path, png);
        QString error;
        const auto book = BookLoader::load(path, &error);
        QVERIFY2(book, qPrintable(error));
        QCOMPARE(book->title, QString("A Made-up Book"));
        QCOMPARE(book->author, QString("Ada Example"));
        QCOMPARE(book->format, QString("epub"));
        QCOMPARE(book->chapters.size(), 2);
        QCOMPARE(book->chapters[0].title, QString("The Beginning"));
        QCOMPARE(book->chapters[0].paragraphs.size(), 3); // the split file joins its chapter
        QCOMPARE(book->chapters[1].title, QString("The End"));
        QVERIFY(!QImage::fromData(book->cover).isNull());
    }

    void fb2Loads() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book.fb2");
        Fixtures::writeFb2(path);
        QString error;
        const auto book = BookLoader::load(path, &error);
        QVERIFY2(book, qPrintable(error));
        QCOMPARE(book->title, QString("Short Tale"));
        QCOMPARE(book->author, QString("Ivan Example"));
        QCOMPARE(book->chapters.size(), 2);
        QCOMPARE(book->chapters[0].title, QString("Part One"));
        QCOMPARE(book->chapters[0].paragraphs[1].text, QString("First words here."));
        QCOMPARE(book->chapters[0].paragraphs[2].level, 3);
        QCOMPARE(book->chapters[1].paragraphs.last().text, QString("Last words."));
    }

    void docxLoads() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book.docx");
        Fixtures::writeDocx(path);
        QString error;
        const auto book = BookLoader::load(path, &error);
        QVERIFY2(book, qPrintable(error));
        QCOMPARE(book->title, QString("Doc Title"));
        QCOMPARE(book->chapters.size(), 2);
        QCOMPARE(book->chapters[0].paragraphs[1].text, QString("Hello world."));
        QCOMPARE(book->chapters[1].paragraphs[1].text, QString("Goodbye."));
    }

    void odtLoads() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book.odt");
        Fixtures::writeOdt(path);
        QString error;
        const auto book = BookLoader::load(path, &error);
        QVERIFY2(book, qPrintable(error));
        QCOMPARE(book->title, QString("Odt Title"));
        QCOMPARE(book->chapters[0].title, QString("Heading"));
        QCOMPARE(book->chapters[0].paragraphs[1].text, QString("Two words and\na break."));
    }

    void pdfLoadsThroughPdftotext() {
        if (QStandardPaths::findExecutable("pdftotext").isEmpty())
            QSKIP("pdftotext is not installed");
        QTemporaryDir dir;
        const QString path = dir.filePath("book.pdf");
        {
            QPdfWriter writer(path);
            writer.setTitle("Pdf Title");
            QPainter painter(&writer);
            QFont font("sans-serif");
            font.setPointSize(14);
            painter.setFont(font);
            painter.drawText(QPointF(200, 400), "A sentence printed into a PDF.");
            writer.newPage();
            painter.drawText(QPointF(200, 400), "The second page.");
        }
        QString error;
        const auto book = BookLoader::load(path, &error);
        QVERIFY2(book, qPrintable(error));
        QCOMPARE(book->format, QString("pdf"));
        QCOMPARE(book->title, QString("Pdf Title"));
        QCOMPARE(book->chapters.size(), 1);
        QCOMPARE(book->chapters[0].paragraphs.size(), 2);
        QCOMPARE(book->chapters[0].paragraphs[0].text, QString("A sentence printed into a PDF."));
    }

    void loaderReportsProblems() {
        QString error;
        QVERIFY(!BookLoader::load("/nonexistent/book.epub", &error));
        QVERIFY(error.contains("does not exist"));
        QTemporaryDir dir;
        write(dir.filePath("x.cbz"), "PK");
        QVERIFY(!BookLoader::load(dir.filePath("x.cbz"), &error));
        QVERIFY(error.contains(".cbz"));
        write(dir.filePath("empty.txt"), "\n\n  \n");
        QVERIFY(!BookLoader::load(dir.filePath("empty.txt"), &error));
        QVERIFY(error.contains("No readable text"));
        write(dir.filePath("bad.epub"), "not a zip");
        QVERIFY(!BookLoader::load(dir.filePath("bad.epub"), &error));
    }

    void textDecodingFallsBackFromUtf8() {
        QCOMPARE(BookLoader::decodeText("caf\xc3\xa9"), QString("café"));
        QCOMPARE(BookLoader::decodeText("caf\xe9"), QString("café"));
        QCOMPARE(BookLoader::decodeText("\xef\xbb\xbfhi"), QString("hi"));
    }

    // Timing words to Kokoro's audio.

    void timestampsParse() {
        const auto words = Speech::parseTimestamps("word\tstart_sec\tend_sec\nHello\t0.000\t0.250\n,\t0.250\t0.300\nworld\t0.300\t0.700\n");
        QCOMPARE(words.size(), 3);
        QCOMPARE(words[2].word, QString("world"));
        QCOMPARE(words[2].start, 0.3);
    }

    void alignmentMatchesKokoroWords() {
        const QStringList text{"Don’t", "worry", "it", "was", "1984", "and", "Mr", "Smith", "read", "well-known", "books"};
        const auto spoken = Speech::parseTimestamps(
            "word\tstart_sec\tend_sec\nDon't\t0.000\t0.222\nworry\t0.222\t0.620\n:\t0.620\t0.800\nit\t0.800\t0.878\n"
            "was\t0.878\t1.039\n1984\t1.039\t2.409\n,\t2.409\t2.544\nand\t2.544\t2.664\nMr\t2.664\t3.336\n.\t3.336\t3.606\n"
            "Smith\t3.606\t3.920\nread\t3.920\t4.198\nwell\t4.198\t4.400\nknown\t4.400\t4.659\nbooks\t4.659\t5.330\n");
        const auto timings = Speech::align(text, spoken, 5.6);
        QCOMPARE(timings.size(), text.size());
        QCOMPARE(timings[0].start, 0.0);
        QCOMPARE(timings[4].start, 1.039);
        QCOMPARE(timings[7].start, 3.606);
        QCOMPARE(timings[9].start, 4.198); // "well" + "known" become one word
        QCOMPARE(timings[9].end, 4.659);
        QCOMPARE(timings[10].start, 4.659);
    }

    void alignmentInterpolatesSkippedWords() {
        const QStringList text{"one", "two", "three", "four"};
        const auto spoken = Speech::parseTimestamps("w\ts\te\none\t0\t1\nthree\t2\t3\nfour\t3\t4\n");
        const auto timings = Speech::align(text, spoken, 4);
        QCOMPARE(timings[1].start, 1.0);
        QVERIFY(timings[1].end <= 2.0 + 1e-9);
        QCOMPARE(timings[2].start, 2.0);
    }

    void alignmentFallsBackToEstimates() {
        const QStringList text{"alpha", "beta", "gamma"};
        const auto timings = Speech::align(text, {}, 3.0);
        QCOMPARE(timings.size(), 3);
        QCOMPARE(timings[0].start, 0.0);
        QVERIFY(timings[1].start > 0 && timings[2].start > timings[1].start);
        QVERIFY(qAbs(timings[2].end - 3.0) < 1e-9);
    }

    void wavParsesFloatAndStereoInt16() {
        Audio audio;
        QString error;
        QVERIFY2(Speech::parseWav(wav(24000, 1, {0.5f, -0.25f}), &audio, &error), qPrintable(error));
        QCOMPARE(audio.sampleRate, 24000);
        QCOMPARE(audio.frames(), 2);
        QCOMPARE(reinterpret_cast<const float *>(audio.samples.constData())[1], -0.25f);

        QByteArray pcm;
        for (qint16 s : {qint16(16384), qint16(0), qint16(-16384), qint16(-16384)})
            pcm.append(reinterpret_cast<const char *>(&s), 2);
        QVERIFY(Speech::parseWav(wavHeader(1, 2, 22050, 16, pcm.size()) + pcm, &audio, &error));
        QCOMPARE(audio.frames(), 2);
        QCOMPARE(reinterpret_cast<const float *>(audio.samples.constData())[0], 0.25f);
        QCOMPARE(reinterpret_cast<const float *>(audio.samples.constData())[1], -0.5f);

        QVERIFY(!Speech::parseWav("garbage", &audio, &error));
    }

    // The narration is mono, but its channel must be positioned: an unknown
    // layout reaches PipeWire as AUX0, which plays in one ear only.
    void narrationFormatIsPositionedMono() {
        for (const auto sampleFormat : {QAudioFormat::Float, QAudioFormat::Int16}) {
            const QAudioFormat format = AudioPlayer::narrationFormat(24000, sampleFormat);
            QCOMPARE(format.channelConfig(), QAudioFormat::ChannelConfigMono);
            QCOMPARE(format.channelCount(), 1);
            QCOMPARE(format.sampleRate(), 24000);
            QCOMPARE(format.sampleFormat(), sampleFormat);
            QCOMPARE(format.bytesPerFrame(), sampleFormat == QAudioFormat::Float ? 4 : 2);
            QCOMPARE(format.framesForDuration(1000000), qint64(24000));
        }
    }

    // Kokoros.

    void kokoroSetupSearchesOverrides() {
        QTemporaryDir dir;
        const QString koko = dir.filePath("koko");
        write(koko, "#!/bin/sh\n");
        QFile::setPermissions(koko, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        KokoroSetup setup = KokoroSetup::find(koko, dir.path());
        QVERIFY(!setup.ready());
        QVERIFY(setup.problem().contains("kokoro-v1.0.onnx"));
        write(dir.filePath("kokoro-v1.0.onnx"), "x");
        write(dir.filePath("voices-v1.0.bin"), "x");
        setup = KokoroSetup::find(koko, dir.path());
        QVERIFY(setup.ready());
        QVERIFY(setup.problem().isEmpty());
        QCOMPARE(setup.model, dir.filePath("kokoro-v1.0.onnx"));
        QVERIFY(!KokoroSetup::find(dir.filePath("missing"), dir.path()).ready());
    }

    void voicesMapToLanguages() {
        QCOMPARE(Voices::languageFor("af_heart"), QString("en-us"));
        QCOMPARE(Voices::languageFor("bm_george"), QString("en-gb"));
        QCOMPARE(Voices::languageFor("pf_dora"), QString("pt-br"));
        QCOMPARE(Voices::languageFor("zf_xiaobei"), QString("cmn"));
        QVERIFY(Voices::all().size() > 20);
        const QStringList arguments = KokoroRunner::arguments({{}, "pf_dora", 1.25}, "in.txt", "s_{line}.wav");
        QVERIFY(arguments.contains("--timestamps"));
        QCOMPARE(arguments.at(arguments.indexOf("--lan") + 1), QString("pt-br"));
        QCOMPARE(arguments.at(arguments.indexOf("--speed") + 1), QString("1.25"));
        QVERIFY(arguments.indexOf("file") < arguments.indexOf("--output"));
    }

    void runnerReportsSentencesFromKoko() {
        QTemporaryDir dir;
        const KokoroSetup setup = fakeKoko(dir);
        KokoroRunner runner;
        QSignalSpy ready(&runner, &KokoroRunner::sentenceReady);
        QSignalSpy failed(&runner, &KokoroRunner::sentenceFailed);
        QSignalSpy finished(&runner, &KokoroRunner::finished);
        QVERIFY(runner.start({setup, "af_heart", 1.0}, {{10, "Hello there."}, {11, "FAIL"}, {14, "Line\nwith break."}}));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first().at(0).toBool());
        QCOMPARE(ready.size(), 2);
        QCOMPARE(ready.at(0).at(0).toInt(), 10);
        QCOMPARE(ready.at(1).at(0).toInt(), 14);
        QVERIFY(QFile::exists(ready.at(1).at(1).toString()));
        QVERIFY(ready.at(1).at(2).toString().endsWith(".tsv"));
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.first().at(0).toInt(), 11);
    }

    void runnerReportsAMissingSetup() {
        KokoroRunner runner;
        QSignalSpy finished(&runner, &KokoroRunner::finished);
        QVERIFY(!runner.start({KokoroSetup{}, "af_heart", 1.0}, {{0, "Hi."}}));
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.first().at(0).toBool());
        QVERIFY(finished.first().at(1).toString().contains("omareader setup"));
    }

    void narratorHasNothingToPlayWhereNothingIsRead() {
        QTemporaryDir dir;
        Narrator narrator;
        narrator.setOverrides(fakeKoko(dir).binary, dir.path());
        Book book;
        book.chapters = {{"One", {{"https://example.com/only", 0}, {"1 A footnote.", 0, {}, true}}}};
        book.segment();
        narrator.setBook(std::make_shared<Book>(book));
        narrator.play(0);
        QCOMPARE(narrator.state(), Narrator::Stopped);
        narrator.play(1);
        QCOMPARE(narrator.state(), Narrator::Stopped);
        QVERIFY(narrator.error().isEmpty());
    }

    void narratorSaysWhatIsMissing() {
        QTemporaryDir dir;
        Narrator narrator;
        narrator.setOverrides(dir.filePath("no-koko"), dir.path());
        QVERIFY(!narrator.available());
        Book book;
        book.chapters = {{"One", {{"Hello world.", 0}}}};
        book.segment();
        narrator.setBook(std::make_shared<Book>(book));
        narrator.play(0);
        QCOMPARE(narrator.state(), Narrator::Stopped);
        QVERIFY(narrator.error().contains("koko"));
        narrator.setOverrides({}, {});
    }

    // The sleep timer.

    void sleepTimerCountsListening() {
        SleepTimer timer;
        QVERIFY(!timer.isOn());
        QCOMPARE(timer.msLeft(), -1);
        QVERIFY(!timer.overdue());

        timer.setMinutes(1);
        QVERIFY(timer.isOn());
        QCOMPARE(timer.minutes(), 1);
        QCOMPARE(timer.msLeft(), 60000);
        timer.listen(59000);
        timer.listen(-5);
        QCOMPARE(timer.msLeft(), 1000);
        timer.listen(1000);
        QCOMPARE(timer.msLeft(), 0);
        // A sentence may run on past the time, but not for long.
        timer.listen(SleepTimer::GraceMs - 1);
        QVERIFY(!timer.overdue());
        timer.listen(1);
        QVERIFY(timer.overdue());
        QCOMPARE(timer.msLeft(), 0);

        // Setting it again starts over; 0 turns it off.
        timer.setMinutes(15);
        QCOMPARE(timer.msLeft(), 15 * 60000);
        QVERIFY(!timer.overdue());
        timer.setMinutes(0);
        QVERIFY(!timer.isOn());

        timer.setChapterEnd();
        QVERIFY(timer.isOn());
        QCOMPARE(timer.minutes(), 0);
        QCOMPARE(timer.msLeft(), -1);
        timer.listen(10 * 3600 * 1000);
        QVERIFY(!timer.overdue());
        timer.cancel();
        QVERIFY(!timer.isOn());
    }

    void sleepTimerStopsTheStreamAtTheStartOfASentence() {
        // Three sentences of chapter 0, then two of chapter 1, a second
        // (100 frames at 100 Hz) apart.
        const int rate = 100;
        const QList<SleepTimer::Queued> queued{{0, 0}, {0, 100}, {0, 200}, {1, 300}, {1, 400}};
        SleepTimer timer;
        QCOMPARE(timer.stopFrame(queued, 0, rate), -1);

        // At the end of the chapter: where the next chapter starts, wherever
        // in the chapter the voice is.
        timer.setChapterEnd();
        QCOMPARE(timer.stopFrame(queued, 0, rate), 300);
        QCOMPARE(timer.stopFrame(queued, 250, rate), 300);
        // Set again where it stopped, it goes on to the end of the next one,
        // which isn't queued yet.
        QCOMPARE(timer.stopFrame(queued, 300, rate), -1);
        QCOMPARE(timer.stopFrame(queued + QList<SleepTimer::Queued>{{2, 500}}, 300, rate), 500);
        // A sentence that couldn't be spoken is never queued, so the chapter
        // still ends where the next one starts.
        QCOMPARE(timer.stopFrame({{0, 0}, {0, 100}, {1, 200}}, 50, rate), 200);
        // Nothing queued past the chapter yet: no stopping point yet.
        QCOMPARE(timer.stopFrame({{0, 0}, {0, 100}}, 50, rate), -1);
        QCOMPARE(timer.stopFrame({}, 0, rate), -1);

        // Counting minutes: the first sentence to start once the time is up,
        // so the one it runs out in is finished.
        timer.setMinutes(1);
        timer.listen(60000 - 1500);
        QCOMPARE(timer.stopFrame(queued, 0, rate), 200);
        QCOMPARE(timer.stopFrame(queued, 120, rate), 300);
        timer.listen(1500);
        QCOMPARE(timer.stopFrame(queued, 100, rate), 200);
        QCOMPARE(timer.stopFrame(queued, 200, rate), 300);
        // Ran out in the last sentence queued: wait for the next one.
        QCOMPARE(timer.stopFrame(queued, 450, rate), -1);
        timer.cancel();
        QCOMPARE(timer.stopFrame(queued, 0, rate), -1);
    }

    void audioPlayerWritesNothingPastItsLimit() {
        // Four bytes a frame; room for 100 frames, 80 pending.
        QCOMPARE(AudioPlayer::writable(400, 320, 0, -1, 4), qsizetype(320));
        QCOMPARE(AudioPlayer::writable(200, 320, 0, -1, 4), qsizetype(200));
        QCOMPARE(AudioPlayer::writable(400, 320, 10, 50, 4), qsizetype(160));
        QCOMPARE(AudioPlayer::writable(400, 320, 50, 50, 4), qsizetype(0));
        QCOMPARE(AudioPlayer::writable(400, 320, 60, 50, 4), qsizetype(0));
        // Whole frames only.
        QCOMPARE(AudioPlayer::writable(399, 400, 0, -1, 4), qsizetype(396));
        QCOMPARE(AudioPlayer::writable(0, 320, 0, -1, 4), qsizetype(0));
    }

    void narratorSleepTimerCountsOnlyWhilePlaying() {
        Narrator narrator;
        QSignalSpy changed(&narrator, &Narrator::sleepChanged);
        QSignalSpy asleep(&narrator, &Narrator::fellAsleep);
        Book book;
        book.chapters = {{"One", {{"First. Second.", 0}}}, {"Two", {{"Third. Fourth.", 0}}}};
        book.segment();
        narrator.setBook(std::make_shared<Book>(book));
        QCOMPARE(narrator.sleepSecondsLeft(), -1);

        narrator.sleepAfter(1);
        QVERIFY(narrator.sleepTimerOn());
        QCOMPARE(narrator.sleepMinutes(), 1);
        QVERIFY(!narrator.sleepAtChapterEnd());
        QCOMPARE(narrator.sleepSecondsLeft(), 60);
        QCOMPARE(changed.size(), 1);

        // Nothing goes by while paused.
        narrator.showForTests(Narrator::Paused, 0, 0);
        narrator.listenForTests(90000);
        QCOMPARE(narrator.sleepSecondsLeft(), 60);
        QCOMPARE(narrator.state(), Narrator::Paused);

        narrator.showForTests(Narrator::Playing, 0, 0);
        narrator.listenForTests(30500);
        QCOMPARE(narrator.sleepSecondsLeft(), 30);
        narrator.listenForTests(29500);
        QCOMPARE(narrator.sleepSecondsLeft(), 0);
        // With nothing queued to stop at, it waits for the sentence, but
        // not for ever.
        QCOMPARE(narrator.state(), Narrator::Playing);
        narrator.listenForTests(SleepTimer::GraceMs);
        QCOMPARE(narrator.state(), Narrator::Paused);
        QVERIFY(!narrator.sleepTimerOn());
        QCOMPARE(narrator.sleepSecondsLeft(), -1);
        QCOMPARE(asleep.size(), 1);

        narrator.sleepAtEndOfChapter();
        QVERIFY(narrator.sleepAtChapterEnd());
        QCOMPARE(narrator.sleepMinutes(), 0);
        QCOMPARE(narrator.sleepSecondsLeft(), -1);
        narrator.showForTests(Narrator::Playing, 0, 0);
        narrator.listenForTests(3600 * 1000);
        QCOMPARE(narrator.state(), Narrator::Playing);

        // Turning it off, or another book, ends it.
        narrator.cancelSleep();
        QVERIFY(!narrator.sleepTimerOn());
        narrator.sleepAfter(15);
        narrator.setBook(nullptr);
        QVERIFY(!narrator.sleepTimerOn());
        QCOMPARE(asleep.size(), 1);
    }

    // Voice settings for both engines.

    void voiceSettingsKeepKokoroAsTheDefault() {
        QSettings().remove("voice");
        QCOMPARE(VoiceSettings::selectedEngine(), Engine::Kokoro);
        const VoiceSettings kokoro = VoiceSettings::loadSelected();
        QCOMPARE(kokoro.engine, Engine::Kokoro);
        QCOMPARE(kokoro.voice, QString("af_heart"));
        QCOMPARE(kokoro.speed, 1.0);
        QCOMPARE(kokoro.number("volume"), 100.0);
        QCOMPARE(kokoro.number("paragraphPause"), 0.45);
        VoiceSettings::setSelectedEngine("espeak");
        QCOMPARE(VoiceSettings::selectedEngine(), Engine::Kokoro);
        VoiceSettings::setSelectedEngine(Engine::Qwen);
        const VoiceSettings qwen = VoiceSettings::loadSelected();
        QCOMPARE(qwen.engine, Engine::Qwen);
        QCOMPARE(qwen.voice, QString("ryan"));
        QCOMPARE(qwen.language, QString("English"));
        QCOMPARE(qwen.text("model"), QString("0.6b"));
        QCOMPARE(qwen.number("temperature"), 0.9);
        QSettings().remove("voice");
    }

    void voiceSettingsReadTheOldKokoroKeys() {
        QSettings stored;
        stored.remove("voice");
        // What versions before Qwen3-TTS saved.
        stored.setValue("voice/name", "pf_dora");
        stored.setValue("voice/speed", 1.25);
        VoiceSettings kokoro = VoiceSettings::load(Engine::Kokoro);
        QCOMPARE(kokoro.voice, QString("pf_dora"));
        QCOMPARE(kokoro.speed, 1.25);
        QCOMPARE(VoiceSettings::selectedEngine(), Engine::Kokoro);
        kokoro.speed = 1.5;
        kokoro.save();
        QCOMPARE(stored.value("voice/speed").toDouble(), 1.5);
        QCOMPARE(stored.value("voice/name").toString(), QString("pf_dora"));
        stored.setValue("voice/name", "zz_nobody");
        QCOMPARE(VoiceSettings::load(Engine::Kokoro).voice, QString("af_heart"));
        stored.remove("voice");
    }

    void voiceSettingsArePerEngineAndReset() {
        QSettings().remove("voice");
        VoiceSettings qwen = VoiceSettings::load(Engine::Qwen);
        QVERIFY(qwen.set("voice", "serena"));
        QVERIFY(qwen.set("language", "Portuguese"));
        QVERIFY(qwen.set("temperature", 0.6));
        QVERIFY(qwen.set("speed", "1.3"));
        qwen.save();
        VoiceSettings kokoro = VoiceSettings::load(Engine::Kokoro);
        QVERIFY(kokoro.set("volume", 120));
        kokoro.save();

        qwen = VoiceSettings::load(Engine::Qwen);
        QCOMPARE(qwen.voice, QString("serena"));
        QCOMPARE(qwen.language, QString("Portuguese"));
        QCOMPARE(qwen.number("temperature"), 0.6);
        QCOMPARE(qwen.speed, 1.3);
        QCOMPARE(qwen.number("volume"), 100.0); // Kokoro's volume is Kokoro's own
        QCOMPARE(VoiceSettings::load(Engine::Kokoro).number("volume"), 120.0);
        QCOMPARE(VoiceSettings::load(Engine::Kokoro).voice, QString("af_heart"));
        // Only what differs from the recommendation is stored.
        QVERIFY(QSettings().contains("voice/qwen/tuning/temperature"));
        QVERIFY(!QSettings().contains("voice/qwen/tuning/topP"));

        VoiceSettings::reset(Engine::Qwen);
        qwen = VoiceSettings::load(Engine::Qwen);
        QCOMPARE(qwen.voice, QString("serena")); // the voice is a choice, not tuning
        QCOMPARE(qwen.language, QString("Portuguese"));
        QCOMPARE(qwen.number("temperature"), 0.9);
        QCOMPARE(qwen.speed, 1.0);
        QCOMPARE(VoiceSettings::load(Engine::Kokoro).number("volume"), 120.0);
        QSettings().remove("voice");
    }

    void voiceSettingsRejectWhatTheEngineDoesNotSupport() {
        VoiceSettings qwen = VoiceSettings::recommended(Engine::Qwen);
        QString error;
        QVERIFY(!qwen.set("voice", "af_heart", &error));
        QVERIFY(error.contains("af_heart"));
        QVERIFY(!qwen.set("language", "Klingon", &error));
        QVERIFY(!qwen.set("blend", "af_bella", &error)); // a Kokoro setting
        QVERIFY(error.contains("blend"));
        QVERIFY(!qwen.set("temperature", 5, &error));
        QVERIFY(error.contains("0.3"));
        QVERIFY(!qwen.set("speed", 2.5, &error));
        QVERIFY(!qwen.set("model", "7b", &error));
        QVERIFY(error.contains("1.7b"));
        QVERIFY(qwen.set("temperature", 0.6234));
        QCOMPARE(qwen.number("temperature"), 0.6); // to the slider's step
        QVERIFY(qwen.set("paragraphPause", 0.45));
        QCOMPARE(qwen.number("paragraphPause"), 0.45);
        VoiceSettings kokoro = VoiceSettings::recommended(Engine::Kokoro);
        QVERIFY(!kokoro.set("language", "English", &error)); // Kokoro's language comes with its voice
        QVERIFY(!kokoro.set("temperature", 0.6, &error));
        QCOMPARE(VoiceSettings::clampSpeed(0.1), 0.5);
        QCOMPARE(VoiceSettings::clampSpeed(1.234), 1.25);
    }

    void enginesOfferOnlyTheirOwnVoicesAndLanguages() {
        QVERIFY(EngineCatalog::hasVoice(Engine::Kokoro, "af_heart"));
        QVERIFY(!EngineCatalog::hasVoice(Engine::Kokoro, "ryan"));
        QVERIFY(EngineCatalog::hasVoice(Engine::Qwen, "ryan"));
        QCOMPARE(EngineCatalog::voices(Engine::Qwen).size(), 9);
        QVERIFY(EngineCatalog::languages(Engine::Kokoro).isEmpty());
        QVERIFY(EngineCatalog::hasLanguage(Engine::Qwen, "Portuguese"));
        QVERIFY(EngineCatalog::hasLanguage(Engine::Qwen, "Auto"));
        QVERIFY(!EngineCatalog::hasLanguage(Engine::Qwen, "Hindi"));
        // Kokoro has no sampling knobs, and Qwen has no voice blending.
        QVERIFY(!Tuning::find(Tuning::parameters(Engine::Kokoro), "temperature"));
        QVERIFY(!Tuning::find(Tuning::parameters(Engine::Qwen), "blend"));
        for (const QString &engine : Engine::all()) {
            for (const char *shared : {"volume", "paragraphPause", "headingPause"})
                QVERIFY(Tuning::find(Tuning::parameters(engine), shared));
            for (const TuningParameter &p : Tuning::parameters(engine)) {
                bool ok = false;
                QCOMPARE(Tuning::normalize(p, p.recommended, &ok), p.recommended);
                QVERIFY2(ok, qPrintable(engine + "/" + p.key));
                QVERIFY(!p.label.isEmpty() && !p.description.isEmpty());
            }
        }
        QCOMPARE(EngineCatalog::previewLanguage(Engine::Kokoro, "pf_dora", {}), QString("pt"));
        QCOMPARE(EngineCatalog::previewLanguage(Engine::Qwen, "ryan", "Portuguese"), QString("pt"));
        QVERIFY(EngineCatalog::previewText("pt").contains(" "));
    }

    void everyVoiceStringHasAPortugueseTranslation() {
        QFile file(QFINDTESTDATA("../src/i18n/pt-BR.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject catalog = QJsonDocument::fromJson(file.readAll()).object();
        QStringList shown;
        for (const QString &engine : Engine::all()) {
            for (const TuningParameter &p : Tuning::parameters(engine)) {
                shown << p.label << p.description;
                if (p.key != "blend") // the other labels are voice names
                    shown << p.choiceLabels;
            }
            for (const EngineVoice &voice : EngineCatalog::voices(engine))
                if (!voice.description.isEmpty())
                    shown << voice.description;
            for (const EngineLanguage &language : EngineCatalog::languages(engine))
                shown << language.name;
        }
        shown << "No second voice";
        for (const QString &text : std::as_const(shown))
            QVERIFY2(catalog.contains(text), qPrintable("No pt-BR entry for: " + text));
    }

    // The settings and the export dialog say a lot; check each of their
    // strings so none ships in English to a Portuguese reader.
    void everySettingsStringHasAPortugueseTranslation() {
        QFile file(QString(OMAREADER_SOURCE) + "/src/i18n/pt-BR.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject catalog = QJsonDocument::fromJson(file.readAll()).object();
        static const QRegularExpression call("\\b(?:qsTr|tr)\\(\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
        int found = 0;
        for (const QString name : {"src/SettingsPanel.qml", "src/ExportDialog.qml", "src/ReaderView.qml",
                                   "src/NarrationBar.qml", "src/HelpOverlay.qml"}) {
            QFile source(QString(OMAREADER_SOURCE) + "/" + name);
            QVERIFY(source.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(source.readAll());
            for (auto it = call.globalMatch(text); it.hasNext();) {
                QString key = it.next().captured(1);
                key.replace("\\\"", "\"").replace("\\n", "\n");
                ++found;
                QVERIFY2(catalog.contains(key), qPrintable(name + ": no pt-BR entry for: " + key));
            }
        }
        QVERIFY(found > 20);
    }

    void kokoroBlendsVoicesInTenths() {
        VoiceSettings voice = VoiceSettings::recommended(Engine::Kokoro);
        QCOMPARE(KokoroSynthesizer::style(voice), QString("af_heart"));
        QVERIFY(voice.set("blend", "af_bella"));
        QCOMPARE(KokoroSynthesizer::style(voice), QString("af_heart.7+af_bella.3"));
        QVERIFY(voice.set("blendAmount", 60));
        QCOMPARE(KokoroSynthesizer::style(voice), QString("af_heart.4+af_bella.6"));
        QVERIFY(voice.set("blend", "af_heart")); // a voice blended with itself is itself
        QCOMPARE(KokoroSynthesizer::style(voice), QString("af_heart"));
    }

    void qwenRequestsCarryTheVoiceAndASteadySeed() {
        VoiceSettings voice = VoiceSettings::recommended(Engine::Qwen);
        QVERIFY(voice.set("voice", "serena"));
        QVERIFY(voice.set("language", "Portuguese"));
        QVERIFY(voice.set("temperature", 0.7));
        QVERIFY(voice.set("style", "calm narration"));
        QJsonObject request = QwenSynthesizer::voiceRequest(voice);
        QCOMPARE(request.value("speaker").toString(), QString("serena"));
        QCOMPARE(request.value("language").toString(), QString("Portuguese"));
        QCOMPARE(request.value("generation").toObject().value("temperature").toDouble(), 0.7);
        QCOMPARE(request.value("seed").toInt(), 1001);
        QVERIFY(!request.contains("instruct")); // the 0.6B model can't follow one
        QVERIFY(voice.set("model", "1.7b"));
        QVERIFY(voice.set("take", 3));
        request = QwenSynthesizer::voiceRequest(voice);
        QCOMPARE(request.value("instruct").toString(), QString("calm narration"));
        QCOMPARE(request.value("seed").toInt(), 1003);
        // Speed and volume are applied afterwards, so they never change the take.
        VoiceSettings faster = voice;
        faster.speed = 1.6;
        QVERIFY(faster.set("volume", 150));
        QCOMPARE(QwenSynthesizer::voiceRequest(faster), request);
    }

    void pausesFollowTheVoiceSettings() {
        QTemporaryDir dir;
        Book book = sampleBook(dir.filePath("a.txt"));
        book.chapters[1].paragraphs.prepend({"Heading", 1});
        book.segment();
        VoiceSettings voice = VoiceSettings::recommended(Engine::Qwen);
        QVERIFY(voice.set("paragraphPause", 1.2));
        QVERIFY(voice.set("headingPause", 2));
        QCOMPARE(Synthesis::pauseAfter(book, 0, voice), 0.0);   // "First." mid-paragraph
        QCOMPARE(Synthesis::pauseAfter(book, 1, voice), 1.2);   // "Second." ends it
        QCOMPARE(Synthesis::pauseAfter(book, 3, voice), 2.0);   // the heading
        QCOMPARE(Synthesis::pauseAfter(book, 99, voice), 0.0);
    }

    // Timing and the audio both engines deliver.

    void audioIsResampledStretchedAndAmplified() {
        Audio audio;
        audio.sampleRate = 12000;
        QList<float> tone;
        for (int i = 0; i < 12000; ++i)
            tone << 0.5f * float(std::sin(2 * M_PI * 220 * i / 12000.0));
        audio.samples = QByteArray(reinterpret_cast<const char *>(tone.constData()), tone.size() * 4);
        const Audio resampled = Speech::resample(audio, 24000);
        QCOMPARE(resampled.sampleRate, 24000);
        QVERIFY(qAbs(resampled.frames() - 24000) <= 1);

        const Audio faster = Speech::timeStretch(resampled, 2.0);
        QVERIFY2(qAbs(faster.seconds() - 0.5) < 0.03, qPrintable(QString::number(faster.seconds())));
        const Audio slower = Speech::timeStretch(resampled, 0.5);
        QVERIFY2(qAbs(slower.seconds() - 2.0) < 0.05, qPrintable(QString::number(slower.seconds())));
        QCOMPARE(Speech::timeStretch(resampled, 1.0).frames(), resampled.frames());
        // Stretching keeps the loudness: no silence and no blow-up.
        auto peak = [](const Audio &a) {
            const float *s = reinterpret_cast<const float *>(a.samples.constData());
            float p = 0;
            for (qint64 i = 0; i < a.frames(); ++i)
                p = std::max(p, std::abs(s[i]));
            return p;
        };
        QVERIFY(peak(faster) > 0.35f && peak(faster) < 0.65f);

        Audio louder = resampled;
        Speech::applyGain(&louder, 3.0);
        QCOMPARE(peak(louder), 1.0f); // clipped, never past full scale
        Audio quieter = resampled;
        Speech::applyGain(&quieter, 0.5);
        QVERIFY(qAbs(peak(quieter) - 0.25f) < 0.01f);
    }

    void qwenWordTimesFollowTheSpeed() {
        QTemporaryDir dir;
        if (QStandardPaths::findExecutable("python3").isEmpty())
            QSKIP("python3 is not installed");
        QwenWorker worker;
        QwenSynthesizer synth(nullptr, &worker);
        QSignalSpy ready(&synth, &Synthesizer::sentenceReady);
        QSignalSpy finished(&synth, &Synthesizer::finished);
        SynthesisOptions options{VoiceSettings::recommended(Engine::Qwen)};
        options.qwen = fakeQwen(dir);
        options.voice.speed = 2.0;
        QVERIFY(synth.start(options, {{7, "One two three four."}}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready.first().at(0).toInt(), 7);
        const SpokenSentence sentence = ready.first().at(1).value<SpokenSentence>();
        QCOMPARE(sentence.audio.sampleRate, Synthesizer::SampleRate);
        // Two seconds of 12 kHz audio, at twice the speed.
        QVERIFY2(qAbs(sentence.audio.seconds() - 1.0) < 0.05, qPrintable(QString::number(sentence.audio.seconds())));
        QCOMPARE(sentence.words.size(), 4);
        QCOMPARE(sentence.words.at(2).word, QString("three"));
        QCOMPARE(sentence.words.at(2).start, 0.5);
        QCOMPARE(sentence.words.at(2).end, 0.7);
        // And the highlight finds each word where it is heard.
        const QList<WordTiming> timings =
            Speech::align({"One", "two", "three", "four"}, sentence.words, sentence.audio.seconds());
        QCOMPARE(timings.size(), 4);
        QCOMPARE(timings.at(3).start, 0.75);
    }

    void qwenWorkerSharesOneProcessByPriority() {
        QTemporaryDir dir;
        if (QStandardPaths::findExecutable("python3").isEmpty())
            QSKIP("python3 is not installed");
        const QString log = dir.filePath("worker.log");
        qputenv("FAKE_QWEN_LOG", log.toUtf8());
        const QwenSetup setup = fakeQwen(dir);
        QwenWorker worker;
        QwenSynthesizer exporting(nullptr, &worker), listening(nullptr, &worker);
        QSignalSpy exported(&exporting, &Synthesizer::sentenceReady);
        QSignalSpy heard(&listening, &Synthesizer::sentenceReady);
        QSignalSpy exportDone(&exporting, &Synthesizer::finished);
        QSignalSpy listenDone(&listening, &Synthesizer::finished);
        SynthesisOptions background{VoiceSettings::recommended(Engine::Qwen)};
        background.qwen = setup;
        background.background = true;
        SynthesisOptions foreground = background;
        foreground.background = false;
        QList<Synthesizer::Line> lines;
        for (int i = 0; i < 12; ++i)
            lines.append({i, QString("Sentence %1.").arg(i)});
        QVERIFY(exporting.start(background, lines));
        QVERIFY(listening.start(foreground, {{100, "Listen to me."}, {101, "FAIL"}}));
        QTRY_COMPARE_WITH_TIMEOUT(exportDone.size(), 1, 20000);
        QCOMPARE(listenDone.size(), 1);
        QCOMPARE(heard.size(), 1);
        QCOMPARE(exported.size(), 12);
        unsetenv("FAKE_QWEN_LOG");

        QFile file(log);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QList<QByteArray> entries = file.readAll().split('\n');
        int processes = 0, firstListening = -1, lastExport = -1, index = 0;
        for (const QByteArray &line : entries) {
            const QJsonObject entry = QJsonDocument::fromJson(line).object();
            if (entry.contains("args"))
                ++processes;
            const QJsonArray items = entry.value("request").toObject().value("items").toArray();
            for (const QJsonValue &item : items) {
                const QString text = item.toObject().value("text").toString();
                if (text.startsWith("Listen") && firstListening < 0)
                    firstListening = index;
                if (text.startsWith("Sentence"))
                    lastExport = index;
            }
            if (!items.isEmpty())
                ++index;
        }
        QCOMPARE(processes, 1); // one model in memory for both
        // The listener jumped the export's queue rather than waiting for it.
        QVERIFY2(firstListening >= 0 && firstListening < lastExport,
                 qPrintable(QString("%1 %2").arg(firstListening).arg(lastExport)));
    }

    void qwenDropsStaleOutputAndCancels() {
        QTemporaryDir dir;
        if (QStandardPaths::findExecutable("python3").isEmpty())
            QSKIP("python3 is not installed");
        qputenv("FAKE_QWEN_DELAY", "0.2");
        QwenWorker worker;
        QwenSynthesizer synth(nullptr, &worker);
        QSignalSpy ready(&synth, &Synthesizer::sentenceReady);
        QSignalSpy finished(&synth, &Synthesizer::finished);
        SynthesisOptions options{VoiceSettings::recommended(Engine::Qwen)};
        options.qwen = fakeQwen(dir);
        QVERIFY(synth.start(options, {{1, "Old one."}, {2, "Old two."}}));
        QTRY_VERIFY_WITH_TIMEOUT(worker.isLoaded(), 15000);
        // A seek: the old sentences are already in flight, but must never arrive.
        QVERIFY(synth.start(options, {{50, "New one."}}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QTest::qWait(600);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready.first().at(0).toInt(), 50);

        // Stopping leaves nothing to arrive later, and no error.
        QVERIFY(synth.start(options, {{60, "Never heard."}, {61, "Nor this."}}));
        synth.stop();
        QVERIFY(!synth.isRunning());
        QTest::qWait(800);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(finished.size(), 1);
        unsetenv("FAKE_QWEN_DELAY");
    }

    void qwenCancelsStaleRequestsInTheWorker() {
        QTemporaryDir dir;
        if (QStandardPaths::findExecutable("python3").isEmpty())
            QSKIP("python3 is not installed");
        const QString log = dir.filePath("qwen.log");
        qputenv("FAKE_QWEN_LOG", log.toUtf8());
        // Each request takes two seconds unless the worker is told to stop.
        qputenv("FAKE_QWEN_DELAY", "2");
        QwenWorker worker;
        QwenSynthesizer synth(nullptr, &worker);
        QSignalSpy ready(&synth, &Synthesizer::sentenceReady);
        QSignalSpy finished(&synth, &Synthesizer::finished);
        SynthesisOptions options{VoiceSettings::recommended(Engine::Qwen)};
        options.qwen = fakeQwen(dir);
        QVERIFY(synth.start(options, {{1, "Old one."}, {2, "Old two."}, {3, "Old three."}}));
        QTRY_VERIFY_WITH_TIMEOUT(worker.isLoaded(), 15000);
        QTest::qWait(100);

        // A seek: both old requests are in the worker. Waiting them out would
        // take four seconds before the new sentence even started.
        QElapsedTimer clock;
        clock.start();
        QVERIFY(synth.start(options, {{50, "New one."}}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QVERIFY2(clock.elapsed() < 3500, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready.first().at(0).toInt(), 50);

        QFile file(log);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QList<qint64> requested, cancelled;
        for (const QByteArray &line : file.readAll().split('\n')) {
            const QJsonObject entry = QJsonDocument::fromJson(line).object();
            if (entry.contains("request"))
                requested.append(entry.value("request").toObject().value("req").toInteger());
            if (entry.contains("cancelled"))
                cancelled.append(entry.value("cancelled").toInteger());
        }
        QCOMPARE(requested.size(), 3);
        QCOMPARE(cancelled, (QList<qint64>{requested.at(0), requested.at(1)}));
        unsetenv("FAKE_QWEN_DELAY");
        unsetenv("FAKE_QWEN_LOG");
    }

    void qwenFailuresAreReportedNotSwitched() {
        QTemporaryDir dir;
        if (QStandardPaths::findExecutable("python3").isEmpty())
            QSKIP("python3 is not installed");
        QwenWorker worker;
        QwenSynthesizer synth(nullptr, &worker);
        QSignalSpy ready(&synth, &Synthesizer::sentenceReady);
        QSignalSpy finished(&synth, &Synthesizer::finished);
        SynthesisOptions options{VoiceSettings::recommended(Engine::Qwen)};
        options.qwen = fakeQwen(dir, "Qwen3-TTS-broken");
        QVERIFY(synth.start(options, {{0, "Hello."}}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QVERIFY(!finished.first().at(0).toBool());
        QVERIFY2(finished.first().at(1).toString().contains("broken"), qPrintable(finished.first().at(1).toString()));
        QCOMPARE(ready.size(), 0);
        QCOMPARE(synth.engine(), Engine::Qwen);

        // Without its setup, Qwen says how to install it and Kokoro is untouched.
        SynthesisOptions missing{VoiceSettings::recommended(Engine::Qwen)};
        QVERIFY(Synthesizer::problem(missing).contains("omareader setup --engine qwen"));
        missing.voice.set("model", "1.7b");
        missing.qwen.modelSize = "1.7b";
        QVERIFY(Synthesizer::problem(missing).contains("--model 1.7b"));
        SynthesisOptions kokoro{VoiceSettings::recommended(Engine::Kokoro)};
        kokoro.kokoro = fakeKoko(dir);
        QVERIFY(Synthesizer::problem(kokoro).isEmpty());
    }

    void qwenSetupCountsOnlyVerifiedFolders() {
        const QString data = qEnvironmentVariable("XDG_DATA_HOME");
        const QString models = data + "/omareader/models/qwen";
        const QString python = data + "/omareader/qwen/venv/bin/python";
        QVERIFY(QDir().mkpath(QFileInfo(python).path()));
        write(python, "#!/bin/sh\n");
        QFile::setPermissions(python, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        for (const QString &name : {QwenSetup::modelFolderName("0.6b"), QwenSetup::alignerFolderName()}) {
            QVERIFY(QDir().mkpath(models + "/" + name));
            write(models + "/" + name + "/model.safetensors", "x");
        }
        // Files alone, as a download stopped halfway leaves them, don't count.
        QwenSetup setup = QwenSetup::find();
        QVERIFY(!setup.ready());
        QVERIFY(setup.problem().contains("Python environment"));
        QVERIFY(setup.problem().contains("0.6B model"));
        write(data + "/omareader/qwen/venv/" + QwenSetup::verifiedMarker(), "lock");
        write(models + "/" + QwenSetup::modelFolderName("0.6b") + "/" + QwenSetup::verifiedMarker(), "rev");
        write(models + "/" + QwenSetup::alignerFolderName() + "/" + QwenSetup::verifiedMarker(), "rev");
        setup = QwenSetup::find("0.6b", "cpu");
        QVERIFY2(setup.ready(), qPrintable(setup.problem()));
        QCOMPARE(setup.device, QString("cpu"));
        QCOMPARE(setup.model, models + "/" + QwenSetup::modelFolderName("0.6b"));
        // The 1.7B model is a separate install.
        QVERIFY(QwenSetup::find("1.7b").problem().contains("--model 1.7b"));
        const QStringList arguments = QwenWorker::arguments({python, setup.model, setup.aligner, "cpu"}, "w.py");
        QCOMPARE(arguments.at(arguments.indexOf("--device") + 1), QString("cpu"));
        QCOMPARE(arguments.at(arguments.indexOf("--gpu-mib") + 1), QString::number(QwenHardware::neededMiB("0.6b")));
        QVERIFY(QDir(data + "/omareader").removeRecursively());
    }

    void qwenHardwareAdviceExplainsTheGpu() {
        QTemporaryDir dir;
        const QString smi = dir.filePath("nvidia-smi"), answer = dir.filePath("answer");
        write(smi, "#!/bin/sh\ncat '" + answer.toUtf8() + "'\n");
        QFile::setPermissions(smi, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        write(answer, "NVIDIA GeForce RTX 4050 Laptop GPU, 6141, 4380\n");
        QwenHardware hardware = QwenHardware::probe(smi);
        QVERIFY(hardware.nvidia);
        QCOMPARE(hardware.gpu, QString("NVIDIA GeForce RTX 4050 Laptop GPU"));
        QCOMPARE(hardware.totalMiB, 6141);
        QCOMPARE(hardware.freeMiB, 4380);
        QVERIFY(hardware.advice("0.6b").isEmpty());
        QVERIFY(hardware.advice("1.7b").contains("0.6B model fits"));
        write(answer, "Small GPU, 4096, 2000\n");
        hardware = QwenHardware::probe(smi);
        QVERIFY(hardware.advice("0.6b").contains("CPU"));
        QVERIFY(!QwenHardware::probe(dir.filePath("missing")).nvidia);
        QVERIFY(QwenHardware{}.advice("0.6b").contains("No NVIDIA GPU"));
        QCOMPARE(hardware.toJson("0.6b").value("neededMiB").toInt(), QwenHardware::neededMiB("0.6b"));
    }

    void narratorSwitchesEnginesAndKeepsEachOnesSettings() {
        QSettings().remove("voice");
        QTemporaryDir dir;
        Narrator narrator;
        QSignalSpy engineChanged(&narrator, &Narrator::engineChanged);
        QCOMPARE(narrator.engine(), Engine::Kokoro);
        QVERIFY(narrator.languages().isEmpty());
        narrator.setVoice("bf_emma");
        narrator.setSpeed(1.4);
        narrator.setEngine(Engine::Qwen);
        QCOMPARE(engineChanged.size(), 1);
        QCOMPARE(VoiceSettings::selectedEngine(), Engine::Qwen);
        QCOMPARE(narrator.voice(), QString("ryan"));
        QCOMPARE(narrator.speed(), 1.0);
        QCOMPARE(narrator.languages().size(), EngineCatalog::languages(Engine::Qwen).size());
        QCOMPARE(narrator.voices().first().toMap().value("id").toString(), QString("ryan"));
        // Qwen isn't installed in the tests' home: it says so, and doesn't
        // quietly read with Kokoro instead.
        QVERIFY(!narrator.available());
        QVERIFY(narrator.problem().contains("--engine qwen"));
        narrator.setVoice("af_heart"); // not a Qwen voice
        QCOMPARE(narrator.voice(), QString("ryan"));
        narrator.setEngine("espeak");
        QCOMPARE(narrator.engine(), Engine::Qwen);
        narrator.setEngine(Engine::Kokoro);
        QCOMPARE(narrator.voice(), QString("bf_emma"));
        QCOMPARE(narrator.speed(), 1.4);
        QSettings().remove("voice");
    }

    void narratorTuningAndResetFollowTheEngine() {
        QSettings().remove("voice");
        QTemporaryDir dir;
        Narrator narrator;
        QSignalSpy tuningChanged(&narrator, &Narrator::tuningChanged);
        QVERIFY(!narrator.tuned());
        auto find = [&](const QString &key) {
            for (const QVariant &item : narrator.tuning())
                if (item.toMap().value("key") == key)
                    return item.toMap();
            return QVariantMap();
        };
        QVERIFY(!find("blendAmount").value("visible").toBool()); // nothing to blend yet
        narrator.setTuning("blend", "af_bella");
        QVERIFY(find("blendAmount").value("visible").toBool());
        narrator.setTuning("volume", 130);
        QCOMPARE(find("volume").value("value").toDouble(), 130.0);
        QCOMPARE(find("volume").value("recommended").toDouble(), 100.0);
        narrator.setTuning("volume", 9999); // out of range: ignored
        QCOMPARE(find("volume").value("value").toDouble(), 130.0);
        QVERIFY(narrator.tuned());
        QVERIFY(tuningChanged.size() >= 2);
        narrator.setEngine(Engine::Qwen);
        QVERIFY(!narrator.tuned());
        QVERIFY(find("temperature").value("visible").toBool());
        QVERIFY(!find("style").value("visible").toBool()); // only the 1.7B model follows a style
        narrator.setTuning("model", "1.7b");
        QVERIFY(find("style").value("visible").toBool());
        QVERIFY(narrator.problem().contains("--model 1.7b"));
        narrator.setEngine(Engine::Kokoro);
        QVERIFY(narrator.tuned());
        narrator.resetTuning();
        QVERIFY(!narrator.tuned());
        QCOMPARE(narrator.voiceSettings().number("volume"), 100.0);
        QCOMPARE(narrator.voiceSettings().text("blend"), QString());
        QSettings().remove("voice");
    }

    void narratorPreviewsTheCurrentVoice() {
        QSettings().remove("voice");
        QTemporaryDir dir;
        Narrator narrator;
        const KokoroSetup setup = fakeKoko(dir);
        narrator.setOverrides(setup.binary, dir.path());
        QVERIFY(narrator.available());
        QSignalSpy previewChanged(&narrator, &Narrator::previewChanged);
        narrator.preview();
        QVERIFY(narrator.previewing());
        // It either plays (and ends) or this machine has no audio output; both end the preview.
        QTRY_VERIFY_WITH_TIMEOUT(!narrator.previewing(), 15000);
        QCOMPARE(previewChanged.size(), 2);
        narrator.preview();
        narrator.stopPreview();
        QVERIFY(!narrator.previewing());
        // A preview of an engine that isn't installed says why.
        narrator.setEngine(Engine::Qwen);
        narrator.preview();
        QVERIFY(!narrator.previewing());
        QVERIFY(narrator.error().contains("--engine qwen"));
        narrator.setOverrides({}, {});
        QSettings().remove("voice");
    }

    void narratorSaysTheRespellings() {
        QSettings().remove("voice");
        QTemporaryDir dir;
        const QString spoken = dir.filePath("spoken.txt");
        qputenv("FAKE_KOKO_TEXT", spoken.toUtf8());
        Lexicon lexicon;
        lexicon.set("Hermione", "Her-my-oh-nee");
        QVERIFY(lexicon.save());
        Narrator narrator;
        narrator.setOverrides(fakeKoko(dir).binary, dir.path());
        Book book;
        book.chapters = {{"One", {{"Hermione waved. Then she left.", 0}}}};
        book.segment();
        narrator.setBook(std::make_shared<Book>(book));
        // The export gets the pronunciations along with the voice.
        QCOMPARE(narrator.synthesisOptions().lexicon.entries().size(), 1);
        QFile file(spoken);
        auto heard = [&] {
            const QStringList lines = file.open(QIODevice::ReadOnly)
                                          ? QString::fromUtf8(file.readAll()).trimmed().split('\n') : QStringList();
            file.close();
            file.remove();
            return lines;
        };
        // A respelling is heard as it is, before it is saved.
        narrator.previewText("  Her-my-oh-nee ");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(spoken).size() > 0, 10000);
        narrator.stopPreview();
        QCOMPARE(heard(), QStringList{"Her-my-oh-nee"});
        narrator.play(0);
        const bool playing = narrator.state() != Narrator::Stopped; // no audio output here otherwise
        if (playing)
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(spoken).size() > 0, 10000);
        narrator.stop();
        unsetenv("FAKE_KOKO_TEXT");
        QFile::remove(Lexicon::defaultFile());
        narrator.setOverrides({}, {});
        QSettings().remove("voice");
        if (!playing)
            QSKIP("There is no audio output to read aloud through.");
        QCOMPARE(heard(), (QStringList{"Her-my-oh-nee waved.", "Then she left."}));
    }

    void narratorHandsExportsACopyOfItsVoice() {
        QSettings().remove("voice");
        Narrator narrator;
        narrator.setTuning("volume", 140);
        SynthesisOptions copy = narrator.synthesisOptions();
        narrator.setTuning("volume", 60);
        narrator.setSpeed(1.8);
        QCOMPARE(copy.voice.number("volume"), 140.0);
        QCOMPARE(copy.voice.speed, 1.0);
        QCOMPARE(narrator.synthesisOptions().voice.number("volume"), 60.0);
        QSettings().remove("voice");
    }

    // Exporting the book as audio.

    void exportPlanSkipsChaptersWithNothingToSay() {
        Book book;
        book.chapters = {{"Cover", {}}, {"", {{"Hello there. Again.", 0}}}, {"Two", {{"Bye.", 0}}}};
        book.segment();
        const QList<Exporter::Track> tracks = Exporter::plan(book);
        QCOMPARE(tracks.size(), 2);
        QCOMPARE(tracks.at(0).chapter, 1);
        QCOMPARE(tracks.at(0).title, QString("Chapter 2"));
        QCOMPARE(tracks.at(0).sentences.size(), 2);
        QCOMPARE(tracks.at(0).words, 3);
        QCOMPARE(Exporter::plan(book, 2, 2).size(), 1);
        QCOMPARE(Exporter::plan(book, 2, 2).first().title, QString("Two"));
    }

    void exportNamesAreSafeForPhones() {
        QCOMPARE(Exporter::safeName("Step One: To Get Started / Evaluated?"), QString("Step One To Get Started Evaluated"));
        QCOMPARE(Exporter::safeName("...hidden. "), QString("hidden"));
        QCOMPARE(Exporter::safeName("  "), QString("Audiobook"));
        QVERIFY(Exporter::safeName(QString(200, 'a')).size() <= 80);
        QCOMPARE(Exporter::trackFileName(3, 12, "Down the Rabbit-Hole", "mp3"), QString("03 Down the Rabbit-Hole.mp3"));
        QCOMPARE(Exporter::trackFileName(7, 120, "", "m4a"), QString("007 Chapter 7.m4a"));
        QCOMPARE(Exporter::extension(Exporter::M4b, Exporter::SingleFile), QString("m4b"));
        QCOMPARE(Exporter::extension(Exporter::M4b, Exporter::ChapterFiles), QString("m4a"));
    }

    void exportNeverReplacesExistingFiles() {
        QTemporaryDir dir;
        const QString file = dir.filePath("Book.mp3");
        QCOMPARE(Exporter::uniquePath(file, false), file);
        write(file, "x");
        QCOMPARE(Exporter::uniquePath(file, false), dir.filePath("Book (2).mp3"));
        write(dir.filePath("Book (2).mp3"), "x");
        QCOMPARE(Exporter::uniquePath(file, false), dir.filePath("Book (3).mp3"));
        QVERIFY(QDir(dir.path()).mkdir("Book"));
        QCOMPARE(Exporter::uniquePath(dir.filePath("Book"), true), dir.filePath("Book (2)"));
    }

    void exportChapterMetadataMarksEveryChapter() {
        const QString metadata = Exporter::chapterMetadata("A = B; #1", "Ann", {"One", "Two\\Three"}, {24000, 12000}, 24000);
        QVERIFY(metadata.startsWith(";FFMETADATA1\n"));
        QVERIFY(metadata.contains("title=A \\= B\\; \\#1\n"));
        QVERIFY(metadata.contains("artist=Ann\n"));
        QVERIFY(metadata.contains("TIMEBASE=1/24000\nSTART=0\nEND=24000\ntitle=One\n"));
        QVERIFY(metadata.contains("START=24000\nEND=36000\ntitle=Two\\\\Three\n"));
        QCOMPARE(metadata.count("[CHAPTER]"), 2);
    }

    void exportArgumentsFollowTheFormat() {
        const QStringList mp3 = Exporter::encodeArguments(Exporter::Mp3, 24000, "cover.jpg", {{"title", "One"}, {"album", "Book"}}, "out.mp3");
        QCOMPARE(mp3.at(mp3.indexOf("-ar") + 1), QString("24000"));
        QCOMPARE(mp3.at(mp3.indexOf("-c:a") + 1), QString("libmp3lame"));
        QVERIFY(mp3.contains("attached_pic"));
        QVERIFY(mp3.indexOf("album=Book") < mp3.indexOf("title=One"));
        QVERIFY(mp3.contains("-n")); // never overwrite
        QCOMPARE(mp3.last(), QString("out.mp3"));
        const QStringList m4b = Exporter::joinArguments(Exporter::M4b, "parts.txt", "chapters.txt", {}, "book.m4b");
        QCOMPARE(m4b.at(m4b.indexOf("-c:a") + 1), QString("copy"));
        QCOMPARE(m4b.at(m4b.indexOf("-map_chapters") + 1), QString("1"));
        QCOMPARE(m4b.at(m4b.indexOf("-f", m4b.indexOf("-map_chapters")) + 1), QString("mp4"));
        QVERIFY(!m4b.contains("attached_pic"));
    }

    void exportSaysWhatIsMissing() {
        QTemporaryDir dir;
        Exporter exporter;
        QSignalSpy finished(&exporter, &Exporter::finished);
        Exporter::Options options;
        options.folder = dir.filePath("out");
        options.ffmpeg = dir.filePath("no-ffmpeg");
        auto book = std::make_shared<Book>(sampleBook(dir.filePath("a.txt")));

        QVERIFY(!exporter.start(book, options));
        QVERIFY(exporter.error().contains("omareader setup"));
        options.synthesis.kokoro = fakeKoko(dir);
        QVERIFY(!exporter.start(book, options));
        QVERIFY(exporter.error().contains("ffmpeg"));
        QVERIFY(!exporter.start(std::make_shared<Book>(), options));
        QVERIFY(exporter.error().contains("nothing to read"));
        QCOMPARE(finished.size(), 3);
        QVERIFY(!exporter.running());
    }

    void exportReportsAnEncoderFailureAndCleansUp() {
        QTemporaryDir dir;
        const QString ffmpeg = dir.filePath("ffmpeg");
        write(ffmpeg, "#!/bin/sh\ncat > /dev/null\necho 'no encoder here' >&2\nexit 1\n");
        QFile::setPermissions(ffmpeg, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        Exporter exporter;
        QSignalSpy finished(&exporter, &Exporter::finished);
        Exporter::Options options;
        options.synthesis.kokoro = fakeKoko(dir);
        options.ffmpeg = ffmpeg;
        options.folder = dir.filePath("out");
        QVERIFY(exporter.start(std::make_shared<Book>(sampleBook(dir.filePath("a.txt"))), options));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first().at(0).toBool());
        QVERIFY(exporter.error().contains("no encoder here"));
        QVERIFY(QDir(options.folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    }

    void exportWritesOneFileWithChapters() {
        if (Exporter::findFfmpeg().isEmpty())
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        Exporter exporter;
        QSignalSpy finished(&exporter, &Exporter::finished);
        QSignalSpy progress(&exporter, &Exporter::progressChanged);
        Exporter::Options options;
        options.synthesis.kokoro = fakeKoko(dir);
        options.folder = dir.filePath("out");
        options.format = Exporter::M4b;
        QVERIFY(exporter.start(std::make_shared<Book>(sampleBook(dir.filePath("a.txt"))), options));
        QVERIFY(exporter.running());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QCOMPARE(exporter.result(), dir.filePath("out/Sample.m4b"));
        QCOMPARE(exporter.progress(), 1.0);
        QVERIFY(progress.size() > 2);
        QCOMPARE(QDir(options.folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
                 QStringList{"Sample.m4b"});

        const QString ffprobe = QStandardPaths::findExecutable("ffprobe");
        if (!ffprobe.isEmpty()) {
            QProcess probe;
            probe.start(ffprobe, {"-v", "error", "-show_chapters", "-of", "json", exporter.result()});
            QVERIFY(probe.waitForFinished());
            const QJsonArray chapters = QJsonDocument::fromJson(probe.readAllStandardOutput()).object().value("chapters").toArray();
            QCOMPARE(chapters.size(), 2);
            QCOMPARE(chapters.at(1).toObject().value("tags").toObject().value("title").toString(), QString("Two"));
        }

        // A second export never replaces the first.
        QVERIFY(exporter.start(std::make_shared<Book>(sampleBook(dir.filePath("a.txt"))), options));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 20000);
        QCOMPARE(exporter.result(), dir.filePath("out/Sample (2).m4b"));
    }

    void exportWritesAFolderWithAFilePerChapter() {
        if (Exporter::findFfmpeg().isEmpty())
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        Exporter exporter;
        QSignalSpy finished(&exporter, &Exporter::finished);
        Exporter::Options options;
        options.synthesis.kokoro = fakeKoko(dir);
        options.folder = dir.filePath("out");
        options.layout = Exporter::ChapterFiles;
        QVERIFY(exporter.start(std::make_shared<Book>(sampleBook(dir.filePath("a.txt"))), options));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QCOMPARE(exporter.result(), dir.filePath("out/Sample"));
        QCOMPARE(QDir(exporter.result()).entryList(QDir::Files), (QStringList{"01 One.mp3", "02 Two.mp3"}));
        QCOMPARE(QDir(options.folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
                 QStringList{"Sample"});
        QVERIFY(QFileInfo(exporter.result()).permissions() & QFile::ReadOther);
    }

    void exportCancelLeavesNothingBehind() {
        QTemporaryDir dir;
        Exporter exporter;
        QSignalSpy finished(&exporter, &Exporter::finished);
        Exporter::Options options;
        options.synthesis.kokoro = fakeKoko(dir);
        options.ffmpeg = QStandardPaths::findExecutable("sh"); // never reached
        options.folder = dir.filePath("out");
        QVERIFY(exporter.start(std::make_shared<Book>(sampleBook(dir.filePath("a.txt"))), options));
        exporter.cancel();
        QVERIFY(!exporter.running());
        QCOMPARE(finished.size(), 1);
        QVERIFY(exporter.error().isEmpty()); // cancelling is not an error
        QVERIFY(QDir(options.folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    }

    void exportReadsTheTextWithoutItsClutter() {
        if (Exporter::findFfmpeg().isEmpty())
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        const QString spoken = dir.filePath("spoken.txt");
        qputenv("FAKE_KOKO_TEXT", spoken.toUtf8());
        write(dir.filePath("a.txt"), "clutter");
        Book book;
        book.path = dir.filePath("a.txt");
        book.title = "Clutter";
        book.format = "pdf";
        book.chapters = {{"One",
                          {{"It helps.1 Read more at https://example.com/x today [4].", 0},
                           {"1 A note the voice skips.", 0, {}, true},
                           {"The end.", 0}}}};
        book.segment();
        Exporter exporter;
        QSignalSpy finished(&exporter, &Exporter::finished);
        Exporter::Options options;
        options.synthesis.kokoro = fakeKoko(dir);
        options.folder = dir.filePath("out");
        QVERIFY(exporter.start(std::make_shared<Book>(book), options));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        unsetenv("FAKE_KOKO_TEXT");
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QFile file(spoken);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(file.readAll()).trimmed().split('\n'),
                 (QStringList{"It helps.", "Read more at today.", "The end."}));
    }

    void exportSaysTheRespellings() {
        if (Exporter::findFfmpeg().isEmpty())
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        const QString spoken = dir.filePath("spoken.txt");
        qputenv("FAKE_KOKO_TEXT", spoken.toUtf8());
        write(dir.filePath("a.txt"), "names");
        Book book;
        book.path = dir.filePath("a.txt");
        book.title = "Names";
        book.chapters = {{"One", {{"Kubectl talks to Kubernetes.", 0}, {"The end.", 0}}}};
        book.segment();
        Exporter exporter;
        QSignalSpy finished(&exporter, &Exporter::finished);
        Exporter::Options options;
        options.synthesis.kokoro = fakeKoko(dir);
        options.synthesis.lexicon.set("kubectl", "cube control");
        options.folder = dir.filePath("out");
        QVERIFY(exporter.start(std::make_shared<Book>(book), options));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        unsetenv("FAKE_KOKO_TEXT");
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QFile file(spoken);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(file.readAll()).trimmed().split('\n'),
                 (QStringList{"cube control talks to Kubernetes.", "The end."}));
    }

    void exportKeepsTheSettingsItStartedWith() {
        if (Exporter::findFfmpeg().isEmpty())
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        const QString log = dir.filePath("koko.log");
        qputenv("FAKE_KOKO_LOG", log.toUtf8());
        Exporter exporter;
        QSignalSpy finished(&exporter, &Exporter::finished);
        Exporter::Options options;
        options.synthesis.kokoro = fakeKoko(dir);
        QVERIFY(options.synthesis.voice.set("voice", "pf_dora"));
        QVERIFY(options.synthesis.voice.set("speed", 1.5));
        QVERIFY(options.synthesis.voice.set("paragraphPause", 0.0));
        QVERIFY(options.synthesis.voice.set("headingPause", 0.0));
        options.folder = dir.filePath("out");
        options.layout = Exporter::ChapterFiles;
        QVERIFY(exporter.start(std::make_shared<Book>(sampleBook(dir.filePath("a.txt"))), options));
        // Changing the voice now, for the reader or the next export, doesn't
        // reach the chapters still to come.
        options.synthesis.voice.speed = 0.5;
        VoiceSettings saved = VoiceSettings::load(Engine::Kokoro);
        saved.speed = 0.5;
        saved.save();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        unsetenv("FAKE_KOKO_LOG");
        QFile file(log);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QStringList runs = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(runs.size(), 2); // one run per chapter
        for (const QString &run : runs) {
            QVERIFY2(run.contains("--speed 1.5"), qPrintable(run));
            QVERIFY2(run.contains("--style pf_dora"), qPrintable(run));
        }
        QSettings().remove("voice");
    }

    void exportRemembersItsChoices() {
        {
            Exporter exporter;
            QVERIFY(exporter.folder().endsWith("Audiobooks"));
            exporter.setLayout(Exporter::ChapterFiles);
            exporter.setFormat(Exporter::M4b);
            exporter.setFolder("/tmp/somewhere");
        }
        Exporter exporter;
        QCOMPARE(exporter.layout(), int(Exporter::ChapterFiles));
        QCOMPARE(exporter.format(), int(Exporter::M4b));
        QCOMPARE(exporter.folder(), QString("/tmp/somewhere"));
        exporter.setFormat(42);
        QCOMPARE(exporter.format(), int(Exporter::M4b));
    }

    void readerExportsTheOpenBook() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.json"), dir.filePath("covers"));
        Reader reader(&library, nullptr, nullptr);
        Exporter exporter;
        reader.setExporter(&exporter);
        reader.exportAudio(); // no book: nothing happens
        QVERIFY(!exporter.running());
        QVERIFY(exporter.error().isEmpty());
        reader.openBook(std::make_shared<Book>(sampleBook(dir.filePath("a.txt"))));
        QCOMPARE(reader.audioMinutes(1.0), 1);
        reader.exportAudio(); // the tests' home has no model, so it says so
        QVERIFY(exporter.error().contains("omareader setup"));
    }

    // Searching the open book.

    void foldingForSearchKeepsEveryOffset() {
        const QString text = QString::fromUtf8("Ação “Olá” — don’t\u00a0Ünïcode ÇA");
        QList<int> offsets;
        const QString folded = Text::fold(text, &offsets);
        QCOMPARE(folded, QString("acao \"ola\" - don't unicode ca"));
        QCOMPARE(offsets.size(), folded.size());
        for (int i = 0; i < offsets.size(); ++i)
            QCOMPARE(offsets.at(i), i);
        // Accents kept as separate marks are dropped too, and the letters
        // still point at where they are in the text.
        const QString marked = QString::fromUtf8("re\u0301sume\u0301");
        QCOMPARE(Text::fold(marked, &offsets), QString("resume"));
        QCOMPARE(offsets, (QList<int>{0, 1, 3, 4, 5, 6}));
        const QList<QPair<int, int>> found = Text::find(marked, Text::fold(marked, &offsets), offsets, "resume");
        QCOMPARE(found, (QList<QPair<int, int>>{{0, int(marked.size())}}));
        // Korean syllables come apart into their letters, which tell them
        // apart, and a match can't start or end inside one.
        const QString ga = QString::fromUtf8("가"), go = QString::fromUtf8("고"), gak = QString::fromUtf8("각");
        QVERIFY(Text::fold(ga) != Text::fold(go));
        QVERIFY(Text::find(go, Text::fold(go, &offsets), offsets, Text::fold(ga)).isEmpty());
        QVERIFY(Text::find(gak, Text::fold(gak, &offsets), offsets, Text::fold(ga)).isEmpty());
        QCOMPARE(Text::find(ga, Text::fold(ga, &offsets), offsets, Text::fold(ga)).size(), 1);
        // A ligature has no one-letter form, so it stays as it is.
        QCOMPARE(Text::fold(QString::fromUtf8("ﬁne")), QString::fromUtf8("ﬁne"));
        // Vowel signs in other scripts are part of the word.
        QVERIFY(Text::fold(QString::fromUtf8("कि")) != Text::fold(QString::fromUtf8("क")));
    }

    void searchFindsEveryMatchAndStepsThroughThem() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.json"), dir.filePath("covers"));
        Reader reader(&library, nullptr, nullptr);
        reader.search("anything"); // no book: nothing happens
        QCOMPARE(reader.searchCount(), 0);

        const QString path = dir.filePath("search.txt");
        write(path, "search");
        Book book;
        book.path = path;
        book.title = "Search";
        book.format = "txt";
        book.chapters = {{"One", {{"The café opened early. Nobody came to the CAFE that day.", 0, {}},
                                  {QString::fromUtf8("Her résumé and 고 and 각."), 0, {}}}},
                         {"Two", {{"A quiet chapter.", 0, {}}, {"Later, the Café closed for good.", 0, {}}}},
                         {"Three", {{"No match here at all.", 0, {}}}}};
        book.segment();
        reader.openBook(std::make_shared<Book>(book));
        QSignalSpy searched(&reader, &Reader::searchChanged);

        // Case and accents don't matter, and spaces around the words are dropped.
        reader.search("  Cafe ");
        QCOMPARE(searched.count(), 1);
        QCOMPARE(reader.searchQuery(), QString("Cafe"));
        QCOMPARE(reader.searchCount(), 3);
        QCOMPARE(reader.searchIndex(), -1);
        QVERIFY(reader.match().isEmpty());
        const QVariantMap first = reader.searchResults().first().toMap();
        QCOMPARE(first.value("chapter").toInt(), 0);
        QCOMPARE(first.value("start").toInt(), 4);
        QCOMPARE(first.value("end").toInt(), 8);
        QCOMPARE(first.value("chapterTitle").toString(), QString("One"));
        QCOMPARE(first.value("before").toString(), QString("The "));
        QCOMPARE(first.value("text").toString(), QString::fromUtf8("café"));
        QVERIFY(first.value("after").toString().startsWith(" opened early."));
        const QVariantMap third = reader.searchResults().at(2).toMap();
        QCOMPARE(third.value("chapter").toInt(), 1);
        QCOMPARE(third.value("paragraph").toInt(), 1);
        QCOMPARE(third.value("start").toInt(), 11);
        // Asking again for the same thing changes nothing.
        reader.search("Cafe");
        QCOMPARE(searched.count(), 1);

        // The first match after the reading place, which is now in chapter two.
        reader.goToChapter(1);
        reader.nextMatch();
        QCOMPARE(reader.searchIndex(), 2);
        QCOMPARE(reader.chapter(), 1);
        QCOMPARE(reader.match().value("paragraph").toInt(), 1);
        QCOMPARE(reader.match().value("start").toInt(), 11);
        QCOMPARE(reader.match().value("end").toInt(), 15);
        // The reading place is the match's sentence, so reading goes on from there.
        QCOMPARE(reader.sentence(), book.sentenceAt(1, 1, 11));
        // Stepping on wraps around to the first, and back again to the last.
        reader.nextMatch();
        QCOMPARE(reader.searchIndex(), 0);
        QCOMPARE(reader.chapter(), 0);
        reader.previousMatch();
        QCOMPARE(reader.searchIndex(), 2);
        reader.previousMatch();
        QCOMPARE(reader.searchIndex(), 1);
        QCOMPARE(reader.match().value("start").toInt(), 42);

        // Putting the mark away keeps the search and the place in it.
        reader.hideMatch();
        QVERIFY(reader.match().isEmpty());
        QCOMPARE(reader.searchIndex(), 1);
        QCOMPARE(reader.searchCount(), 3);
        reader.nextMatch();
        QCOMPARE(reader.searchIndex(), 2);
        reader.showMatch(7); // past the end: nothing happens
        QCOMPARE(reader.searchIndex(), 2);

        // Once the reading has moved on, the next match is the first after
        // where it is now, not the one after the match shown.
        reader.showMatch(0);
        reader.goTo(1, 1, 0);
        reader.nextMatch();
        QCOMPARE(reader.searchIndex(), 2);
        reader.showMatch(0);
        reader.goTo(2, 0, 0);
        reader.nextMatch();
        QCOMPARE(reader.searchIndex(), 0); // none after it, so round to the first
        reader.goTo(2, 0, 0);
        reader.previousMatch();
        QCOMPARE(reader.searchIndex(), 2);

        // Before the reading place, with none shown yet, steps back.
        reader.search("chapter");
        QCOMPARE(reader.searchCount(), 1);
        reader.goToChapter(2);
        reader.previousMatch();
        QCOMPARE(reader.chapter(), 1);

        // Accents as separate marks, and Korean syllables kept apart.
        reader.search("resume");
        QCOMPARE(reader.searchCount(), 1);
        QCOMPARE(reader.searchResults().first().toMap().value("text").toString(),
                 QString::fromUtf8("résumé"));
        reader.search(QString::fromUtf8("가"));
        QCOMPARE(reader.searchCount(), 0);
        reader.search(QString::fromUtf8("고"));
        QCOMPARE(reader.searchCount(), 1);

        reader.search("nowhere to be found");
        QCOMPARE(reader.searchCount(), 0);
        QVERIFY(reader.searchResults().isEmpty());
        reader.nextMatch(); // nothing to step to
        QCOMPARE(reader.searchIndex(), -1);
        reader.search("   ");
        QCOMPARE(reader.searchQuery(), QString());

        // Another book, or none, starts with no search.
        reader.search("cafe");
        reader.nextMatch();
        reader.closeBook();
        QCOMPARE(reader.searchCount(), 0);
        QCOMPARE(reader.searchQuery(), QString());
        QVERIFY(reader.match().isEmpty());
        QCOMPARE(reader.searchIndex(), -1);
    }

    void searchListsTheFirstMatchesButStepsThroughAll() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.json"), dir.filePath("covers"));
        Reader reader(&library, nullptr, nullptr);
        const QString path = dir.filePath("many.txt");
        write(path, "many");
        Book book;
        book.path = path;
        book.title = "Many";
        book.format = "txt";
        QList<Paragraph> paragraphs;
        for (int i = 0; i < 300; ++i)
            paragraphs.append({"Echo and echo again.", 0, {}});
        book.chapters = {{"Echoes", paragraphs}};
        book.segment();
        reader.openBook(std::make_shared<Book>(book));
        reader.search("echo");
        QCOMPARE(reader.searchCount(), 600);
        QCOMPARE(reader.searchResults().size(), Reader::SearchListLimit);
        reader.previousMatch(); // from the top, back round to the very last
        QCOMPARE(reader.searchIndex(), 599);
        QCOMPARE(reader.match().value("paragraph").toInt(), 299);
        QCOMPARE(reader.match().value("start").toInt(), 9);
    }

    // The library and the reading position.

    void libraryRemembersBooksAndPositions() {
        QTemporaryDir dir;
        const QString file = dir.filePath("library.json");
        Book book = sampleBook(dir.filePath("a.txt"));
        {
            Library library(file, dir.filePath("covers"));
            library.opened(book);
            library.savePosition(book.path, 1, 0, 3, book.sentenceCount());
        }
        Library library(file, dir.filePath("covers"));
        QCOMPARE(library.count(), 1);
        const LibraryEntry entry = library.entry(book.path);
        QCOMPARE(entry.title, QString("Sample"));
        QCOMPARE(entry.chapter, 1);
        QCOMPARE(entry.sentence, 3);
        QCOMPARE(library.books().first().toMap().value("progress").toDouble(), 1.0);
        library.remove(book.path);
        QCOMPARE(library.count(), 0);
        QVERIFY(QFile::exists(book.path)); // forgetting a book never deletes it
    }

    void readerFindsItsPlaceByTextWhenParagraphsMove() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.json"), dir.filePath("covers"));
        Book before = sampleBook(dir.filePath("a.txt"));
        before.chapters = {{"One", {{"Opening words.", 0}, {"A running head", 0}, {"The place we stopped. More.", 0}}}};
        before.segment();
        library.opened(before);
        {
            Reader reader(&library, nullptr, nullptr);
            reader.openBook(std::make_shared<Book>(before));
            reader.goTo(0, 2, 22);
            reader.closeBook();
        }
        QCOMPARE(library.entry(before.path).anchor, QString("More."));
        // A newer reading of the same book drops the running head.
        Book after = before;
        after.chapters = {{"One", {{"Opening words.", 0}, {"The place we stopped. More.", 0}}}};
        after.segment();
        Reader reader(&library, nullptr, nullptr);
        reader.openBook(std::make_shared<Book>(after));
        QCOMPARE(reader.sentence(), after.sentenceAt(0, 1, 22));
        QCOMPARE(after.sentenceText(reader.sentence()), QString("More."));
    }

    void readerResumesWhereReadingStopped() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.json"), dir.filePath("covers"));
        Book book = sampleBook(dir.filePath("a.txt"));
        library.opened(book);
        library.savePosition(book.path, 1, 0, 3, book.sentenceCount());

        Reader reader(&library, nullptr, nullptr);
        reader.openBook(std::make_shared<Book>(book));
        QVERIFY(reader.hasBook());
        QCOMPARE(reader.chapter(), 1);
        QCOMPARE(reader.sentence(), 3);
        QCOMPARE(reader.paragraphs().size(), 1);
        QCOMPARE(reader.chapterTitles(), (QStringList{"One", "Two"}));

        reader.goToChapter(0);
        QCOMPARE(reader.sentence(), 0);
        reader.setVisibleParagraph(1);
        QCOMPARE(reader.sentence(), 2);
        reader.saveNow();
        QCOMPARE(library.entry(book.path).sentence, 2);
        reader.closeBook();
        QVERIFY(!reader.hasBook());
    }

    // Bookmarks, highlights and notes.

    void annotationsMergeKeepsTheLatestChange() {
        auto make = [](const QString &id, const QString &note, int second, bool deleted = false) {
            Annotation a;
            a.id = id;
            a.kind = "highlight";
            a.note = note;
            a.modified = QDateTime::fromSecsSinceEpoch(1767225600 + second, QTimeZone::utc());
            a.deleted = deleted;
            return a;
        };
        const QList<Annotation> here{make("a", "old", 1), make("b", "mine", 5), make("c", "kept", 3), make("e", "same", 7)};
        const QList<Annotation> there{make("a", "new", 2), make("b", "theirs", 4), make("c", "kept", 3, true),
                                      make("d", "only there", 1), make("e", "same, elsewhere", 7)};
        auto byId = [](const QList<Annotation> &list) {
            QMap<QString, Annotation> map;
            for (const Annotation &a : list)
                map.insert(a.id, a);
            return map;
        };
        const auto merged = byId(Annotations::merge(here, there));
        QCOMPARE(merged.size(), 5);
        QCOMPARE(merged["a"].note, QString("new"));         // the later change
        QCOMPARE(merged["b"].note, QString("mine"));        // ours is later
        QVERIFY(merged["c"].deleted);                       // a removal at the same moment wins
        QCOMPARE(merged["d"].note, QString("only there"));  // new to us
        QCOMPARE(merged["e"].note, QString("same"));        // a tie keeps what is here

        // Either computer comes to the same removals and latest changes.
        const auto other = byId(Annotations::merge(there, here));
        for (const QString id : {"a", "b", "c", "d"}) {
            QCOMPARE(other[id].note, merged[id].note);
            QCOMPARE(other[id].deleted, merged[id].deleted);
        }
    }

    void annotationsFingerprintFollowsTheFile() {
        QTemporaryDir dir;
        write(dir.filePath("a.txt"), "The same words.");
        write(dir.filePath("elsewhere.txt"), "The same words.");
        write(dir.filePath("b.txt"), "Other words.");
        const QString a = Annotations::fingerprint(dir.filePath("a.txt"));
        QCOMPARE(a.size(), 32);
        QCOMPARE(Annotations::fingerprint(dir.filePath("elsewhere.txt")), a);
        QVERIFY(Annotations::fingerprint(dir.filePath("b.txt")) != a);
        QVERIFY(Annotations::fingerprint(dir.filePath("missing.txt")).isEmpty());
    }

    void annotationsKeepHighlightsBookmarksAndNotes() {
        QTemporaryDir dir;
        auto book = std::make_shared<Book>(notedBook(dir.filePath("book.txt")));
        const QString first = book->chapters.at(0).paragraphs.at(0).text;
        auto at = [&](const QString &word) { return int(first.indexOf(word)); };
        const QString folder = dir.filePath("annotations");
        QString wider, across;
        {
            Annotations notes(folder, "here");
            notes.setBook(book);
            QCOMPARE(notes.count(), 0);

            // A drag that starts and ends inside words takes in both whole words.
            const QString id = notes.addHighlight(0, at("bright") + 2, 0, at("cold") + 2);
            QVERIFY(!id.isEmpty());
            QCOMPARE(notes.item(id).value("text").toString(), QString("bright cold"));
            QCOMPARE(notes.highlightAt(0, at("cold")), id);
            QVERIFY(notes.highlightAt(0, at("day")).isEmpty());
            // Only spaces is nothing to highlight.
            QVERIFY(notes.addHighlight(0, at("bright") - 1, 0, at("bright")).isEmpty());

            notes.setNote(id, "  The weather.  ");
            QCOMPARE(notes.item(id).value("note").toString(), QString("The weather."));
            const QVariantList marks = notes.chapterMarks().at(0).toMap().value("marks").toList();
            QCOMPARE(marks.size(), 1);
            QCOMPARE(marks.first().toMap().value("start").toInt(), at("bright"));
            QCOMPARE(marks.first().toMap().value("end").toInt(), at("cold") + 4);
            QVERIFY(marks.first().toMap().value("note").toBool());

            // A highlight over it folds it in, note and all.
            wider = notes.addHighlight(0, at("a bright"), 0, at("day") + 3);
            QCOMPARE(notes.item(wider).value("text").toString(), QString("a bright cold day"));
            QCOMPARE(notes.item(wider).value("note").toString(), QString("The weather."));
            QVERIFY(notes.item(id).isEmpty());
            QCOMPARE(notes.count(), 1);

            // One can run on into the next paragraph.
            across = notes.addHighlight(1, 7, 0, at("striking"));
            QCOMPARE(notes.item(across).value("text").toString(), QString("striking thirteen.\nAnother"));
            QCOMPARE(notes.chapterMarks().at(1).toMap().value("marks").toList().size(), 1);

            // A bookmark marks the start of the sentence, in the margin of its paragraph.
            QVERIFY(!notes.toggleBookmark(0, 0, at("clocks")).isEmpty());
            QVERIFY(notes.hasBookmark(0, 0));
            QVERIFY(notes.chapterMarks().at(0).toMap().value("bookmark").toBool());
            QCOMPARE(notes.count(), 3);
            const QVariantList items = notes.items();
            QCOMPARE(items.at(0).toMap().value("kind").toString(), QString("highlight"));
            QCOMPARE(items.at(1).toMap().value("kind").toString(), QString("bookmark"));
            QVERIFY(items.at(1).toMap().value("text").toString().startsWith("The clocks were striking"));
            QCOMPARE(items.at(1).toMap().value("chapterTitle").toString(), QString("One"));
        }

        // Still there after a restart, readable only by their owner.
        const QString file = folder + "/" + Annotations::fingerprint(book->path) + ".json";
        QCOMPARE(QFileInfo(file).permissions() & (QFile::ReadOther | QFile::ReadGroup), QFileDevice::Permissions());
        Annotations again(folder, "here");
        again.setBook(book);
        QCOMPARE(again.count(), 3);
        QCOMPARE(again.item(wider).value("note").toString(), QString("The weather."));

        // A second press removes the bookmark; removing a highlight takes its note.
        QVERIFY(again.toggleBookmark(0, 0, 0).isEmpty());
        QVERIFY(!again.hasBookmark(0, 0));
        again.remove(across);
        QCOMPARE(again.count(), 1);
        // What was removed stays in the file, marked so, for the other computers.
        const QList<Annotation> saved = Annotations::read(file);
        QCOMPARE(saved.size(), 4);
        QCOMPARE(std::count_if(saved.begin(), saved.end(), [](const Annotation &a) { return a.deleted; }), 3);

        // Closing the book lets them go.
        again.setBook(nullptr);
        QCOMPARE(again.count(), 0);
        QVERIFY(again.chapterMarks().isEmpty());
    }

    void annotationsFindTheirTextAgain() {
        QTemporaryDir dir;
        const Book book = notedBook(dir.filePath("book.txt"));
        const QString folder = dir.filePath("annotations");
        QString highlight, bookmark;
        {
            Annotations notes(folder, "here");
            notes.setBook(std::make_shared<Book>(book));
            const QString first = book.chapters.at(0).paragraphs.at(0).text;
            highlight = notes.addHighlight(0, int(first.indexOf("striking")), 0, int(first.indexOf("thirteen")) + 3);
            bookmark = notes.toggleBookmark(1, 0, 0);
            QVERIFY(!highlight.isEmpty() && !bookmark.isEmpty());
        }

        // The same file read differently, as after a change to how its
        // format is parsed: a chapter and a paragraph now come first.
        Book moved = book;
        moved.chapters[0].paragraphs.prepend({"A foreword nobody asked for.", 0, {}});
        moved.chapters.prepend({"Preface", {{"Some preface text.", 0, {}}}});
        moved.segment();
        Annotations notes(folder, "here");
        notes.setBook(std::make_shared<Book>(moved));
        QVariantMap found = notes.item(highlight);
        QVERIFY(found.value("placed").toBool());
        QCOMPARE(found.value("chapter").toInt(), 1);
        QCOMPARE(found.value("paragraph").toInt(), 1);
        QCOMPARE(Annotations::passage(moved, 1, 1, found.value("start").toInt(), 1, found.value("end").toInt()),
                 QString("striking thirteen"));
        found = notes.item(bookmark);
        QCOMPARE(found.value("chapter").toInt(), 2);
        QCOMPARE(found.value("paragraph").toInt(), 0);

        // Text that is gone is still listed, but marks nothing.
        Book changed = book;
        changed.chapters[0].paragraphs[0].text = "Different words entirely.";
        changed.segment();
        Annotations lost(folder, "here");
        lost.setBook(std::make_shared<Book>(changed));
        QCOMPARE(lost.count(), 2);
        QVERIFY(!lost.item(highlight).value("placed").toBool());
        QVERIFY(!lost.chapterMarks().at(0).toMap().contains("marks"));
        QVERIFY(lost.highlightAt(0, 0).isEmpty());
    }

    void annotationsSyncThroughAFolder() {
        QSettings().remove("annotations");
        QTemporaryDir dir;
        const QString sync = dir.filePath("Sync");

        // Notes made before syncing go into the folder once it is chosen.
        Annotations tablet(dir.filePath("tablet"), "tablet");
        QVERIFY(tablet.syncFolder().isEmpty());
        Book other = sampleBook(dir.filePath("other.txt"));
        tablet.setBook(std::make_shared<Book>(other));
        tablet.toggleBookmark(0, 0, 0);
        tablet.setBook(nullptr);
        tablet.setSyncFolder(sync);
        const QString otherFolder = sync + "/omareader/" + Annotations::fingerprint(other.path);
        QCOMPARE(Annotations::read(otherFolder + "/tablet.json").size(), 1);

        // Two computers, each with its own copy of the book in its own place.
        QDir().mkpath(dir.filePath("desk"));
        auto here = std::make_shared<Book>(notedBook(dir.filePath("book.txt")));
        auto there = std::make_shared<Book>(notedBook(dir.filePath("desk/book.txt")));
        Annotations laptop(dir.filePath("laptop"), "laptop");
        Annotations desktop(dir.filePath("desktop"), "desktop");
        QCOMPARE(laptop.syncFolder(), QDir::cleanPath(sync)); // remembered
        laptop.setBook(here);
        desktop.setBook(there);
        // Opening a book without notes leaves nothing in the folder.
        const QString folder = sync + "/omareader/" + Annotations::fingerprint(here->path);
        QVERIFY(!QFileInfo::exists(folder));

        const QString id = laptop.addHighlight(0, 0, 0, 6);
        laptop.setNote(id, "From the laptop.");
        QTRY_COMPARE(desktop.item(id).value("note").toString(), QString("From the laptop."));
        QTRY_COMPARE(desktop.syncedDevices(), 1);
        QTRY_COMPARE(laptop.syncedDevices(), 1);
        // Each computer writes only its own file, so the sync tool never has two to choose from.
        QCOMPARE(QDir(folder).entryList({"*.json"}, QDir::Files, QDir::Name), QStringList({"desktop.json", "laptop.json"}));
        // And keeps it on this computer too, for when the folder isn't there.
        QCOMPARE(Annotations::read(dir.filePath("desktop") + "/" + Annotations::fingerprint(there->path) + ".json").size(), 1);

        // A removal on one reaches the other.
        desktop.toggleBookmark(0, 1, 0);
        QTRY_VERIFY(laptop.hasBookmark(0, 1));
        desktop.remove(id);
        QTRY_VERIFY(laptop.item(id).isEmpty());
        QCOMPARE(laptop.count(), 1);

        // Stopping leaves the notes on each computer.
        laptop.stopSyncing();
        QVERIFY(laptop.syncFolder().isEmpty());
        QCOMPARE(laptop.count(), 1);
        QSettings().remove("annotations");
    }

    void readerBookmarksTheReadingPlace() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.json"), dir.filePath("covers"));
        Annotations notes(dir.filePath("annotations"), "here");
        Reader reader(&library, nullptr, nullptr);
        reader.setAnnotations(&notes);
        QSignalSpy changed(&reader, &Reader::bookmarkedChanged);
        reader.openBook(std::make_shared<Book>(notedBook(dir.filePath("book.txt"))));
        QVERIFY(!reader.bookmarked());

        reader.setVisibleParagraph(1);
        reader.toggleBookmark();
        QVERIFY(reader.bookmarked());
        QVERIFY(notes.hasBookmark(0, 1));
        QVERIFY(changed.count() > 0);

        // Going to a place moves the reading position there.
        reader.goTo(1, 0, 0);
        QCOMPARE(reader.chapter(), 1);
        QVERIFY(!reader.bookmarked());
        reader.goTo(0, 1, 0);
        QCOMPARE(reader.chapter(), 0);
        QCOMPARE(reader.anchorParagraph(), 1);
        QVERIFY(reader.bookmarked());
        reader.toggleBookmark();
        QVERIFY(!reader.bookmarked());

        // A passage reads the same whichever way it was dragged.
        QCOMPARE(reader.passage(0, 9, 0, 15), QString("bright"));
        QCOMPARE(reader.passage(0, 15, 0, 9), QString("bright"));
        QCOMPARE(reader.passage(0, 60, 1, 7), QString("thirteen.\nAnother"));
        reader.closeBook();
        QCOMPARE(notes.count(), 0);
    }

    // Text pasted into the library.

    void pastedTitleIsTheFirstWords() {
        QCOMPARE(PastedTitle::fromText("  Short   note.\n"), QString("Short note."));
        QCOMPARE(PastedTitle::fromText("One two three four five six seven eight nine ten"),
                 QString("One two three four five six seven eight…"));
        QCOMPARE(PastedTitle::fromText("Café com leite,\n\npão de queijo e mais coisas boas para o lanche"),
                 QString("Café com leite, pão de queijo e mais…"));
        QCOMPARE(PastedTitle::fromText("Hello, world, again", 2), QString("Hello, world…"));
        const QString longWord = QString(200, 'x');
        QCOMPARE(PastedTitle::fromText(longWord).size(), 60);
        QVERIFY(PastedTitle::fromText(longWord).endsWith(QChar(0x2026)));
        QVERIFY(PastedTitle::fromText(" \n\t ").isEmpty());
    }

    void agentRepliesBecomeBareTitles() {
        QCOMPARE(PastedTitle::fromAgentReply("\n  The Arctic Tern's Journey\n"), QString("The Arctic Tern's Journey"));
        QCOMPARE(PastedTitle::fromAgentReply("Title: \"Arctic Terns\"\nThey fly far."), QString("Arctic Terns"));
        QCOMPARE(PastedTitle::fromAgentReply("# **Título:** “Pão de Queijo”."), QString("Pão de Queijo"));
        QCOMPARE(PastedTitle::fromAgentReply("`Notes on Qt`"), QString("Notes on Qt"));
        QVERIFY(PastedTitle::fromAgentReply("\n \n").isEmpty());
        QVERIFY(PastedTitle::fromAgentReply("\"\"").isEmpty());
        QCOMPARE(PastedTitle::fromAgentReply(QString(300, 'a')).size(), 100);
    }

    void libraryKeepsPastedText() {
        QTemporaryDir dir;
        const QString file = dir.filePath("library.json");
        const QString text = "Olá, mundo! Ünïcödé text.\nSame paragraph.\n\n日本語の段落。\n\nThird paragraph.";
        QString path;
        {
            Library library(file, dir.filePath("covers"));
            QVERIFY(library.addText("Notes", "  \n\t ").isEmpty());
            QVERIFY(library.addText("Notes", {}).isEmpty());
            QCOMPARE(library.count(), 0);
            QVERIFY(!QFileInfo::exists(library.textDirectory()) || QDir(library.textDirectory()).isEmpty());

            path = library.addText("  My   notes ", text);
            QVERIFY(!path.isEmpty());
            QVERIFY(library.isText(path));
            QCOMPARE(QFileInfo(path).permissions() & (QFile::ReadOther | QFile::ReadGroup), QFileDevice::Permissions());
            QCOMPARE(library.count(), 1);
            QCOMPARE(library.entry(path).title, QString("My notes"));
            QCOMPARE(library.books().first().toMap().value("format").toString(), QString("TEXT"));
            QVERIFY(!library.books().first().toMap().value("missing").toBool());
        }

        // Still there after a restart, word for word.
        Library library(file, dir.filePath("covers"));
        QCOMPARE(library.count(), 1);
        QCOMPARE(library.entry(path).title, QString("My notes"));
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(saved.readAll()).object().value("text").toString(), text);

        QString error;
        const auto book = BookLoader::load(path, &error);
        QVERIFY2(book, qPrintable(error));
        QCOMPARE(book->title, QString("My notes"));
        QCOMPARE(book->format, QString("text"));
        QCOMPARE(book->chapters.size(), 1);
        QCOMPARE(book->chapters.first().title, QString("My notes"));
        const auto &paragraphs = book->chapters.first().paragraphs;
        QCOMPARE(paragraphs.size(), 3);
        QCOMPARE(paragraphs.at(0).text, QString("Olá, mundo! Ünïcödé text. Same paragraph."));
        QCOMPARE(paragraphs.at(1).text, QString("日本語の段落。"));
        QVERIFY(book->sentenceCount() >= 4);

        // Without a title, the first words name it.
        const QString untitled = library.addText(" ", "First words of a message\nthat goes on");
        QCOMPARE(library.entry(untitled).title, QString("First words of a message that goes on"));

        // Forgetting pasted text moves it to the trash rather than deleting it.
        // The trash lives in ~/.local/share, which a real home already has.
        const QString trash = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash/files";
        QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
        library.remove(path);
        QVERIFY(!library.contains(path));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(trash + "/" + QFileInfo(path).fileName()));
    }

    void loaderTurnsAwayOtherJson() {
        QTemporaryDir dir;
        write(dir.filePath("data.json"), R"({"text": "not ours"})");
        QString error;
        QVERIFY(!BookLoader::load(dir.filePath("data.json"), &error));
        QVERIFY(error.contains(".json"));
    }

    void readerAddsPastedTextAndResumesIt() {
        QTemporaryDir dir;
        const QString file = dir.filePath("library.json");
        QString path;
        {
            Library library(file, dir.filePath("covers"));
            Reader reader(&library, nullptr, nullptr);
            QVERIFY(!reader.addText("Empty", "\n \n"));
            QCOMPARE(library.count(), 0);
            QVERIFY(reader.error().isEmpty());

            QVERIFY(reader.addText("Letter", "Dear friend. How are you?\n\nI am well. Thank you."));
            QTRY_VERIFY(reader.hasBook());
            path = reader.path();
            QCOMPARE(reader.title(), QString("Letter"));
            QCOMPARE(reader.paragraphs().size(), 2);
            reader.setVisibleParagraph(1);
            QCOMPARE(reader.sentence(), 2);
            reader.saveNow();
            reader.closeBook();
        }

        Library library(file, dir.filePath("covers"));
        QCOMPARE(library.entry(path).sentence, 2);
        QCOMPARE(library.entry(path).sentenceCount, 4);
        Reader reader(&library, nullptr, nullptr);
        reader.open(path);
        QTRY_VERIFY(reader.hasBook());
        QCOMPARE(reader.title(), QString("Letter"));
        QCOMPARE(reader.sentence(), 2);
    }

    // Web pages, fetched once and read like pasted text. Every page here is
    // made up and served from this computer.

    void articleKeepsTheStoryAndDropsTheRest() {
        const Article article = ArticleExtractor::extract(Fixtures::articlePage(),
                                                          QUrl("https://www.fieldjournal.example/2026/terns"),
                                                          "text/html; charset=utf-8");
        QCOMPARE(article.title, QString("Why Terns Fly So Far"));
        QCOMPARE(article.author, QString("Ana Ribeiro"));
        QCOMPARE(article.site, QString("The Field Journal"));
        // The heading that repeats the title goes, as the reader shows the title.
        QVERIFY2(article.text.startsWith("Every year the Arctic tern"), qPrintable(article.text));
        for (const char *kept : {"Every year the Arctic tern", "**prevailing winds**", "*S-shaped*", "## Following the wind",
                                 "three trips to the Moon", "They see two summers every year.", "`tern_tracks.csv`"})
            QVERIFY2(article.text.contains(QString::fromUtf8(kept)), qPrintable(QString("lost: ") + kept + "\n" + article.text));
        // The site around the article, and anything that would run or load.
        for (const char *dropped : {"cookies", "Accept all", "About us", "binoculars", "Most read", "gulls", "Great article",
                                    "Related stories", "Penguins", "Copyright", "Privacy", "Share on", "track(", "color:",
                                    "loadComments", "tern.jpg", "http", "By Ana"})
            QVERIFY2(!article.text.contains(QString::fromUtf8(dropped)),
                     qPrintable(QString("kept: ") + dropped + "\n" + article.text));

        // It reads back as the reader shows it: a heading, styled words and
        // the list's items as paragraphs of their own.
        const auto chapters = BookLoader::markdownChapters(article.text, false);
        QList<Paragraph> paragraphs;
        for (const Chapter &chapter : chapters)
            paragraphs += chapter.paragraphs;
        QStringList texts;
        for (const Paragraph &p : paragraphs)
            texts << p.text;
        QVERIFY2(texts.contains("Following the wind"), qPrintable(texts.join(" | ")));
        QCOMPARE(paragraphs.at(texts.indexOf("Following the wind")).level, 2);
        QVERIFY(texts.contains("They spend more time in daylight than any other animal."));
        const Paragraph &winds = paragraphs.at(1);
        QCOMPARE(paragraphs.first().level, 0);
        QVERIFY2(winds.text.startsWith("The birds do not fly"), qPrintable(winds.text));
        QVERIFY(!winds.styles.isEmpty());
        QCOMPARE(winds.text.mid(winds.styles.first().start, winds.styles.first().length), QString("prevailing winds"));
        QCOMPARE(winds.styles.first().flags, int(TextStyle::Bold));
    }

    void articleTitleFallsBackToTheFirstWords() {
        const Article article = ArticleExtractor::extract(Fixtures::untitledPage(), QUrl("https://harbour.example/day"));
        QVERIFY2(article.text.startsWith("Rain fell over the harbour all morning"), qPrintable(article.text));
        QCOMPARE(article.title, PastedTitle::fromText(article.text));
        QVERIFY(article.title.startsWith("Rain fell over the harbour"));
        QCOMPARE(article.site, QString("harbour.example"));

        // Plain text comes whole, its marks kept as they are.
        const QString plain = "1. Not a list but a date\n\n*Not italic* and a_b_c\n\n# not a heading";
        const Article text = ArticleExtractor::extract(plain.toUtf8(), QUrl("https://notes.example/a.txt"), "text/plain");
        QStringList texts;
        for (const Chapter &chapter : BookLoader::markdownChapters(text.text, false))
            for (const Paragraph &p : chapter.paragraphs) {
                texts << p.text;
                QVERIFY(p.styles.isEmpty());
                QCOMPARE(p.level, 0);
            }
        QCOMPARE(texts, (QStringList{"1. Not a list but a date", "*Not italic* and a_b_c", "# not a heading"}));
        QCOMPARE(text.title, PastedTitle::fromText(text.text));
        QVERIFY(text.title.startsWith("1. Not a list but a date"));
        // Markdown is taken as Markdown.
        const Article markdown = ArticleExtractor::extract("# Notes\n\nSome *words*.", QUrl("https://notes.example/a"),
                                                           "text/markdown; charset=utf-8");
        QCOMPARE(markdown.text, QString("# Notes\n\nSome *words*."));
    }

    void articleTitleDropsTheSiteName() {
        using ArticleExtractor::cleanTitle;
        QCOMPARE(cleanTitle("Why Terns Fly | The Field Journal", "The Field Journal"), QString("Why Terns Fly"));
        QCOMPARE(cleanTitle("The Field Journal - Why Terns Fly", "The Field Journal"), QString("Why Terns Fly"));
        QCOMPARE(cleanTitle("Arctic tern - Wikipedia", {}, "en.wikipedia.org"), QString("Arctic tern"));
        QCOMPARE(cleanTitle("  Arctic   tern  ", {}, "example.com"), QString("Arctic tern"));
        // A title that only has a dash in it keeps it.
        QCOMPARE(cleanTitle("Rock - Paper", {}, "games.example"), QString("Rock - Paper"));
    }

    void articleReadsItsMetadataFromThePage() {
        // JSON-LD names the author when the page shows none, and its name
        // wins over a headline that doesn't match the title.
        const QByteArray page =
            "<html><head><title>Arctic tern - Encyclopedia</title>"
            "<script type=\"application/ld+json\">{\"@type\":\"Article\",\"name\":\"Arctic tern\","
            "\"headline\":\"species of seabird\",\"author\":[{\"@type\":\"Person\",\"name\":\"Lee Moss\"}]}</script>"
            "</head><body><div class=\"article\">"
            "<p>The Arctic tern is a tern in the family Laridae. It breeds in the Arctic and sub-Arctic regions.</p>"
            "<p>It sees two summers each year as it migrates along a convoluted route from its northern breeding grounds.</p>"
            "</div></body></html>";
        const Article article = ArticleExtractor::extract(page, QUrl("https://encyclopedia.example/wiki/Arctic_tern"));
        QCOMPARE(article.title, QString("Arctic tern"));
        QCOMPARE(article.author, QString("Lee Moss"));
        QCOMPARE(article.site, QString("encyclopedia.example"));
    }

    void articleEscapesMarkdownSoItReadsBack() {
        for (const QString &line : {QString("1. Not a list"), QString("# not a heading"), QString("*not italic*"),
                                    QString("snake_case_name and __init__"), QString("[a link](https://x.example)"),
                                    QString("- not an item"), QString("> not a quote"), QString("2 * 3 * 4 = 24"),
                                    QString("`not code`"), QString("a <b>tag</b> &amp; more"), QString("~~kept~~"),
                                    QString("C:\\path\\to\\file")}) {
            const QString escaped = ArticleExtractor::escapeMarkdown(line);
            const auto chapters = BookLoader::markdownChapters(escaped, false);
            QCOMPARE(chapters.size(), 1);
            QCOMPARE(chapters.first().paragraphs.size(), 1);
            const Paragraph &p = chapters.first().paragraphs.first();
            QVERIFY2(p.text == line, qPrintable(line + " came back as " + p.text + " from " + escaped));
            QVERIFY2(p.styles.isEmpty() && p.level == 0, qPrintable(line));
        }
    }

    void articleDecodesTheCharset() {
        // Windows-1252, named by the page.
        const QByteArray page = "<html><head><meta charset=\"windows-1252\"><title>Caf\xE9 notes</title></head><body>"
                                "<p>The caf\xE9 said \x93hello\x94 to everyone who came in from the rain that day.</p>"
                                "</body></html>";
        Article article = ArticleExtractor::extract(page, QUrl("https://cafe.example/"));
        QCOMPARE(article.title, QString::fromUtf8("Café notes"));
        QVERIFY2(article.text.contains(QString::fromUtf8("The café said “hello”")), qPrintable(article.text));
        // The response's charset wins over the page's, and Latin-1 reads as
        // browsers read it.
        const QByteArray labelled = "<html><head><meta charset=\"utf-8\"></head><body><p>Na\xEFve \x96 ok.</p></body></html>";
        QCOMPARE(ArticleExtractor::decode(labelled, "text/html; charset=ISO-8859-1").contains(QString::fromUtf8("Naïve – ok.")),
                 true);
        // UTF-8 without a label.
        QVERIFY(ArticleExtractor::decode("<p>Olá</p>", "text/html").contains(QString::fromUtf8("Olá")));
    }

    void webAddressesAreChecked() {
        QString error;
        QCOMPARE(WebImport::parseAddress("example.com/story?id=1", &error), QUrl("https://example.com/story?id=1"));
        QCOMPARE(WebImport::parseAddress("  http://example.com/a  ", &error), QUrl("http://example.com/a"));
        QCOMPARE(WebImport::parseAddress("HTTPS://Example.com", &error).host(), QString("example.com"));
        QVERIFY(WebImport::parseAddress("http://localhost:8080/x", &error).isValid());
        QVERIFY(WebImport::parseAddress("http://[::1]:8080/x", &error).isValid());
        for (const QString address : {"", "hello", "https://exa mple.com", "https://", "http://intranet/page"}) {
            error.clear();
            QVERIFY2(!WebImport::parseAddress(address, &error).isValid(), qPrintable(address));
            QVERIFY2(error.contains("such as https://example.com"), qPrintable(address + ": " + error));
        }
        for (const QString address : {"ftp://example.com/file", "file:///etc/passwd", "javascript:alert(1)",
                                       "data:text/html,<p>hi</p>", "mailto:someone@example.com"}) {
            error.clear();
            QVERIFY2(!WebImport::parseAddress(address, &error).isValid(), qPrintable(address));
            QVERIFY2(error.contains("http:// or https://"), qPrintable(address + ": " + error));
        }

        const QUrl base("https://site.example/old/page");
        QCOMPARE(WebImport::refreshTarget("<head><meta http-equiv=\"refresh\" content=\"0; url=/new/\"></head>", base),
                 QUrl("https://site.example/new/"));
        QCOMPARE(WebImport::refreshTarget("<META HTTP-EQUIV=Refresh CONTENT='1;URL=next.html'>", base),
                 QUrl("https://site.example/old/next.html"));
        // A page that waits has something to show, and only web pages are followed.
        QVERIFY(!WebImport::refreshTarget("<meta http-equiv=\"refresh\" content=\"30; url=/new/\">", base).isValid());
        QVERIFY(!WebImport::refreshTarget("<meta http-equiv=\"refresh\" content=\"0; url=javascript:x()\">", base).isValid());
        QVERIFY(!WebImport::refreshTarget("<meta http-equiv=\"refresh\" content=\"0; url=page\">", base).isValid());
        QVERIFY(!WebImport::refreshTarget("<meta name=\"viewport\" content=\"width=device-width\">", base).isValid());
    }

    void webImportFetchesOnlyThePage() {
        FakeHttpServer server([](const FakeHttpServer::Request &r) -> FakeHttpServer::Response {
            const QList<QPair<QByteArray, QByteArray>> html{{"Content-Type", "text/html; charset=utf-8"},
                                                            {"Set-Cookie", "visitor=1; Path=/"}};
            if (r.target == "/story")
                return {200, Fixtures::articlePage(), html};
            if (r.target == "/old")
                return {301, {}, {{"Location", "/story"}}};
            if (r.target == "/moved")
                return {200, "<html><head><meta http-equiv=\"refresh\" content=\"0; url=/old\"></head><body></body></html>", html};
            return {404, "missing", {}};
        });
        WebImport web;
        QSignalSpy fetched(&web, &WebImport::fetched);
        QSignalSpy failed(&web, &WebImport::failed);
        web.fetch(server.url() + "/story#comments");
        QVERIFY(web.busy());
        QCOMPARE(web.fetching(), QString("127.0.0.1"));
        QTRY_COMPARE(fetched.count(), 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!web.busy());
        QVERIFY(web.hasArticle());
        QVERIFY(web.error().isEmpty());
        QCOMPARE(web.title(), QString("Why Terns Fly So Far"));
        QCOMPARE(web.author(), QString("Ana Ribeiro"));
        QCOMPARE(web.source(), server.url() + "/story");
        QVERIFY(web.text().contains("Following the wind"));
        // The page only: none of its scripts, styles or images.
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(server.requests.first().method, QByteArray("GET"));
        QVERIFY(server.requests.first().headers.value("user-agent").contains("omareader"));

        // Moved pages are followed, and keep the address they ended up at,
        // and no cookie carries from one fetch to the next.
        web.fetch(server.url() + "/moved");
        QTRY_COMPARE(fetched.count(), 2);
        QCOMPARE(web.source(), server.url() + "/story");
        QCOMPARE(server.requests.size(), 4);
        for (const auto &request : std::as_const(server.requests))
            QVERIFY(!request.headers.contains("cookie"));
        QCOMPARE(failed.count(), 0);
    }

    void webImportExplainsWhatWentWrong() {
        FakeHttpServer server([](const FakeHttpServer::Request &r) -> FakeHttpServer::Response {
            const QList<QPair<QByteArray, QByteArray>> html{{"Content-Type", "text/html"}};
            if (r.target == "/missing")
                return {404, "<p>Not found</p>", html};
            if (r.target == "/private")
                return {403, "<p>Log in</p>", html};
            if (r.target == "/busy")
                return {429, {}, {}};
            if (r.target == "/broken")
                return {500, {}, {}};
            if (r.target == "/paper.pdf")
                return {200, "%PDF-1.4 binary", {{"Content-Type", "application/pdf"}}};
            if (r.target == "/picture")
                return {200, "\x89PNG", {{"Content-Type", "image/png"}}};
            if (r.target == "/unlabelled")
                return {200, QByteArray("PK\x03\x04zipped", 10), {}};
            if (r.target == "/empty")
                return {200, "<html><body><nav><a href=\"/\">Home</a></nav><script>render()</script></body></html>", html};
            if (r.target == "/big")
                return {200, "<p>" + QByteArray(5000, 'a') + "</p>", html};
            if (r.target == "/elsewhere")
                return {302, {}, {{"Location", "ftp://files.example/story"}}};
            return {200, Fixtures::articlePage(), html};
        });
        // A port nothing listens on.
        QTcpServer closed;
        QVERIFY(closed.listen(QHostAddress::LocalHost));
        const QString refused = QString("http://127.0.0.1:%1/").arg(closed.serverPort());
        closed.close();

        WebImport web;
        web.setMaxBytes(4000);
        const QList<QPair<QString, QString>> cases{
            {"hello", "such as https://example.com"},
            {"ftp://example.com/x", "http:// or https://"},
            {server.url() + "/missing", "no page at this address (HTTP 404)"},
            {server.url() + "/private", "login or a paywall"},
            {server.url() + "/busy", "too many requests"},
            {server.url() + "/broken", "(HTTP 500)"},
            {server.url() + "/paper.pdf", "a PDF file"},
            {server.url() + "/picture", "image/png content"},
            {server.url() + "/unlabelled", "leads to a file"},
            {server.url() + "/empty", "No readable text"},
            {server.url() + "/big", "larger than"},
            {server.url() + "/elsewhere", "on to ftp://files.example/story, which isn't a web page"},
            {refused, "couldn't be reached"},
        };
        for (const auto &[address, reason] : cases) {
            QSignalSpy failed(&web, &WebImport::failed);
            QSignalSpy fetched(&web, &WebImport::fetched);
            web.fetch(address);
            QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
            QVERIFY2(web.error().contains(reason), qPrintable(address + ": " + web.error()));
            QCOMPARE(failed.first().first().toString(), web.error());
            QCOMPARE(fetched.count(), 0);
            QVERIFY(!web.busy());
            QVERIFY(!web.hasArticle());
            QVERIFY(web.text().isEmpty());
        }
        // The next fetch starts clean.
        QSignalSpy fetched(&web, &WebImport::fetched);
        web.fetch(server.url() + "/story");
        QVERIFY(web.error().isEmpty());
        QTRY_COMPARE(fetched.count(), 1);
    }

    void webImportGivesUpOnASilentSiteOrWhenCancelled() {
        // Takes the connection and never answers.
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        QList<QTcpSocket *> held;
        connect(&silent, &QTcpServer::newConnection, this, [&] {
            while (QTcpSocket *socket = silent.nextPendingConnection())
                held << socket;
        });
        const QString address = QString("http://127.0.0.1:%1/slow").arg(silent.serverPort());

        WebImport web;
        web.setTimeout(300);
        QSignalSpy failed(&web, &WebImport::failed);
        QSignalSpy fetched(&web, &WebImport::fetched);
        web.fetch(address);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(web.error(), QString("127.0.0.1 took too long to answer."));
        QVERIFY(!web.busy());

        // Cancelling stops it quietly, leaving no error and no article.
        web.setTimeout(30000);
        QSignalSpy busy(&web, &WebImport::busyChanged);
        web.fetch(address);
        QVERIFY(web.busy());
        QVERIFY(web.error().isEmpty());
        QTRY_COMPARE(held.size(), 2);
        web.cancel();
        QVERIFY(!web.busy());
        QTest::qWait(200);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(fetched.count(), 0);
        QVERIFY(web.error().isEmpty());
        QVERIFY(!web.hasArticle());
        QCOMPARE(busy.count(), 2);

        qDeleteAll(held);
    }

    void webPagesStayInTheLibrary() {
        QTemporaryDir dir;
        const QString file = dir.filePath("library.json");
        const QString text = "# Why Terns Fly\n\nEvery year the tern flies far. It rests on the wind.\n\n"
                             "## Following the wind\n\nIt glides **most** of the way.";
        QString path;
        {
            Library library(file, dir.filePath("covers"));
            Reader reader(&library, nullptr, nullptr);
            QVERIFY(reader.addText(" Why Terns Fly ", text, "https://www.fieldjournal.example/2026/terns", "Ana Ribeiro"));
            QTRY_VERIFY(reader.hasBook());
            path = reader.path();
            QCOMPARE(reader.title(), QString("Why Terns Fly"));
            QCOMPARE(library.entry(path).source, QString("https://www.fieldjournal.example/2026/terns"));
            QCOMPARE(library.entry(path).author, QString("Ana Ribeiro"));
            QCOMPARE(library.entry(path).format, QString("web"));
            const QVariantMap card = library.books().first().toMap();
            QCOMPARE(card.value("site").toString(), QString("fieldjournal.example"));
            QCOMPARE(card.value("source").toString(), QString("https://www.fieldjournal.example/2026/terns"));
            reader.setVisibleParagraph(2);
            const int sentence = reader.sentence();
            QVERIFY(sentence > 0);
            reader.saveNow();
            reader.closeBook();
        }

        // Kept in the app's own folder, read without the network after a
        // restart, from where it was left.
        Library library(file, dir.filePath("covers"));
        QCOMPARE(library.count(), 1);
        QVERIFY(library.isText(path));
        QVERIFY(path.startsWith(library.textDirectory()));
        QCOMPARE(library.entry(path).source, QString("https://www.fieldjournal.example/2026/terns"));
        QString error;
        const auto book = BookLoader::load(path, &error);
        QVERIFY2(book, qPrintable(error));
        QCOMPARE(book->source, QString("https://www.fieldjournal.example/2026/terns"));
        QCOMPARE(book->author, QString("Ana Ribeiro"));
        QCOMPARE(book->format, QString("web"));
        const int saved = library.entry(path).sentence;
        QVERIFY(saved > 0);
        Reader reader(&library, nullptr, nullptr);
        reader.open(path);
        QTRY_VERIFY(reader.hasBook());
        QCOMPARE(reader.sentence(), saved);
        QCOMPARE(library.entry(path).source, QString("https://www.fieldjournal.example/2026/terns"));
    }

    // The web page dialog and its messages are new; check each of their
    // strings so none ships in English to a Portuguese reader.
    void everyWebPageStringHasAPortugueseTranslation() {
        QFile file(QString(OMAREADER_SOURCE) + "/src/i18n/pt-BR.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject catalog = QJsonDocument::fromJson(file.readAll()).object();
        static const QRegularExpression call("\\b(?:qsTr|tr)\\(\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
        int found = 0;
        for (const QString name : {"src/UrlDialog.qml", "src/webimport.cpp", "src/LibraryView.qml", "src/HelpOverlay.qml"}) {
            QFile source(QString(OMAREADER_SOURCE) + "/" + name);
            QVERIFY(source.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(source.readAll());
            for (auto it = call.globalMatch(text); it.hasNext();) {
                QString key = it.next().captured(1);
                key.replace("\\\"", "\"").replace("\\n", "\n");
                ++found;
                QVERIFY2(catalog.contains(key), qPrintable(name + ": no pt-BR entry for: " + key));
            }
        }
        QVERIFY(found > 40);
    }

    void titleAgentPrefersOmarchysDefault() {
        QTemporaryDir dir;
        const QString defaults = dir.filePath("agent");
        QVERIFY(!TitleAgent::find(defaults, {dir.path()}).isValid());
        fakeAgent(dir, "claude", "echo Claude");
        fakeAgent(dir, "codex", "echo Codex");
        fakeAgent(dir, "pi", "echo Pi");
        QCOMPARE(TitleAgent::find(defaults, {dir.path()}).id, QString("claude"));
        write(defaults, "codex\n");
        QCOMPARE(TitleAgent::find(defaults, {dir.path()}).id, QString("codex"));
        QCOMPARE(TitleAgent::find(defaults, {dir.path()}).name, QString("Codex"));
        // An agent without a safe headless mode is passed over.
        write(defaults, "pi\n");
        QCOMPARE(TitleAgent::find(defaults, {dir.path()}).id, QString("claude"));

        // Every supported agent gets a read-only, one-turn command line.
        for (const QString &id : TitleAgent::supportedAgents()) {
            const QStringList arguments = TitleAgent::arguments(id, "PROMPT");
            QCOMPARE(arguments.last(), QString("PROMPT"));
        }
        QVERIFY(TitleAgent::arguments("codex", "x").join(' ').contains("--sandbox read-only"));
        QCOMPARE(TitleAgent::arguments("claude", "x").at(TitleAgent::arguments("claude", "x").indexOf("--tools") + 1), QString());
        QVERIFY(TitleAgent::prompt(QString(10000, 'a')).size() < 4500);
    }

    void titleAgentSuggestsOnlyWhenAllowed() {
        QTemporaryDir dir;
        const QString defaults = dir.filePath("agent");
        write(defaults, "codex\n");
        fakeAgent(dir, "codex",
                  "printf '%s\\n' \"$@\" > '" + dir.filePath("arguments").toUtf8() + "'\n"
                  "pwd > '" + dir.filePath("cwd").toUtf8() + "'\n"
                  "echo 'Title: \"Arctic Terns\"'\n");
        TitleAgent agent(defaults, {dir.path()});
        agent.setEnabled(false);
        QVERIFY(agent.available());
        QSignalSpy suggested(&agent, &TitleAgent::suggested);
        QSignalSpy failed(&agent, &TitleAgent::failed);

        // Off by default: nothing is sent anywhere.
        agent.suggest("Terns fly from pole to pole.");
        QVERIFY(!agent.busy());
        QVERIFY(!QFileInfo::exists(dir.filePath("arguments")));

        agent.setEnabled(true);
        QVERIFY(QSettings().value("titles/useAgent").toBool());
        agent.suggest("   ");
        QVERIFY(!agent.busy());
        agent.suggest("Terns fly from pole to pole.");
        QVERIFY(agent.busy());
        QTRY_COMPARE(suggested.size(), 1);
        QCOMPARE(suggested.first().first().toString(), QString("Arctic Terns"));
        QVERIFY(!agent.busy());
        QVERIFY(failed.isEmpty());
        QFile arguments(dir.filePath("arguments"));
        QVERIFY(arguments.open(QIODevice::ReadOnly));
        const QString passed = QString::fromUtf8(arguments.readAll());
        QVERIFY(passed.startsWith("exec\n"));
        QVERIFY(passed.contains("Terns fly from pole to pole."));
        // It runs in an empty directory of its own, removed afterwards.
        QFile cwd(dir.filePath("cwd"));
        QVERIFY(cwd.open(QIODevice::ReadOnly));
        const QString workDirectory = QString::fromUtf8(cwd.readAll()).trimmed();
        QVERIFY(workDirectory != dir.path());
        QVERIFY(!QFileInfo::exists(workDirectory));
        agent.setEnabled(false);
    }

    void titleAgentFailuresFallBack() {
        QTemporaryDir dir;
        const QString defaults = dir.filePath("agent");
        write(defaults, "claude\n");
        const QString script = dir.filePath("claude");
        fakeAgent(dir, "claude", "echo 'not logged in' >&2\nexit 1\n");
        TitleAgent agent(defaults, {dir.path()});
        agent.setEnabled(true);
        QSignalSpy suggested(&agent, &TitleAgent::suggested);
        QSignalSpy failed(&agent, &TitleAgent::failed);

        agent.suggest("Some text.");
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(failed.first().first().toString().contains("Claude Code"));

        // An empty answer is no title.
        fakeAgent(dir, "claude", "printf '\\n\\n'\n");
        agent.suggest("Some text.");
        QTRY_COMPARE(failed.size(), 2);

        // One that never answers is stopped.
        fakeAgent(dir, "claude", "sleep 30\n");
        agent.setTimeout(300);
        QElapsedTimer timer;
        timer.start();
        agent.suggest("Some text.");
        QTRY_COMPARE(failed.size(), 3);
        QVERIFY(timer.elapsed() < 5000);
        QVERIFY(!agent.busy());

        // A request dropped part way through reports nothing.
        agent.suggest("Some text.");
        QVERIFY(agent.busy());
        agent.cancel();
        QVERIFY(!agent.busy());
        QTest::qWait(500);
        QCOMPARE(failed.size(), 3);
        QVERIFY(suggested.isEmpty());

        // An agent removed since it was found fails rather than hangs.
        QFile::remove(script);
        agent.suggest("Some text.");
        QTRY_COMPARE(failed.size(), 4);
        agent.refresh();
        QVERIFY(!agent.available());
        agent.setEnabled(false);
    }

    void readerAppearanceIsBounded() {
        Reader reader(nullptr, nullptr, nullptr);
        reader.setFontSize(200);
        QCOMPARE(reader.fontSize(), 48);
        reader.setLineHeight(0.2);
        QCOMPARE(reader.lineHeight(), 1.0);
    }

    void readerFocusSettingsAreCheckedAndRemembered() {
        QSettings().remove("reader");
        {
            Reader reader(nullptr, nullptr, nullptr);
            // Focus mode starts off; what it turns on is ready for when it is.
            QVERIFY(!reader.focusMode());
            QCOMPARE(reader.spotlight(), QString("paragraph"));
            QVERIFY(reader.fadeRead());
            QVERIFY(reader.steadyLine());
            QVERIFY(reader.hideControls());
            QCOMPARE(reader.highlight(), QString("glass"));
            QCOMPARE(reader.highlightStrength(), QString("normal"));

            QSignalSpy changed(&reader, &Reader::focusChanged);
            reader.setSpotlight("everything");
            reader.setHighlight("none");
            reader.setHighlightStrength("blinding");
            QCOMPARE(reader.spotlight(), QString("paragraph"));
            QCOMPARE(reader.highlight(), QString("glass"));
            QCOMPARE(reader.highlightStrength(), QString("normal"));
            QCOMPARE(changed.size(), 0);

            reader.setFocusMode(true);
            reader.setSpotlight("sentence");
            reader.setFadeRead(false);
            reader.setSteadyLine(false);
            reader.setHideControls(false);
            reader.setHighlight("word");
            reader.setHighlightStrength("strong");
            QCOMPARE(changed.size(), 7);
        }
        Reader again(nullptr, nullptr, nullptr);
        QVERIFY(again.focusMode());
        QCOMPARE(again.spotlight(), QString("sentence"));
        QVERIFY(!again.fadeRead());
        QVERIFY(!again.steadyLine());
        QVERIFY(!again.hideControls());
        QCOMPARE(again.highlight(), QString("word"));
        QCOMPARE(again.highlightStrength(), QString("strong"));
        QSettings().remove("reader");
    }

    // Dictionaries.

    void starDictLooksWordsUpIgnoringCase() {
        QTemporaryDir dir;
        // Compressed as FreeDict ships them, in chunks small enough that an
        // article spans several.
        writeStarDict(dir.filePath("eng-por"), "English-Portuguese",
                      {{"house", "<div>/<font color=\"gray\">haus</font>/<br><ol><li><div>casa</div></li>"
                                 "<li><div>lar</div></li></ol></div>"},
                       {"House", "<div>a family line</div>"},
                       {"run", "<ol><li><ol><li>correr</li><li>executar</li></ol></li><li>corrida</li></ol>"},
                       {"são", "<div>sound &amp; healthy</div>"}},
                      "sametypesequence=h\n", true, {{"ran", 2}});
        const auto dictionary = Dictionary::open(dir.filePath("eng-por/eng-por.ifo"));
        QVERIFY(dictionary);
        QCOMPARE(dictionary->name(), QString("English-Portuguese"));
        QCOMPARE(dictionary->format(), Dictionary::StarDict);
        QCOMPARE(dictionary->wordCount(), 4);

        const QList<DictionaryEntry> house = dictionary->lookup("HOUSE");
        QCOMPARE(house.size(), 2);
        QCOMPARE(house.at(0).headword, QString("house"));
        QCOMPARE(house.at(0).text, QString("/haus/\n1. casa\n2. lar"));
        QCOMPARE(house.at(0).dictionary, QString("English-Portuguese"));
        QCOMPARE(house.at(1).text, QString("a family line"));
        QCOMPARE(dictionary->lookup("house", 1).size(), 1);

        // A synonym leads to its entry; a group of senses keeps to one line.
        const QList<DictionaryEntry> ran = dictionary->lookup("ran");
        QCOMPARE(ran.size(), 1);
        QCOMPARE(ran.at(0).headword, QString("run"));
        QCOMPARE(ran.at(0).text, QString("1. correr; executar\n2. corrida"));

        // Case folds beyond ASCII too.
        QCOMPARE(dictionary->lookup("SÃO").value(0).text, QString("sound & healthy"));
        QVERIFY(dictionary->lookup("missing").isEmpty());
        QVERIFY(dictionary->lookup("").isEmpty());
    }

    void starDictReadsEachTypedField() {
        QTemporaryDir dir;
        // Without a sametypesequence each field carries its type: here a
        // phonetic, a text and a sound, which is skipped.
        QByteArray article = QByteArray("t") + "kat" + '\0' + "m" + "cat\na small feline" + '\0' + "W";
        article += QByteArray::fromHex("00000003") + "RIF";
        writeStarDict(dir.filePath("plain"), "Plain", {{"cat", article}}, "", false);
        const auto dictionary = Dictionary::open(dir.filePath("plain/plain.ifo"));
        QVERIFY(dictionary);
        // The text's first line only repeated the headword.
        QCOMPARE(dictionary->lookup("Cat").value(0).text, QString("/kat/\na small feline"));

        // Anything that isn't a StarDict .ifo is no dictionary.
        write(dir.filePath("other.ifo"), "not a dictionary\n");
        QVERIFY(!Dictionary::open(dir.filePath("other.ifo")));
        QVERIFY(!Dictionary::open(dir.filePath("plain/plain.dict")));
    }

    void dictdDictionaryReadsItsIndexAndName() {
        QTemporaryDir dir;
        const QByteArray name = "00-database-short\n     Test WordNet\n";
        const QByteArray dog = "dog\n     n 1: a member of the genus {Canis}\n          that barks\n";
        const QByteArray house = "house /haus/\ncasa, lar\n";
        const QByteArray dict = name + dog + house;
        auto number = [](qsizetype value) {
            static const QByteArray digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            QByteArray out;
            do {
                out.prepend(digits.at(value % 64));
                value /= 64;
            } while (value > 0);
            return out;
        };
        const QByteArray index = "00-database-short\t" + number(0) + "\t" + number(name.size()) + "\n" + "dog\t"
                                 + number(name.size()) + "\t" + number(dog.size()) + "\n" + "house\t"
                                 + number(name.size() + dog.size()) + "\t" + number(house.size()) + "\n";
        // Once plain, once gzipped whole (no dictzip chunk table).
        for (const bool zipped : {false, true}) {
            const QString base = dir.filePath(zipped ? "zipped/wn" : "plain/wn");
            QDir().mkpath(QFileInfo(base).path());
            write(base + ".index", index);
            write(base + (zipped ? ".dict.dz" : ".dict"), zipped ? gzipped(dict) : dict);
            const auto dictionary = Dictionary::open(base + ".index");
            QVERIFY(dictionary);
            QCOMPARE(dictionary->name(), QString("Test WordNet"));
            QCOMPARE(dictionary->format(), Dictionary::Dictd);
            // The headword line goes, the indent goes, the braces go.
            QCOMPARE(dictionary->lookup("Dog").value(0).text, QString("n 1: a member of the genus Canis\n     that barks"));
            // The pronunciation on the headword's line stays.
            QCOMPARE(dictionary->lookup("house").value(0).text, QString("/haus/\ncasa, lar"));
        }
        // A dictd index without its data is no dictionary.
        write(dir.filePath("lonely.index"), index);
        QVERIFY(!Dictionary::open(dir.filePath("lonely.index")));
    }

    void dictionaryMarkupBecomesPlainText() {
        QCOMPARE(Dictionary::plainText("<b>bold</b> and <i>it</i>&nbsp;&lt;tag&gt; &#233;&#xE9; &bogus; a&b", true),
                 QString("bold and it <tag> éé &bogus; a&b"));
        QCOMPARE(Dictionary::plainText("<ul><li>one</li><li>two<ol><li>deep</li></ol></li></ul>", true),
                 QString("• one\n• two\n   1. deep"));
        QCOMPARE(Dictionary::plainText("line<br>next<p>para</p><script>alert(1)</script><style>p{}</style>end", true),
                 QString("line\nnext\npara\nend"));
        QCOMPARE(Dictionary::plainText("<div>\n  <a href=\"#a\">Haus</a>\n  <a href=\"#b\">frei Haus</a>\n</div>", true),
                 QString("Haus · frei Haus"));
        // HTML's spacing collapses; other markup keeps its lines.
        QCOMPARE(Dictionary::plainText("a\n\n   b", true), QString("a b"));
        QCOMPARE(Dictionary::plainText("<k>word</k>\n<tr>wɜːd</tr>\na <ex>unit</ex>", false), QString("word\nwɜːd\na unit"));
        QCOMPARE(Dictionary::plainText("<!-- note -->x <unclosed", true), QString("x <unclosed"));
    }

    void dictionaryFormsTryTheBaseWord() {
        auto has = [](const QString &word, const QString &form) { return Dictionary::forms(word).contains(form); };
        QCOMPARE(Dictionary::forms("Running").first(), QString("Running"));
        QVERIFY(has("Running", "run"));
        QVERIFY(has("houses", "house"));
        QVERIFY(has("stories", "story"));
        QVERIFY(has("wolves", "wolf"));
        QVERIFY(has("stopped", "stop"));
        QVERIFY(has("used", "use"));
        QVERIFY(has("making", "make"));
        QVERIFY(has("bigger", "big"));
        QVERIFY(has("happily", "happy"));
        QVERIFY(has("children", "child"));
        QVERIFY(has("women", "woman"));
        QVERIFY(has("Alice’s", "alice"));
        QVERIFY(has("animais", "animal"));
        QVERIFY(has("canções", "canção"));
        QVERIFY(has("homens", "homem"));
        QVERIFY(has("bonitas", "bonito"));
        QVERIFY(has("chevaux", "cheval"));
        // The page's punctuation goes; the word itself comes first.
        QCOMPARE(Dictionary::forms("“Well,").first(), QString("Well"));
        QVERIFY(Dictionary::forms("…").isEmpty());
        // Short words are left alone.
        QCOMPARE(Dictionary::forms("is"), QStringList{"is"});
    }

    void dictionaryShelfAsksEachDictionaryForAFormItHas() {
        QTemporaryDir dir;
        const QString mine = dir.filePath("mine"), stardict = dir.filePath("stardict");
        writeStarDict(mine + "/eng-por", "English-Portuguese", {{"run", "<div>correr</div>"}}, "sametypesequence=h\n", true);
        writeStarDict(stardict + "/nested/en", "English", {{"running", "the act of running"}, {"run", "to go fast"}},
                      "sametypesequence=m\n", false);
        write(stardict + "/broken.ifo", "garbage");
        DictionaryShelf shelf({mine, stardict, dir.filePath("missing")});
        QCOMPARE(shelf.dictionaries().size(), 2);

        // Each dictionary answers with the first form it has.
        DictionaryShelf::Result result = shelf.lookup("Running");
        QCOMPARE(result.word, QString("Running"));
        QCOMPARE(result.dictionaries, 2);
        QCOMPARE(result.entries.size(), 2);
        QCOMPARE(result.entries.at(0).dictionary, QString("English-Portuguese"));
        QCOMPARE(result.entries.at(0).headword, QString("run"));
        QCOMPARE(result.entries.at(1).headword, QString("running"));

        // A dictionary added later turns up at the next lookup, and one
        // taken away goes.
        writeStarDict(mine + "/eng-spa", "English-Spanish", {{"run", "<div>correr</div>"}}, "sametypesequence=h\n", false);
        QCOMPARE(shelf.lookup("run").entries.size(), 3);
        QVERIFY(QDir(stardict + "/nested").removeRecursively());
        result = shelf.lookup("run");
        QCOMPARE(result.dictionaries, 2);
        QCOMPARE(result.entries.size(), 2);

        // omareader's own folder comes first, under the data home.
        QCOMPARE(DictionaryShelf::defaultDirectories().first(), DictionaryShelf::installDirectory());
        QVERIFY(DictionaryShelf::installDirectory().startsWith(qEnvironmentVariable("XDG_DATA_HOME")));
    }

    void wordLookupFindsTheWordAndLooksItUpInTheBackground() {
        QCOMPARE(WordLookup::wordAt("The house, stood.", 5).value("text").toString(), QString("house"));
        QCOMPARE(WordLookup::wordAt("The house, stood.", 4).value("start").toInt(), 4);
        // Just past a word's last letter still counts as the word.
        QCOMPARE(WordLookup::wordAt("The house, stood.", 9).value("text").toString(), QString("house"));
        QCOMPARE(WordLookup::wordAt("The house, stood.", 9).value("length").toInt(), 5);
        QCOMPARE(WordLookup::wordAt("don't", 2).value("text").toString(), QString("don't"));
        QVERIFY(WordLookup::wordAt("a  —  b", 3).isEmpty());

        QTemporaryDir dir;
        writeStarDict(dir.filePath("eng-por"), "English-Portuguese", {{"house", "<div>casa</div>"}},
                      "sametypesequence=h\n", true);
        WordLookup lookup({dir.path()});
        QSignalSpy changed(&lookup, &WordLookup::changed);
        lookup.lookUp(" Houses ");
        QCOMPARE(lookup.word(), QString("Houses"));
        QVERIFY(lookup.busy());
        QTRY_VERIFY(!lookup.busy());
        QCOMPARE(lookup.dictionaryCount(), 1);
        QCOMPARE(lookup.entries().size(), 1);
        const QVariantMap entry = lookup.entries().first().toMap();
        QCOMPARE(entry.value("headword").toString(), QString("house"));
        QCOMPARE(entry.value("text").toString(), QString("casa"));
        QVERIFY(entry.value("firstOfDictionary").toBool());
        QVERIFY(changed.size() >= 2);
        lookup.clear();
        QVERIFY(lookup.word().isEmpty() && lookup.entries().isEmpty());
    }

    void dictionaryCatalogPinsFreeDictReleases() {
        const QList<CatalogDictionary> all = DictionaryCatalog::all();
        QVERIFY(all.size() > 300);
        static const QRegularExpression sha512("^[0-9a-f]{128}$");
        for (const CatalogDictionary &d : all) {
            QVERIFY2(sha512.match(d.sha512).hasMatch(), qPrintable(d.name));
            QVERIFY2(d.url.startsWith("https://download.freedict.org/dictionaries/" + d.name + "/"), qPrintable(d.url));
            QVERIFY2(d.url.endsWith(".stardict.tar.xz") && d.bytes > 0, qPrintable(d.name));
        }
        QCOMPARE(DictionaryCatalog::find("eng-por").name, QString("eng-por"));
        QCOMPARE(DictionaryCatalog::find("EN-pt").name, QString("eng-por"));
        QCOMPARE(DictionaryCatalog::find("de-en").title(), QString("German → English"));
        QVERIFY(DictionaryCatalog::find("eng-xxx").name.isEmpty());
        QVERIFY(DictionaryCatalog::find("eng").name.isEmpty());
        QCOMPARE(DictionaryCatalog::languageCode("pt-BR"), QString("por"));
        QCOMPARE(DictionaryCatalog::languageCode("zh_CN"), QString("zho"));
        // The window suggests a dictionary into its own language.
        QCOMPARE(WordLookup::setupCommand("pt-BR"), QString("omareader setup --dictionary eng-por"));
        QCOMPARE(WordLookup::setupCommand("es"), QString("omareader setup --dictionary eng-spa"));
        QCOMPARE(WordLookup::setupCommand("en"), QString("omareader dictionaries --available"));
    }

    void setupInstallsAVerifiedDictionary() {
        for (const char *tool : {"curl", "tar", "xz", "sha512sum"})
            if (QStandardPaths::findExecutable(tool).isEmpty())
                QSKIP(qPrintable(QString("%1 is not installed").arg(tool)));
        QTemporaryDir dir;
        // A release as FreeDict packs one: a folder named after the dictionary.
        writeStarDict(dir.filePath("release/eng-tst"), "Test", {{"word", "<div>palavra</div>"}}, "sametypesequence=h\n", true);
        const QString tarball = dir.filePath("eng-tst.tar.xz");
        QCOMPARE(QProcess::execute("tar", {"-cJf", tarball, "-C", dir.filePath("release"), "eng-tst"}), 0);
        QFile packed(tarball);
        QVERIFY(packed.open(QIODevice::ReadOnly));
        const QString sum = QCryptographicHash::hash(packed.readAll(), QCryptographicHash::Sha512).toHex();
        const QString data = dir.filePath("data");
        auto setup = [&](const QStringList &arguments, QString *output) {
            QProcess process;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("XDG_DATA_HOME", data);
            process.setProcessEnvironment(env);
            process.setProcessChannelMode(QProcess::MergedChannels);
            process.start("sh", QStringList{QString(OMAREADER_SOURCE) + "/src/setup-dictionary.sh"} + arguments);
            process.waitForFinished(60000);
            *output = QString::fromUtf8(process.readAll());
            return process.exitCode();
        };
        const QString url = QUrl::fromLocalFile(tarball).toString();
        const QString installed = data + "/omareader/dictionaries/eng-tst";
        QString output;

        // A dry run changes nothing.
        QCOMPARE(setup({"--dry-run", "eng-tst", url, sum}, &output), 0);
        QVERIFY2(output.contains("would download"), qPrintable(output));
        QVERIFY(!QFileInfo::exists(installed));

        // A download that doesn't match its checksum installs nothing.
        QCOMPARE(setup({"eng-tst", url, QString(128, 'a')}, &output), 1);
        QVERIFY2(output.contains("did not match"), qPrintable(output));
        QVERIFY(!QFileInfo::exists(installed));

        QCOMPARE(setup({"eng-tst", url, sum}, &output), 0);
        QVERIFY2(QFileInfo::exists(installed + "/eng-tst.ifo"), qPrintable(output));
        QCOMPARE(DictionaryShelf(QStringList{data + "/omareader/dictionaries"}).lookup("word").entries.value(0).text,
                 QString("palavra"));
        // Nothing is left behind beside it, and a second run leaves it be.
        QCOMPARE(QDir(data + "/omareader").entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
                 QStringList{"dictionaries"});
        QCOMPARE(setup({"eng-tst", url, sum}, &output), 0);
        QVERIFY2(output.contains("already in place"), qPrintable(output));
        QCOMPARE(setup({"../evil", url, sum}, &output), 1);
    }

    // The real window, offscreen.

    void libraryRemoveButtonDrawsItsCrossCentred() {
        QTemporaryDir dir;
        AppWindow app(dir.path());
        app.library.addText("Dialog, Narration, and Agent Title Testing", "Some text to read.");
        QVERIFY(app.show());
        // The button shows on the card the keyboard is on, the first one.
        QTRY_COMPARE(app.items("removeBookButton").size(), 1);
        QQuickItem *button = app.items("removeBookButton").first();
        const QImage shot = app.grab("library-remove-button");
        const QRect area = app.area(shot, button);
        if (!qEnvironmentVariableIsEmpty("OMAREADER_SCREENSHOTS"))
            shot.copy(area.adjusted(-8, -8, 8, 8)).scaled(area.width() * 8, area.height() * 8)
                .save(qEnvironmentVariable("OMAREADER_SCREENSHOTS") + "/library-remove-button-zoom.png");
        const QRect cross = brightBoxInCircle(shot, area);
        QVERIFY2(cross.isValid(), "no cross drawn on the button");
        const QPointF offset = QRectF(cross).center() - QRectF(area).center();
        QVERIFY2(qAbs(offset.x()) <= 1.0 && qAbs(offset.y()) <= 1.0,
                 qPrintable(QString("the cross is %1, %2 px off the button's centre").arg(offset.x()).arg(offset.y())));
        // And it is a cross, not a dot: about 12 px each way.
        QVERIFY2(cross.width() >= 7 && cross.height() >= 7, qPrintable(QString("cross is %1x%2").arg(cross.width()).arg(cross.height())));
    }

    void narrationBarShowsTheSleepTimer() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        QVERIFY(app.reader.addText("Bedtime", "Some words to hear before sleeping.\n\nAnd a few more of them."));
        QTRY_COMPARE(app.items("sleepButton").size(), 1);
        QQuickItem *button = app.items("sleepButton").first();
        QTest::mouseMove(app.window, QPoint(20, 300));

        auto zoom = [&](const QImage &shot, const QRect &area, const QString &name) {
            if (!qEnvironmentVariableIsEmpty("OMAREADER_SCREENSHOTS"))
                shot.copy(area.adjusted(-8, -8, 8, 8)).scaled(area.width() * 8, area.height() * 8)
                    .save(qEnvironmentVariable("OMAREADER_SCREENSHOTS") + "/" + name + ".png");
        };
        auto accent = [](const QImage &shot, const QRect &area) {
            int count = 0;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const QColor c = shot.pixelColor(x, y);
                    count += c.red() > 200 && c.green() > 170 && c.blue() < 80;
                }
            return count;
        };

        // Off: a moon in the text's colour, centred on its button, and no
        // time beside it.
        QImage shot = app.grab("sleep-timer-off");
        QRect area = app.area(shot, button);
        zoom(shot, area, "sleep-timer-off-zoom");
        const QRect moon = brightBoxInCircle(shot, area, 150);
        QVERIFY2(moon.isValid(), "no moon drawn on the button");
        const QPointF offset = QRectF(moon).center() - QRectF(area).center();
        QVERIFY2(qAbs(offset.x()) <= 1.5 && qAbs(offset.y()) <= 1.5,
                 qPrintable(QString("the moon is %1, %2 px off the button's centre").arg(offset.x()).arg(offset.y())));
        QVERIFY2(moon.width() >= 9 && moon.height() >= 12, qPrintable(QString("moon is %1x%2").arg(moon.width()).arg(moon.height())));
        // A crescent, not a disc: its middle, right of centre, is empty.
        QVERIFY(qGray(shot.pixel(moon.right() - 1, moon.center().y())) < 150);
        QCOMPARE(accent(shot, area), 0);
        QVERIFY(app.items("sleepLabel").isEmpty());

        // On: the moon lights up in the accent, with the time left beside it.
        app.narrator.sleepAfter(30);
        QTRY_COMPARE(app.items("sleepLabel").size(), 1);
        QQuickItem *label = app.items("sleepLabel").first();
        QCOMPARE(label->property("text").toString(), QString("30:00"));
        shot = app.grab("sleep-timer-on");
        area = app.area(shot, button);
        zoom(shot, area.united(app.area(shot, label)), "sleep-timer-on-zoom");
        QVERIFY2(accent(shot, area) > 30, qPrintable(QString::number(accent(shot, area))));
        QVERIFY2(accent(shot, app.area(shot, label)) > 20, qPrintable(QString::number(accent(shot, app.area(shot, label)))));
        // It sits inside the bar, clear of the speed buttons.
        QQuickItem *bar = app.items("narrationBar").value(0);
        QVERIFY(bar);
        const QRectF inBar = label->mapRectToItem(bar, QRectF(0, 0, label->width(), label->height()));
        QVERIFY(inBar.left() > 0 && inBar.right() < bar->width());

        app.narrator.sleepAtEndOfChapter();
        QTRY_COMPARE(label->property("text").toString(), QString("End of chapter"));
        app.grab("sleep-timer-chapter");
        // The play button stays in the middle of the bar.
        QQuickItem *play = nullptr;
        for (QQuickItem *item : bar->findChildren<QQuickItem *>())
            if (item->width() == 60 && item->height() == 60 && QString(item->metaObject()->className()).startsWith("AbstractButton"))
                play = item;
        QVERIFY(play);
        const qreal playCentre = play->mapRectToItem(bar, QRectF(0, 0, 60, 60)).center().x();
        QVERIFY2(qAbs(playCentre - bar->width() / 2) <= 1, qPrintable(QString("%1 in %2").arg(playCentre).arg(bar->width())));
        // Even in the narrowest window, the longest of them stays clear of
        // the speed and the transport buttons.
        app.window->resize(640, 720);
        QTRY_COMPARE(app.window->width(), 640);
        app.grab("sleep-timer-narrow");
        const QRectF narrow = label->mapRectToItem(bar, QRectF(0, 0, label->width(), label->height()));
        // Past the play button and the next-sentence one beside it.
        const qreal transportEnd = play->mapRectToItem(bar, QRectF(0, 0, 60, 60)).right() + 8 + 44;
        QVERIFY2(narrow.left() > transportEnd && narrow.right() < bar->width(),
                 qPrintable(QString("%1 to %2 in %3, the buttons end at %4").arg(narrow.left()).arg(narrow.right()).arg(bar->width()).arg(transportEnd)));
        QQuickItem *faster = app.items("fasterButton").value(0);
        QVERIFY(faster);
        const QRectF fasterInBar = faster->mapRectToItem(bar, QRectF(0, 0, faster->width(), faster->height()));
        QVERIFY2(fasterInBar.right() <= bar->width() - 8 && fasterInBar.left() > narrow.right(),
                 qPrintable(QString("+ from %1 to %2 in %3").arg(fasterInBar.left()).arg(fasterInBar.right()).arg(bar->width())));
        // The voice's name gives way to it, and comes back with room.
        QVERIFY(app.items("voiceName").isEmpty());
        app.window->resize(1100, 720);
        QTRY_COMPARE(app.items("voiceName").size(), 1);

        // C opens the menu; the first choice is ten minutes.
        QObject *menu = app.window->findChild<QObject *>("sleepMenu");
        QVERIFY(menu);
        QTest::keyClick(app.window, Qt::Key_C);
        QTRY_VERIFY(menu->property("opened").toBool());
        app.grab("sleep-timer-menu");
        // It opens above the bar, not over it.
        QQuickItem *menuBox = qvariant_cast<QQuickItem *>(menu->property("background"));
        QVERIFY(menuBox);
        QVERIFY2(menuBox->mapToItem(bar, QPointF(0, menuBox->height())).y() <= 0,
                 qPrintable(QString::number(menuBox->mapToItem(bar, QPointF(0, menuBox->height())).y())));
        QTest::keyClick(app.window, Qt::Key_Down);
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_COMPARE(app.narrator.sleepMinutes(), 10);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QTRY_COMPARE(label->property("text").toString(), QString("10:00"));
        app.narrator.cancelSleep();
        QTRY_VERIFY(app.items("sleepLabel").isEmpty());
    }

    void readerDrawsFootnotesSmallerAndQuieter() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        const QString path = dir.filePath("notes.txt");
        write(path, "notes");
        Book book;
        book.path = path;
        book.title = "Footnoted";
        book.format = "pdf";
        const QString words = "Hollow hills hold old halls";
        book.chapters = {{"One", {{words, 0}, {words, 0, {}, true}}}};
        book.segment();
        app.reader.openBook(std::make_shared<Book>(book));
        QTRY_COMPARE(app.items("bookParagraph").size(), 2);
        QTest::mouseMove(app.window, QPoint(1050, 650));

        // The rows the text's ink covers, and its brightest pixel.
        const QImage shot = app.grab("footnote");
        struct Ink { int top = -1, bottom = -1, brightest = 0; };
        auto ink = [&](QQuickItem *item) {
            Ink found;
            const QRect area = app.area(shot, item).intersected(shot.rect());
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const int light = qGray(shot.pixel(x, y));
                    if (light > 90) {
                        found.top = found.top < 0 ? y : found.top;
                        found.bottom = y;
                    }
                    found.brightest = qMax(found.brightest, light);
                }
            return found;
        };
        Ink body, note;
        for (QQuickItem *item : app.items("bookParagraph"))
            (item->property("index").toInt() == 0 ? body : note) = ink(item);
        QVERIFY2(body.top >= 0 && note.top >= 0, "a paragraph isn't drawn");
        const int bodyHeight = body.bottom - body.top, noteHeight = note.bottom - note.top;
        QVERIFY2(noteHeight < bodyHeight * 0.9,
                 qPrintable(QString("the note is %1 px tall, the text %2 px").arg(noteHeight).arg(bodyHeight)));
        // Drawn in the muted colour (#8b8b94), the text in the body's (#d6d6da).
        QVERIFY2(body.brightest > 190, qPrintable(QString::number(body.brightest)));
        QVERIFY2(note.brightest < 160, qPrintable(QString::number(note.brightest)));
        if (!qEnvironmentVariableIsEmpty("OMAREADER_SCREENSHOTS")) {
            const QRect both = app.area(shot, app.items("bookParagraph").first())
                                   .united(app.area(shot, app.items("bookParagraph").last()));
            shot.copy(QRect(both.left() + both.width() / 4, both.top(), both.width() / 2, both.height()))
                .scaled(both.width(), both.height() * 2).save(qEnvironmentVariable("OMAREADER_SCREENSHOTS") + "/footnote-zoom.png");
        }
    }

    void readerDrawsHighlightsNotesAndBookmarks() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        const QString words = " has plain words in it, enough of them to wrap across a few lines of the column.";
        QVERIFY(app.reader.addText("Notes", "First" + words + "\n\nSecond" + words + "\n\nThird" + words));
        QTRY_COMPARE(app.items("bookParagraph").size(), 3);
        QTest::mouseMove(app.window, QPoint(1050, 650));

        // Pixels tinted with the accent, and filled with it, in each paragraph.
        auto marks = [&](const QString &name) {
            const QImage shot = app.grab(name);
            QList<int> tint(3), solid(3);
            for (QQuickItem *item : app.items("bookParagraph")) {
                const int index = item->property("index").toInt();
                const QRect area = app.area(shot, item).intersected(shot.rect());
                for (int y = area.top(); y <= area.bottom(); ++y)
                    for (int x = area.left(); x <= area.right(); ++x) {
                        const QColor c = shot.pixelColor(x, y);
                        tint[index] += c.red() > 50 && c.red() < 140 && c.red() > c.blue() + 30;
                        solid[index] += c.red() > 200 && c.green() > 170 && c.blue() < 80;
                    }
            }
            return std::pair{tint, solid};
        };
        const auto [plainTint, plainSolid] = marks("notes-plain");
        for (int i = 0; i < 3; ++i)
            QVERIFY2(plainTint[i] < 50 && plainSolid[i] < 10, qPrintable(QString("%1: %2, %3").arg(i).arg(plainTint[i]).arg(plainSolid[i])));

        // A highlight tints its passage, and only that.
        const QString id = app.annotations.addHighlight(1, 0, 1, 30);
        QVERIFY(!id.isEmpty());
        const auto [tint, solid] = marks("notes-highlight");
        QVERIFY2(tint[1] > 2000, qPrintable(QString::number(tint[1])));
        QVERIFY(tint[0] < 50 && tint[2] < 50);
        QVERIFY2(solid[1] < 10, qPrintable(QString::number(solid[1])));

        // A note underlines it in the accent.
        app.annotations.setNote(id, "Worth rereading.");
        const auto [notedTint, notedSolid] = marks("notes-note");
        QVERIFY2(notedSolid[1] > 40, qPrintable(QString::number(notedSolid[1])));
        QVERIFY(notedTint[1] > 2000);

        // A bookmark shows as a ribbon beside its paragraph, and fills the title bar's.
        QQuickItem *button = app.items("bookmarkButton").value(0);
        QVERIFY(button);
        auto accentIn = [&](const QImage &shot, QQuickItem *item) {
            const QRect area = app.area(shot, item).intersected(shot.rect());
            int count = 0;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const QColor c = shot.pixelColor(x, y);
                    count += c.red() > 200 && c.green() > 170 && c.blue() < 80;
                }
            return count;
        };
        QImage shot = app.grab("notes-no-bookmark");
        QVERIFY(app.items("paragraphBookmark").isEmpty());
        QCOMPARE(accentIn(shot, button), 0);
        app.reader.goTo(0, 2, 0);
        app.reader.toggleBookmark();
        QVERIFY(app.reader.bookmarked());
        QTRY_COMPARE(app.items("paragraphBookmark").size(), 1);
        QQuickItem *ribbon = app.items("paragraphBookmark").first();
        QQuickItem *third = nullptr;
        for (QQuickItem *item : app.items("bookParagraph"))
            if (item->property("index").toInt() == 2)
                third = item;
        QVERIFY(third && third->isAncestorOf(ribbon));
        shot = app.grab("notes-bookmark");
        QVERIFY2(accentIn(shot, ribbon) > 60, qPrintable(QString::number(accentIn(shot, ribbon))));
        QVERIFY2(accentIn(shot, button) > 60, qPrintable(QString::number(accentIn(shot, button))));
        // The ribbon is drawn in the middle of its button.
        QRect drawn;
        const QRect area = app.area(shot, button);
        for (int y = area.top(); y <= area.bottom(); ++y)
            for (int x = area.left(); x <= area.right(); ++x) {
                const QColor c = shot.pixelColor(x, y);
                if (c.red() > 200 && c.green() > 170 && c.blue() < 80)
                    drawn |= QRect(x, y, 1, 1);
            }
        const QPointF offset = QRectF(drawn).center() - QRectF(area).center();
        QVERIFY2(qAbs(offset.x()) <= 1.0 && qAbs(offset.y()) <= 1.0,
                 qPrintable(QString("the ribbon is %1, %2 px off the button's centre").arg(offset.x()).arg(offset.y())));
        // The ribbon sits in the margin, clear of the text.
        QVariant first;
        QMetaObject::invokeMethod(third, "rectFor", Q_RETURN_ARG(QVariant, first), Q_ARG(QVariant, 0));
        QVERIFY(ribbon->mapRectToScene(QRectF(0, 0, ribbon->width(), ribbon->height())).right()
                < third->mapToScene(first.toRectF().topLeft()).x() - 4);
        app.reader.closeBook();
        QSettings().remove("reader");
    }

    void readerHighlightsWhatIsDraggedOver() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        const QString first = "First has plain words in it, enough of them to wrap across a few lines.";
        const QString second = "Second has more words in it, enough of them to wrap across a few lines.";
        QVERIFY(app.reader.addText("Drag", first + "\n\n" + second));
        QTRY_COMPARE(app.items("bookParagraph").size(), 2);
        auto paragraph = [&](int index) -> QQuickItem * {
            for (QQuickItem *item : app.items("bookParagraph"))
                if (item->property("index").toInt() == index)
                    return item;
            return nullptr;
        };
        // Where a character is on screen.
        auto point = [&](int index, int position) {
            QQuickItem *item = paragraph(index);
            QVariant r;
            QMetaObject::invokeMethod(item, "rectFor", Q_RETURN_ARG(QVariant, r), Q_ARG(QVariant, position));
            return item->mapToScene(r.toRectF().center()).toPoint();
        };

        // From inside "plain" in the first paragraph to inside "more" in the second.
        const QPoint from = point(0, int(first.indexOf("plain")) + 2);
        const QPoint to = point(1, int(second.indexOf("more")) + 2);
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        QTest::mouseMove(app.window, from + QPoint(20, 0));
        QTest::mouseMove(app.window, (from + to) / 2);
        QTest::mouseMove(app.window, to);
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, to);
        QObject *menu = app.window->findChild<QObject *>("selectionMenu");
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QObject *list = app.window->findChild<QObject *>("chapterList");
        QVERIFY(list->property("hasSelection").toBool());
        QCOMPARE(list->property("selectionStartParagraph").toInt(), 0);
        QCOMPARE(list->property("selectionEndParagraph").toInt(), 1);
        app.grab("notes-selection");

        QQuickItem *highlight = app.items("highlightSelection").value(0);
        QVERIFY(highlight);
        QTest::mouseClick(app.window, Qt::LeftButton, {},
                          highlight->mapToScene(QPointF(highlight->width() / 2, highlight->height() / 2)).toPoint());
        QTRY_COMPARE(app.annotations.count(), 1);
        QCOMPARE(app.annotations.items().first().toMap().value("text").toString(),
                 first.mid(first.indexOf("plain")) + "\nSecond has more");
        QTRY_VERIFY(!menu->property("opened").toBool());
        QVERIFY(!list->property("hasSelection").toBool());

        // A click on the highlight opens its note, kept as it closes.
        QObject *editor = app.window->findChild<QObject *>("noteEditor");
        QVERIFY(editor);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, point(1, 2));
        QTRY_VERIFY(editor->property("opened").toBool());
        QQuickItem *note = app.items("noteText").value(0);
        QTRY_VERIFY(note && note->hasActiveFocus());
        for (const char c : QByteArray("mind the tern."))
            QTest::keyClick(app.window, c);
        app.grab("notes-editor");
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!editor->property("opened").toBool());
        QCOMPARE(app.annotations.items().first().toMap().value("note").toString(), QString("mind the tern."));
        app.reader.closeBook();
        QSettings().remove("reader");
    }

    void notesPanelListsWhatWasMarked() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        QVERIFY(app.reader.addText("Panel", "First paragraph to mark.\n\nSecond paragraph to bookmark."));
        QTRY_COMPARE(app.items("bookParagraph").size(), 2);
        const QString id = app.annotations.addHighlight(0, 0, 0, 5);
        app.annotations.setNote(id, "A note on it.");
        app.reader.goTo(0, 1, 0);
        app.reader.toggleBookmark();

        // N opens the drawer on the notes, listed in reading order.
        QObject *panel = app.window->findChild<QObject *>("contentsPanel");
        QVERIFY(panel);
        QTest::keyClick(app.window, Qt::Key_N);
        QTRY_VERIFY(panel->property("opened").toBool());
        QCOMPARE(panel->property("tab").toInt(), 1);
        QTRY_COMPARE(app.items("removeAnnotation").size(), 2);
        app.grab("notes-panel");

        // Removing one from the list removes it everywhere.
        QQuickItem *remove = app.items("removeAnnotation").last();
        QTest::mouseClick(app.window, Qt::LeftButton, {},
                          remove->mapToScene(QPointF(remove->width() / 2, remove->height() / 2)).toPoint());
        QTRY_COMPARE(app.annotations.count(), 1);
        QVERIFY(!app.reader.bookmarked());

        // T shows the contents in the same drawer, and T again closes it.
        QTest::keyClick(app.window, Qt::Key_T);
        QTRY_COMPARE(panel->property("tab").toInt(), 0);
        QVERIFY(panel->property("opened").toBool());
        QTest::keyClick(app.window, Qt::Key_T);
        QTRY_VERIFY(!panel->property("opened").toBool());
        app.reader.closeBook();
        QSettings().remove("reader");
    }

    void searchPanelFindsAMatchAndMarksIt() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        const QString path = dir.filePath("search.txt");
        write(path, "search");
        Book book;
        book.path = path;
        book.title = "Search";
        book.format = "txt";
        QList<Paragraph> filler;
        for (int i = 0; i < 30; ++i)
            filler.append({"Plain words that fill the page and go on for a while, so the chapter is longer than "
                           "the window.", 0, {}});
        QList<Paragraph> second = filler;
        second.append({"At last the lighthouse keeper climbed the stairs.", 0, {}});
        book.chapters = {{"Shore", filler}, {"Tower", second}};
        book.segment();
        app.reader.openBook(std::make_shared<Book>(book));
        QTRY_VERIFY(!app.items("bookParagraph").isEmpty());

        // The button in the title bar draws its glass inside its circle.
        QQuickItem *button = app.items("searchButton").value(0);
        QVERIFY(button);
        const QImage bar = app.grab("search-button");
        const QRect glass = brightBoxInCircle(bar, app.area(bar, button), 150);
        QVERIFY2(!glass.isEmpty(), "the magnifying glass isn't drawn");
        const QPointF centre = QRectF(app.area(bar, button)).center();
        QVERIFY2(qAbs(QRectF(glass).center().x() - centre.x()) <= 2 && qAbs(QRectF(glass).center().y() - centre.y()) <= 2,
                 qPrintable(QString("the glass is off centre: %1,%2 in a button centred on %3,%4")
                                .arg(QRectF(glass).center().x()).arg(QRectF(glass).center().y())
                                .arg(centre.x()).arg(centre.y())));
        if (!qEnvironmentVariableIsEmpty("OMAREADER_SCREENSHOTS"))
            bar.copy(app.area(bar, button)).scaled(app.area(bar, button).size() * 8)
                .save(qEnvironmentVariable("OMAREADER_SCREENSHOTS") + "/search-button-zoom.png");

        // / opens the drawer on the search, with the words to type in focus.
        QObject *panel = app.window->findChild<QObject *>("contentsPanel");
        QVERIFY(panel);
        QTest::keyClick(app.window, Qt::Key_Slash);
        QTRY_VERIFY(panel->property("opened").toBool());
        QCOMPARE(panel->property("tab").toInt(), 2);
        QQuickItem *field = app.items("searchField").value(0);
        QTRY_VERIFY(field && field->hasActiveFocus());
        app.grab("search-empty");
        // Letters that are shortcuts elsewhere go into the field.
        for (const char c : QByteArray("lighthouse"))
            QTest::keyClick(app.window, c);
        QCOMPARE(field->property("text").toString(), QString("lighthouse"));
        QVERIFY(panel->property("opened").toBool());
        QTRY_COMPARE(app.reader.searchCount(), 1);
        QTRY_COMPARE(app.items("searchResult").size(), 1);
        app.grab("search-results");

        // Return goes to it and marks it in the text, in view.
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(!panel->property("opened").toBool());
        QCOMPARE(app.reader.chapter(), 1);
        QCOMPARE(app.reader.match().value("paragraph").toInt(), 30);
        QQuickItem *found = nullptr;
        QTRY_VERIFY((found = [&]() -> QQuickItem * {
            for (QQuickItem *item : app.items("bookParagraph"))
                if (item->property("index").toInt() == 30)
                    return item;
            return nullptr;
        }()));
        QQuickItem *list = app.window->findChild<QQuickItem *>("chapterList");
        QVERIFY(list);
        const QImage marked = app.grab("search-match");
        QVariant rect;
        QMetaObject::invokeMethod(found, "rectFor", Q_RETURN_ARG(QVariant, rect), Q_ARG(QVariant, 9));
        const QRectF start = rect.toRectF();
        QMetaObject::invokeMethod(found, "rectFor", Q_RETURN_ARG(QVariant, rect), Q_ARG(QVariant, 19));
        const QRectF end = rect.toRectF();
        const QRectF word = found->mapRectToScene(QRectF(start.x(), start.y(), end.x() - start.x(), start.height()));
        const QRectF view = list->mapRectToScene(QRectF(0, 0, list->width(), list->height()));
        QVERIFY2(view.contains(word), "the match isn't in view");
        // The match is tinted; the words before it aren't. Compared with the
        // same text once the mark is put away.
        app.reader.hideMatch();
        const QImage plain = app.grab("search-match-hidden");
        auto changed = [&](const QRectF &r) {
            const qreal ratio = marked.devicePixelRatio();
            int count = 0;
            for (int y = qRound(r.top() * ratio); y < qRound(r.bottom() * ratio); ++y)
                for (int x = qRound(r.left() * ratio); x < qRound(r.right() * ratio); ++x) {
                    const QColor a = marked.pixelColor(x, y), b = plain.pixelColor(x, y);
                    count += qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue()) > 30;
                }
            return count;
        };
        const int inside = changed(word);
        const int before = changed(word.translated(-word.width() - 4, 0));
        QVERIFY2(inside > word.width() * word.height() * 0.5,
                 qPrintable(QString("only %1 pixels changed on the match").arg(inside)));
        QCOMPARE(before, 0);
        if (!qEnvironmentVariableIsEmpty("OMAREADER_SCREENSHOTS"))
            marked.copy(word.adjusted(-80, -20, 80, 20).toRect()).scaled(word.adjusted(-80, -20, 80, 20).size().toSize() * 3)
                .save(qEnvironmentVariable("OMAREADER_SCREENSHOTS") + "/search-match-zoom.png");

        // The view settling there leaves the reading place on the match.
        QTest::qWait(600);
        QCOMPARE(app.reader.sentence(), book.sentenceAt(1, 30, 9));
        // Moved away without a key or a flick, as the scroll bar does, the
        // reading place follows the view again.
        list->setProperty("contentY", list->property("originY").toReal());
        QTRY_VERIFY(app.reader.sentence() < book.sentenceAt(1, 5, 0));
        app.reader.showMatch(0);
        QTest::qWait(600);
        QCOMPARE(app.reader.sentence(), book.sentenceAt(1, 30, 9));

        // F3 brings the mark back on the same match, the only one, and
        // Escape takes it away.
        QTest::keyClick(app.window, Qt::Key_F3);
        QTRY_COMPARE(app.reader.match().value("paragraph").toInt(), 30);
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(app.reader.match().isEmpty());
        QTest::keyClick(app.window, Qt::Key_F3);
        QTRY_COMPARE(app.reader.match().value("paragraph").toInt(), 30);

        // Shift+Return in the field reads aloud from the match. The tests'
        // home has no voice, so it only gets as far as saying so.
        QTest::keyClick(app.window, Qt::Key_Slash);
        QTRY_VERIFY(panel->property("opened").toBool());
        QTRY_VERIFY(field->hasActiveFocus());
        app.reader.goToChapter(0);
        QTest::keyClick(app.window, Qt::Key_Return, Qt::ShiftModifier);
        QTRY_VERIFY(!panel->property("opened").toBool());
        QCOMPARE(app.reader.chapter(), 1);
        QCOMPARE(app.reader.sentence(), book.sentenceAt(1, 30, 9));
        app.narrator.stop();
        app.narrator.clearError();
        app.reader.closeBook();
        QSettings().remove("reader");
    }

    void searchResultsShowChapterTitlesAsText() {
        // A heading that looks like markup must not load anything when its
        // chapter turns up in the search results.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        const QString path = dir.filePath("markup.txt");
        write(path, "markup");
        Book book;
        book.path = path;
        book.title = "Markup";
        book.format = "txt";
        const QString title = QString("<img src=\"http://127.0.0.1:%1/x.png\">Tower").arg(server.serverPort());
        book.chapters = {{"Shore", {{"Nothing here.", 0, {}}}}, {title, {{"The lighthouse stands.", 0, {}}}}};
        book.segment();
        app.reader.openBook(std::make_shared<Book>(book));
        QObject *panel = app.window->findChild<QObject *>("contentsPanel");
        QVERIFY(panel);
        QTest::keyClick(app.window, Qt::Key_Slash);
        QTRY_VERIFY(panel->property("opened").toBool());
        app.reader.search("lighthouse");
        QTRY_COMPARE(app.items("searchResult").size(), 1);
        bool shown = false;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
            if (item->property("text").toString() == title)
                shown = true;
            for (QQuickItem *child : item->childItems())
                walk(child);
        };
        walk(app.items("searchResult").first());
        QVERIFY2(shown, "the title isn't shown as it is written");
        QTest::keyClick(app.window, Qt::Key_T); // the contents list the title too
        // The event loop runs meanwhile, as an image request needs it to.
        QTest::qWait(1000);
        QVERIFY2(!server.hasPendingConnections(), "a chapter title loaded something from the network");
        app.reader.closeBook();
    }

    void helpShowsEveryShortcutOnScreen() {
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        app.window->setProperty("helpVisible", true);
        QTRY_VERIFY(!app.items("helpOverlay").isEmpty());
        QQuickItem *overlay = app.items("helpOverlay").first();
        // Every row, the last included, is inside the window with room to
        // spare, and no two overlap.
        auto check = [&](const QString &name) {
            app.grab(name);
            QList<QRectF> labels;
            std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
                if (item->isVisible() && QByteArray(item->metaObject()->className()).contains("Label")) {
                    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
                    QVERIFY2(r.top() >= 16 && r.bottom() <= app.window->height() - 16 && r.left() >= 16
                                 && r.right() <= app.window->width() - 16,
                             qPrintable(item->property("text").toString() + " is off the window"));
                    for (const QRectF &other : std::as_const(labels))
                        QVERIFY2(!other.intersects(r), qPrintable(item->property("text").toString() + " overlaps another row"));
                    labels.append(r);
                }
                for (QQuickItem *child : item->childItems())
                    walk(child);
            };
            walk(overlay);
            QVERIFY(labels.size() > 50);
        };
        check("help");
        // A narrower window that is tall enough still shows them all, in one list.
        app.window->resize(900, 1100);
        check("help-narrow");
    }

    void everyNotesStringHasAPortugueseTranslation() {
        QFile file(QString(OMAREADER_SOURCE) + "/src/i18n/pt-BR.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject catalog = QJsonDocument::fromJson(file.readAll()).object();
        static const QRegularExpression call("\\b(?:qsTr|tr)\\(\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
        int found = 0;
        for (const QString name : {"src/ContentsPanel.qml", "src/NoteEditor.qml", "src/portalfilepicker.cpp",
                                   "src/ReaderView.qml", "src/HelpOverlay.qml"}) {
            QFile source(QString(OMAREADER_SOURCE) + "/" + name);
            QVERIFY(source.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(source.readAll());
            for (auto it = call.globalMatch(text); it.hasNext();) {
                QString key = it.next().captured(1);
                key.replace("\\\"", "\"").replace("\\n", "\n");
                ++found;
                QVERIFY2(catalog.contains(key), qPrintable(name + ": no pt-BR entry for: " + key));
            }
        }
        QVERIFY(found > 20);
    }

    void focusModeDimsAllButTheVoice() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        // The colour highlight, whose marks the counts below allow for.
        app.reader.setHighlight("both");
        QVERIFY(app.show());
        // Three paragraphs; the middle one has two sentences.
        const QString words = " has plain words in it, enough of them to wrap across a few lines of the column.";
        QVERIFY(app.reader.addText("Focus", "First" + words + "\n\nSecond" + words + " Third" + words + "\n\nFourth" + words));
        QTRY_VERIFY(app.reader.hasBook());
        QTRY_COMPARE(app.items("bookParagraph").size(), 3);
        // The mouse waits below the text, where it raises no tooltip over it.
        QTest::mouseMove(app.window, QPoint(550, 500));
        // The voice is on the fifth word of the second paragraph's first sentence.
        app.narrator.showForTests(Narrator::Paused, 1, 4);
        QCOMPARE(app.narrator.paragraph(), 1);

        auto light = [&](const QString &name) {
            const QImage shot = app.grab(name);
            QList<int> counts(3);
            for (QQuickItem *item : app.items("bookParagraph"))
                counts[item->property("index").toInt()] = brightPixels(shot, app.area(shot, item));
            return counts;
        };
        const QList<int> plain = light("focus-off");
        for (int count : plain)
            QVERIFY(count > 500);

        app.reader.setFocusMode(true);
        app.reader.setFadeRead(false);
        const QList<int> paragraph = light("focus-spotlight-paragraph");
        QVERIFY2(paragraph[0] < plain[0] / 10 && paragraph[2] < plain[2] / 10,
                 qPrintable(QString("around the voice: %1 and %2 of %3 and %4").arg(paragraph[0]).arg(paragraph[2]).arg(plain[0]).arg(plain[2])));
        QVERIFY2(paragraph[1] > plain[1] * 0.95, qPrintable(QString("%1 of %2").arg(paragraph[1]).arg(plain[1])));

        // The sentence spotlight quiets the paragraph's second sentence too.
        app.reader.setSpotlight("sentence");
        const QList<int> sentence = light("focus-spotlight-sentence");
        QVERIFY2(sentence[1] < paragraph[1] * 0.75 && sentence[1] > paragraph[1] * 0.25,
                 qPrintable(QString("%1 of %2").arg(sentence[1]).arg(paragraph[1])));

        // The trail fades what was read, and leaves what is to come.
        app.reader.setSpotlight("off");
        app.reader.setFadeRead(true);
        const QList<int> trail = light("focus-trail");
        QVERIFY(trail[0] < plain[0] / 10);
        QVERIFY(trail[2] > plain[2] * 0.95);
        QVERIFY2(trail[1] < plain[1] * 0.95 && trail[1] > plain[1] * 0.6,
                 qPrintable(QString("%1 of %2").arg(trail[1]).arg(plain[1])));

        // Without narration, nothing dims (and the highlight goes too).
        app.narrator.stop();
        const QList<int> stopped = light("focus-stopped");
        QVERIFY(stopped[0] > plain[0] * 0.95);
        QVERIFY(stopped[1] > plain[1] * 0.7);
        QVERIFY(stopped[2] > plain[2] * 0.95);
        app.reader.closeBook();
        QSettings().remove("reader");
    }

    void highlightStylesChangeWhatIsMarked() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        app.reader.setHighlight("both");
        QVERIFY(app.show());
        QVERIFY(app.reader.addText("Styles", "One sentence with plain words in it to read aloud. And a second one after it."));
        QTRY_COMPARE(app.items("bookParagraph").size(), 1);
        app.narrator.showForTests(Narrator::Paused, 0, 3);
        QQuickItem *paragraph = app.items("bookParagraph").first();

        // How many pixels are filled with the accent, and how many are tinted with it.
        auto marks = [&](const QString &name) {
            const QImage shot = app.grab(name);
            const QRect area = app.area(shot, paragraph);
            int solid = 0, tint = 0;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const QColor c = shot.pixelColor(x, y);
                    solid += c.red() > 200 && c.green() > 170 && c.blue() < 80;
                    tint += c.red() > 25 && c.red() < 120 && c.red() > c.blue() + 12;
                }
            return std::pair{solid, tint};
        };
        const auto [boxed, tinted] = marks("highlight-both");
        QVERIFY(boxed > 300);
        QVERIFY(tinted > 1000);

        app.reader.setHighlight("sentence");
        const auto [sentenceSolid, sentenceTint] = marks("highlight-sentence");
        QVERIFY2(sentenceSolid < 20, qPrintable(QString::number(sentenceSolid)));
        QVERIFY(sentenceTint > 1000);

        app.reader.setHighlight("word");
        const auto [wordSolid, wordTint] = marks("highlight-word");
        QVERIFY(wordSolid > 300);
        QVERIFY2(wordTint < tinted / 4, qPrintable(QString("%1 of %2").arg(wordTint).arg(tinted)));

        // A subtle word is written in the accent, not boxed in it.
        app.reader.setHighlightStrength("subtle");
        const auto [subtleSolid, subtleTint] = marks("highlight-word-subtle");
        Q_UNUSED(subtleTint);
        QVERIFY2(subtleSolid > 10 && subtleSolid < boxed / 3, qPrintable(QString("%1 of %2").arg(subtleSolid).arg(boxed)));

        // A strong sentence is tinted more deeply than a normal one.
        auto sentenceShade = [&](const QString &strength) {
            app.reader.setHighlight("sentence");
            app.reader.setHighlightStrength(strength);
            const QImage shot = app.grab("highlight-sentence-" + strength);
            const QRect area = app.area(shot, paragraph);
            int deepest = 0;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.left() + area.width() / 2; ++x) {
                    const QColor c = shot.pixelColor(x, y);
                    if (c.red() < 120)
                        deepest = qMax(deepest, c.red() - c.blue());
                }
            return deepest;
        };
        const int subtle = sentenceShade("subtle"), normal = sentenceShade("normal"), strong = sentenceShade("strong");
        QVERIFY2(subtle < normal && normal < strong, qPrintable(QString("%1 %2 %3").arg(subtle).arg(normal).arg(strong)));
        app.reader.closeBook();
        QSettings().remove("reader");
    }

    void glassWindowGlidesOverTheSpokenWord() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        const QString text = "The voice reads these plain words aloud while a glass window glides along the line over "
                             "each one of them in turn, and a long word like pneumonoultramicroscopicsilicovolcanoconiosis stretches it, and "
                             "then the reading goes on across many more lines of the column until it comes to an end";
        QVERIFY(app.reader.addText("Glass", text));
        QTRY_COMPARE(app.items("bookParagraph").size(), 1);
        QCOMPARE(app.reader.highlight(), QString("glass"));
        QTest::mouseMove(app.window, QPoint(550, 700));

        // The glass sits on the paragraph's text, so its rect is the text's.
        auto glass = [&]() { return app.items("readingGlass").value(0); };
        auto speak = [&](int word) {
            app.narrator.showForTests(Narrator::Paused, 0, word);
            QTRY_VERIFY(glass() && glass()->property("settled").toBool());
        };
        auto panes = [&]() {
            QList<QRectF> rects;
            for (QQuickItem *pane : app.items("readingGlassPane"))
                rects << pane->mapRectToScene(QRectF(0, 0, pane->width(), pane->height()));
            std::sort(rects.begin(), rects.end(), [](const QRectF &a, const QRectF &b) { return a.y() < b.y(); });
            return rects;
        };
        auto spoken = [&]() { return text.mid(app.narrator.wordStart(), app.narrator.wordEnd() - app.narrator.wordStart()); };

        // Over an ordinary word, one pane centred on it, wide enough for it
        // and two words either side.
        speak(4);
        QCOMPARE(spoken(), QString("plain"));
        QList<QRectF> one = panes();
        QCOMPARE(one.size(), 1);
        const qreal width = one.first().width();
        // Centred on the word: the spoken word is halfway across.
        auto across = [&]() {
            QQuickItem *g = glass();
            const qreal w = g->property("paneWidth").toReal();
            return (g->property("targetU").toReal() - (g->property("u").toReal() - w / 2)) / w;
        };
        QVERIFY2(qAbs(across() - 0.5) < 0.02, qPrintable(QString::number(across())));
        const QRectF text4 = glass()->mapRectToScene(QRectF(0, 0, glass()->width(), glass()->height()));
        QVERIFY2(width > app.reader.fontSize() * 10 && width < app.reader.fontSize() * 18, qPrintable(QString::number(width)));

        // The colour highlight is gone: no accent box, and the words in the
        // pane are drawn bright.
        QImage shot = app.grab("glass-plain");
        QQuickItem *paragraph = app.items("bookParagraph").first();
        const QRect area = app.area(shot, paragraph);
        int accent = 0;
        for (int y = area.top(); y <= area.bottom(); ++y)
            for (int x = area.left(); x <= area.right(); ++x) {
                const QColor c = shot.pixelColor(x, y);
                accent += c.red() > 200 && c.green() > 170 && c.blue() < 80;
            }
        QVERIFY2(accent < 20, qPrintable(QString::number(accent)));
        const qreal ratio = shot.devicePixelRatio();
        const QRectF paneRect = one.first();
        const QRect inPane(qRound(paneRect.x() * ratio), qRound(paneRect.y() * ratio), qRound(paneRect.width() * ratio), qRound(paneRect.height() * ratio));
        QVERIFY2(brightPixels(shot, inPane, 200) > 60, qPrintable(QString::number(brightPixels(shot, inPane, 200))));
        if (!qEnvironmentVariableIsEmpty("OMAREADER_SCREENSHOTS"))
            shot.copy(inPane.adjusted(-40, -30, 40, 30)).scaled(inPane.width() * 3 + 240, inPane.height() * 3 + 180)
                .save(qEnvironmentVariable("OMAREADER_SCREENSHOTS") + "/glass-plain-zoom.png");

        // It keeps its size from word to word, and glides to the next one
        // rather than jumping there.
        const qreal from = one.first().center().x();
        app.narrator.showForTests(Narrator::Paused, 0, 5);
        QList<qreal> path;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 1500 && !(glass()->property("settled").toBool() && path.size() > 1)) {
            QTest::qWait(10);
            if (!panes().isEmpty())
                path << panes().first().center().x();
        }
        const qreal to = panes().first().center().x();
        QVERIFY(to > from + 10);
        QVERIFY2(std::any_of(path.begin(), path.end(), [&](qreal x) { return x > from + 3 && x < to - 3; }),
                 "the pane never passed between the two words");
        QCOMPARE(panes().first().width(), width);

        // While the voice plays, it moves on with the voice, toward the next
        // word before that word has even started, and has it inside once it
        // is spoken.
        speak(4);
        const qreal still = panes().first().center().x();
        app.narrator.showForTests(Narrator::Playing, 0, 4, 2.0);
        QTest::qWait(1000);
        const qreal halfway = panes().first().center().x();
        QVERIFY2(halfway > still + 10 && halfway < to - 5,
                 qPrintable(QString("from %1 toward %2, at %3 after half the word").arg(still).arg(to).arg(halfway)));
        QTest::qWait(1200);
        const QRectF later = panes().first();
        QVERIFY2(later.left() < to - app.reader.fontSize() && later.right() > to + app.reader.fontSize(),
                 qPrintable(QString("the next word at %1, the pane from %2 to %3").arg(to).arg(later.left()).arg(later.right())));
        app.narrator.showForTests(Narrator::Paused, 0, 4);

        // Only a word too long for it stretches it.
        int word = 0;
        for (; word < 60; ++word) {
            app.narrator.showForTests(Narrator::Paused, 0, word);
            if (spoken() == "pneumonoultramicroscopicsilicovolcanoconiosis")
                break;
        }
        QCOMPARE(spoken(), QString("pneumonoultramicroscopicsilicovolcanoconiosis"));
        speak(word);
        QVERIFY2(panes().first().width() > width + 10, qPrintable(QString("%1 and %2").arg(panes().first().width()).arg(width)));
        app.grab("glass-long-word");

        // On the first word of a line, the pane is still partly on the end
        // of the line before: it slides out of one line into the next.
        bool straddled = false;
        for (int w = 1; w < 50 && !straddled; ++w) {
            app.narrator.showForTests(Narrator::Paused, 0, w);
            if (app.narrator.wordStart() < 0)
                break;
            speak(w);
            const QList<QRectF> two = panes();
            if (two.size() != 2)
                continue;
            straddled = true;
            app.grab("glass-line-break");
            // The upper part runs past the end of its line, the lower one in
            // from before the start of its own, a line apart.
            QVERIFY2(two[0].right() > text4.right(), qPrintable(QString("%1 and %2").arg(two[0].right()).arg(text4.right())));
            QVERIFY2(two[1].left() < text4.left(), qPrintable(QString("%1 and %2").arg(two[1].left()).arg(text4.left())));
            QVERIFY(two[1].y() - two[0].y() > app.reader.fontSize());
        }
        QVERIFY2(straddled, "the pane never crossed a line break");

        // The colour highlight is still there for those who want it.
        app.reader.setHighlight("both");
        QTRY_VERIFY(app.items("readingGlass").isEmpty());
        app.narrator.stop();
        app.reader.closeBook();
        QSettings().remove("reader");
    }

    void glassWindowKeepsASteadyPace() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        const QString text = "The voice reads these plain words aloud at its own pace, quick over the short ones and slow "
                             "over others, and it stops for a moment at each comma. Then it goes on again across the "
                             "lines of the column, word after word, while the eyes follow the glass along. It pauses "
                             "at the end of every sentence, and starts the next one a little later than the last.";
        QVERIFY(app.reader.addText("Pace", text));
        QTRY_COMPARE(app.items("bookParagraph").size(), 1);
        QTest::mouseMove(app.window, QPoint(550, 700));
        auto glass = [&]() { return app.items("readingGlass").value(0); };

        // Speech is uneven: some words go by quickly, others take three or
        // four times as long, and a comma or a full stop holds the voice.
        const QList<double> pattern = {0.12, 0.14, 0.45, 0.12, 0.3, 0.16, 0.12, 0.6, 0.15, 0.25};
        struct Sample { qint64 ms; qreal u, spoken, width, half; };
        QList<Sample> samples;
        QElapsedTimer clock;
        clock.start();
        int sentence = 0, word = 0, step = 0;
        while (clock.elapsed() < 7000) {
            app.narrator.showForTests(Narrator::Playing, sentence, word, pattern.at(step % pattern.size()));
            if (app.narrator.wordStart() < 0) {
                ++sentence;
                word = 0;
                if (sentence > 3)
                    break;
                continue;
            }
            QElapsedTimer wordClock;
            wordClock.start();
            while (wordClock.elapsed() < qint64(pattern.at(step % pattern.size()) * 1000)) {
                QTest::qWait(20);
                if (QQuickItem *g = glass())
                    samples << Sample{clock.elapsed(), g->property("u").toReal(), g->property("targetU").toReal(),
                                      g->property("paneWidth").toReal(), g->property("targetHalf").toReal()};
            }
            ++word;
            ++step;
        }
        app.narrator.showForTests(Narrator::Paused, 0, 0);
        QVERIFY(samples.size() > 150);

        // The spoken word is always well inside the pane, never at or past its edge.
        for (const Sample &s : samples)
            QVERIFY2(qAbs(s.spoken - s.u) + s.half < s.width / 2 - 4,
                     qPrintable(QString("at %1 ms the word at %2 (half %3) and the pane at %4, %5 wide")
                                    .arg(s.ms).arg(s.spoken).arg(s.half).arg(s.u).arg(s.width)));

        // After the first second, the pane moves on at an even pace while the
        // voice's own pace swings from word to word, and never goes back.
        auto speeds = [&](auto value) {
            QList<qreal> out;
            int from = 0;
            while (from < samples.size() && samples[from].ms < 1500)
                ++from;
            for (int i = from; i < samples.size(); ++i) {
                int j = i;
                while (j < samples.size() && samples[j].ms - samples[i].ms < 250)
                    ++j;
                if (j >= samples.size())
                    break;
                out << (value(samples[j]) - value(samples[i])) * 1000.0 / (samples[j].ms - samples[i].ms);
                i = j - 1;
            }
            return out;
        };
        auto spread = [](const QList<qreal> &values) {
            qreal mean = 0, var = 0;
            for (qreal v : values) mean += v;
            mean /= values.size();
            for (qreal v : values) var += (v - mean) * (v - mean);
            return std::sqrt(var / values.size()) / mean;
        };
        const QList<qreal> pane = speeds([](const Sample &s) { return s.u; });
        const QList<qreal> voice = speeds([](const Sample &s) { return s.spoken; });
        QVERIFY(pane.size() >= 15);
        for (int i = 1; i < samples.size(); ++i)
            QVERIFY2(samples[i].u >= samples[i - 1].u - 0.5, qPrintable(QString("went back at %1 ms").arg(samples[i].ms)));
        QVERIFY2(spread(pane) < 0.3 && spread(pane) < spread(voice) / 2,
                 qPrintable(QString("the pane's pace varies by %1, the voice's by %2").arg(spread(pane)).arg(spread(voice))));
        app.reader.closeBook();
        QSettings().remove("reader");
    }

    void glassWindowSitsOnTheLineItCovers() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        // The paragraph opens with a mark drawn from a taller fallback font,
        // as checklists in some books do, so its first line is taller than
        // the others.
        const QString text = QString::fromUtf8(
            "✓ Executive functions operate together but can cause impairments separately. Other scientists and I "
            "have divided executive functioning into seven separate abilities to understand them better. But we "
            "humans don’t experience them as separate, nor do we use them one at a time as adults. The executive "
            "functions operate like the sections of a symphony orchestra, playing simultaneously to produce seamlessly "
            "beautiful music. When ADHD enters the picture, so do deficits in executive functions.");
        QVERIFY(app.reader.addText("Lines", text));
        QTRY_COMPARE(app.items("bookParagraph").size(), 1);
        QTest::mouseMove(app.window, QPoint(550, 700));
        auto glass = [&]() { return app.items("readingGlass").value(0); };

        QSet<int> tops;
        QList<qreal> firstLines;
        for (int sentence = 0; sentence < 5; ++sentence) {
            // A word whose pane sits on one line; where the lines break
            // depends on the fonts installed, so near a line's end the next
            // word is tried.
            QList<QQuickItem *> panes;
            for (int word = 2; word < 12 && panes.size() != 1; ++word) {
                app.narrator.showForTests(Narrator::Paused, sentence, word);
                QTRY_VERIFY(glass() && glass()->property("settled").toBool());
                panes = app.items("readingGlassPane");
            }
            QCOMPARE(panes.size(), 1);
            // The page's lines keep their places as the voice moves on: the
            // highlight changes colours, never where a line is.
            QList<qreal> lines;
            for (const QVariant &line : glass()->property("lines").toList())
                lines << line.toMap().value("y").toReal();
            QVERIFY(lines.size() >= 5);
            if (sentence == 0)
                firstLines = lines;
            auto join = [](const QList<qreal> &values) {
                QStringList out;
                for (qreal v : values)
                    out << QString::number(v);
                return out.join(",");
            };
            QVERIFY2(lines == firstLines, qPrintable(QString("sentence %1: lines at %2, were at %3").arg(sentence).arg(join(lines)).arg(join(firstLines))));
            const QImage shot = app.grab(QString("glass-line-%1").arg(sentence));
            const QRect pane = app.area(shot, panes.first());
            tops << pane.top();
            // A pane about as tall as a line of letters, not the taller first line.
            QVERIFY2(pane.height() < app.reader.fontSize() * 2.2 * shot.devicePixelRatio(),
                     qPrintable(QString("sentence %1: the pane is %2 px tall").arg(sentence).arg(pane.height())));
            // The words in it sit inside it, not cut off by its top or bottom edge.
            int textTop = -1, textBottom = -1;
            for (int y = pane.top(); y <= pane.bottom(); ++y)
                for (int x = pane.left() + 12; x <= pane.right() - 12; ++x)
                    if (qGray(shot.pixel(x, y)) > 200) {
                        if (textTop < 0)
                            textTop = y;
                        textBottom = y;
                        break;
                    }
            QVERIFY2(textTop >= 0, qPrintable(QString("sentence %1: no words in the pane").arg(sentence)));
            QVERIFY2(textTop - pane.top() >= 2 && pane.bottom() - textBottom >= 2,
                     qPrintable(QString("sentence %1: pane %2 to %3, words %4 to %5").arg(sentence)
                                    .arg(pane.top()).arg(pane.bottom()).arg(textTop).arg(textBottom)));
        }
        QVERIFY(tops.size() >= 3);
        app.reader.closeBook();
        QSettings().remove("reader");
    }

    void focusModeHoldsTheSpokenLineSteady() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        // The word's accent box marks where the spoken line is drawn.
        app.reader.setHighlight("both");
        QVERIFY(app.show());
        QStringList paragraphs;
        for (int i = 0; i < 40; ++i)
            paragraphs << QString("Paragraph %1 has plain words in it, enough of them to wrap across a few lines.").arg(i + 1);
        QVERIFY(app.reader.addText("Steady", paragraphs.join("\n\n")));
        QTRY_VERIFY(!app.items("bookParagraph").isEmpty());
        QQuickItem *list = app.items("chapterList").value(0);
        QVERIFY(list);

        // Where the spoken word's box is drawn, from the top of the text.
        auto wordTop = [&](int sentence, const QString &name) {
            app.narrator.showForTests(Narrator::Paused, sentence, 2);
            QTest::qWait(600);
            const QImage shot = app.grab(name);
            const QRect area = app.area(shot, list);
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const QColor c = shot.pixelColor(x, y);
                    if (c.red() > 200 && c.green() > 170 && c.blue() < 80)
                        return (y - area.top()) / shot.devicePixelRatio();
                }
            return -1.0;
        };
        // Without it, the view jumps when the voice nears the edge, so the
        // word lands at different heights.
        const qreal jumpA = wordTop(12, "steady-off-a"), jumpB = wordTop(14, "steady-off-b");
        QVERIFY(jumpA >= 0 && jumpB >= 0);
        QVERIFY2(qAbs(jumpA - jumpB) > 20, qPrintable(QString("%1 and %2").arg(jumpA).arg(jumpB)));

        app.reader.setFocusMode(true);
        const qreal line = list->height() * 0.33;
        for (int sentence : {12, 14, 21, 22}) {
            const qreal top = wordTop(sentence, QString("steady-%1").arg(sentence));
            QVERIFY2(qAbs(top - line) < app.reader.fontSize() * 2,
                     qPrintable(QString("sentence %1 at %2, the line is at %3").arg(sentence).arg(top).arg(line)));
        }
        app.narrator.stop();
        app.reader.closeBook();
        QSettings().remove("reader");
    }

    void focusModeHidesTheBarsWhileTheVoicePlays() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        // The glass would draw into the corner where a stray tooltip is looked for.
        app.reader.setHighlight("both");
        QVERIFY(app.show());
        QVERIFY(app.reader.addText("Quiet", "Words to read aloud while the bars step away."));
        QTRY_COMPARE(app.items("bookParagraph").size(), 1);
        QQuickItem *title = app.items("titleBar").value(0);
        QQuickItem *bar = app.items("narrationBar").value(0);
        QVERIFY(title && bar);
        auto shows = [&](QQuickItem *item, const QString &name) {
            const QImage shot = app.grab(name);
            return brightPixels(shot, app.area(shot, item), 100);
        };
        const int titleShown = shows(title, "bars-shown"), barShown = shows(bar, "bars-shown");
        QVERIFY(titleShown > 50 && barShown > 50);

        // Off, they stay, even while the voice plays.
        app.narrator.showForTests(Narrator::Playing, 0, 1);
        QTest::qWait(3000);
        QCOMPARE(title->opacity(), 1.0);

        // On, they go once the mouse and keys have been still a moment...
        app.reader.setFocusMode(true);
        QTRY_COMPARE_WITH_TIMEOUT(title->opacity(), 0.0, 5000);
        QTRY_COMPARE(bar->opacity(), 0.0);
        QVERIFY(shows(title, "bars-hidden") < 5);
        QVERIFY(shows(bar, "bars-hidden") < 5);
        // A tooltip the resting mouse raised goes with them.
        const QImage hidden = app.grab("bars-hidden");
        QVERIFY(brightPixels(hidden, QRect(0, 0, 200 * hidden.devicePixelRatio(), 140 * hidden.devicePixelRatio()), 100) < 5);
        // ...without moving the text.
        QCOMPARE(bar->height(), 88.0);

        // A mouse movement brings them back, and so does a key.
        QTest::mouseMove(app.window, QPoint(300, 300));
        QTest::mouseMove(app.window, QPoint(340, 320));
        QTRY_COMPARE(title->opacity(), 1.0);
        QTRY_COMPARE_WITH_TIMEOUT(title->opacity(), 0.0, 5000);
        QTest::keyClick(app.window, Qt::Key_Shift);
        QTRY_COMPARE(title->opacity(), 1.0);

        // Paused, they stay.
        app.narrator.showForTests(Narrator::Paused, 0, 1);
        QTest::qWait(3000);
        QCOMPARE(title->opacity(), 1.0);

        // Z turns focus mode off and on again.
        QTest::keyClick(app.window, Qt::Key_Z);
        QVERIFY(!app.reader.focusMode());
        QTest::keyClick(app.window, Qt::Key_Z);
        QVERIFY(app.reader.focusMode());
        app.narrator.stop();
        app.reader.closeBook();
        QSettings().remove("reader");
    }

    void settingsPanelOffersTheFocusAids() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        // Tall enough for the whole panel to show at once.
        QVERIFY(app.show({1100, 1900}));
        QVERIFY(app.reader.addText("Settings", "Some text to read."));
        QTRY_VERIFY(app.reader.hasBook());
        QObject *panel = app.window->findChild<QObject *>("settingsPanel");
        QVERIFY(panel);
        QMetaObject::invokeMethod(panel, "open");
        QTRY_VERIFY(panel->property("opened").toBool());
        QTRY_COMPARE(app.items("focusModeSwitch").size(), 1);
        QQuickItem *toggle = app.items("focusModeSwitch").first();
        QVERIFY(!toggle->property("checked").toBool());
        app.reader.setFocusMode(true);
        QVERIFY(toggle->property("checked").toBool());

        // Every switch's label fits the panel.
        QQuickItem *drawer = toggle;
        while (drawer->parentItem() && drawer->parentItem() != app.window->contentItem())
            drawer = drawer->parentItem();
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
            if (item->isVisible() && QByteArray(item->metaObject()->className()).contains("Switch")) {
                const QRectF r = item->mapRectToScene(QRectF(0, 0, item->implicitWidth(), item->height()));
                const QRectF bounds = drawer->mapRectToScene(QRectF(0, 0, drawer->width(), drawer->height()));
                QVERIFY2(r.right() <= bounds.right() + 1,
                         qPrintable(item->property("text").toString() + " runs past the panel's edge"));
            }
            for (QQuickItem *child : item->childItems())
                walk(child);
        };
        walk(drawer);
        app.grab("settings-focus");
        app.reader.closeBook();
        QSettings().remove("reader");
    }

    void exportDialogOffersAudioFilesOnly() {
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        QObject *dialog = app.window->findChild<QObject *>("exportDialog");
        QVERIFY(dialog);
        QMetaObject::invokeMethod(dialog, "open");
        QTRY_VERIFY(dialog->property("opened").toBool());

        // Every choice on screen, in order: the two formats, then the files.
        QStringList choices;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
            if (item->isVisible() && QByteArray(item->metaObject()->className()).contains("RadioButton"))
                choices << item->property("text").toString();
            for (QQuickItem *child : item->childItems())
                walk(child);
        };
        walk(app.window->contentItem());
        QCOMPARE(choices, QStringList({"MP3, plays everywhere", "M4B audiobook (AAC)", "One file with chapter markers",
                                       "A folder with one file per chapter"}));
        app.grab("export-dialog");
    }

    void libraryAddsAWebPageFromItsAddress() {
        FakeHttpServer server([](const FakeHttpServer::Request &r) -> FakeHttpServer::Response {
            if (r.target == "/missing")
                return {404, {}, {}};
            return {200, Fixtures::articlePage(), {{"Content-Type", "text/html; charset=utf-8"}}};
        });
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        QObject *dialog = app.window->findChild<QObject *>("urlDialog");
        QVERIFY(dialog);

        // U opens it, from the keyboard alone.
        QTest::keyClick(app.window, Qt::Key_U);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QQuickItem *address = app.items("urlAddress").value(0);
        QVERIFY(address);
        QTRY_VERIFY(address->hasActiveFocus());

        // A failed fetch says why on screen and adds nothing.
        address->setProperty("text", server.url() + "/missing");
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(!app.items("urlError").isEmpty());
        QImage shot = app.grab("url-dialog-error");
        QVERIFY(redPixels(shot, app.area(shot, app.items("urlError").first())) > 50);
        QCOMPARE(app.library.count(), 0);

        // The article comes back to check, with its title ready to edit.
        address->setProperty("text", server.url() + "/story");
        QTest::keyClick(app.window, Qt::Key_Return);
        QQuickItem *title = nullptr;
        QTRY_VERIFY((title = app.items("urlTitle").value(0)) && title->hasActiveFocus());
        QCOMPARE(title->property("text").toString(), QString("Why Terns Fly So Far"));
        QQuickItem *text = app.items("urlText").value(0);
        QVERIFY(text->property("text").toString().startsWith("Every year the Arctic tern"));
        QVERIFY(app.items("urlError").isEmpty());
        shot = app.grab("url-dialog-review");
        // The text is drawn in its box, light on dark.
        const QRect box = app.area(shot, text).intersected(shot.rect());
        QVERIFY2(brightPixels(shot, box) > 500, qPrintable(QString::number(brightPixels(shot, box))));
        QCOMPARE(app.library.count(), 0);

        // Editing the title and pressing Enter adds it and opens it.
        title->setProperty("text", "Terns");
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(app.reader.hasBook());
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QCOMPARE(app.library.count(), 1);
        QCOMPARE(app.reader.title(), QString("Terns"));
        QCOMPARE(app.library.entry(app.reader.path()).source, server.url() + "/story");
        QCOMPARE(app.library.entry(app.reader.path()).author, QString("Ana Ribeiro"));
        QCOMPARE(server.requests.size(), 2);
        app.grab("url-dialog-added");
        QVERIFY(!app.webImport.hasArticle());

        // Back in the library, a copied link opens the dialog with it filled in.
        app.reader.closeBook();
        QTRY_VERIFY(!app.reader.hasBook());
        QGuiApplication::clipboard()->setText(server.url() + "/other");
        QTest::keyClick(app.window, Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(app.items("urlAddress").value(0)->property("text").toString(), server.url() + "/other");
        QCOMPARE(server.requests.size(), 2);
        app.grab("url-dialog-pasted");
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QCOMPARE(app.library.count(), 1);
    }

    void pronunciationIsFixedFromThePageAndListedInTheVoicePanel() {
        QSettings().remove("reader");
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        QVERIFY(app.reader.addText("Names", "Hermione waved at the crowd from the steps of the old library."));
        QTRY_COMPARE(app.items("bookParagraph").size(), 1);
        QQuickItem *paragraph = app.items("bookParagraph").first();
        auto at = [&](int position) {
            QVariant rect;
            QMetaObject::invokeMethod(paragraph, "rectFor", Q_RETURN_ARG(QVariant, rect), Q_ARG(QVariant, position));
            const QRectF r = paragraph->mapRectToScene(rect.toRectF());
            return QPoint(int(r.center().x()), int(r.center().y()));
        };
        QObject *editor = app.window->findChild<QObject *>("pronunciationEditor");
        QVERIFY(editor);

        // P on the word under the mouse opens it for that word, ready for
        // how it sounds.
        QTest::mouseMove(app.window, at(3));
        QTest::qWait(100);
        QTest::keyClick(app.window, Qt::Key_P);
        QTRY_VERIFY(editor->property("opened").toBool());
        QQuickItem *written = app.items("pronunciationWritten").value(0);
        QQuickItem *spoken = app.items("pronunciationSpoken").value(0);
        QVERIFY(written && spoken);
        QCOMPARE(written->property("text").toString(), QString("Hermione"));
        QTRY_VERIFY(spoken->hasActiveFocus());
        QVERIFY(app.items("removePronunciation").isEmpty());
        for (const char c : QByteArray("Her-my-oh-nee"))
            QTest::keyClick(app.window, c);
        QImage shot = app.grab("pronunciation-editor");
        // Both fields are drawn with their text, light on dark.
        QVERIFY2(brightPixels(shot, app.area(shot, written)) > 80, "the written form isn't drawn");
        QVERIFY2(brightPixels(shot, app.area(shot, spoken)) > 80, "the respelling isn't drawn");
        // Typing in it doesn't reach the reader's keys: S didn't stop, A didn't open the panel.
        QObject *settings = app.window->findChild<QObject *>("settingsPanel");
        QVERIFY(!settings->property("opened").toBool());
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(!editor->property("opened").toBool());
        QCOMPARE(app.pronunciations.spokenFor("hermione"), QString("Her-my-oh-nee"));
        QTRY_VERIFY(!editor->property("visible").toBool());

        // The voice panel lists it, the respelling in the accent.
        QTest::keyClick(app.window, Qt::Key_A);
        QTRY_VERIFY(settings->property("opened").toBool());
        QTRY_COMPARE(app.items("pronunciationRow").size(), 1);
        QQuickItem *row = app.items("pronunciationRow").first();
        QMetaObject::invokeMethod(settings, "forceActiveFocus");
        // Brought into view, as a reader scrolls to it.
        QQuickItem *flick = row->parentItem() ? row->parentItem()->parentItem() : nullptr;
        if (flick && flick->property("contentY").isValid())
            flick->setProperty("contentY", qMax(0.0, row->y() - 100));
        shot = app.grab("pronunciation-list");
        QQuickItem *saidLabel = app.items("pronunciationSpokenLabel").first();
        QQuickItem *writtenLabel = app.items("pronunciationWrittenLabel").first();
        const QRect said = app.area(shot, saidLabel).intersected(shot.rect());
        int accent = 0;
        for (int y = said.top(); y <= said.bottom(); ++y)
            for (int x = said.left(); x <= said.right(); ++x) {
                const QColor c = shot.pixelColor(x, y);
                accent += c.red() > 200 && c.green() > 170 && c.blue() < 80;
            }
        QVERIFY2(accent > 30, qPrintable(QString("%1 accent pixels in the respelling").arg(accent)));
        QVERIFY2(brightPixels(shot, app.area(shot, writtenLabel).intersected(shot.rect())) > 30,
                 "the written form isn't drawn in the list");

        // Clicking it opens it again, now with a way to remove it.
        const QPointF centre = row->mapToScene(QPointF(row->width() / 2, row->height() / 2));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centre.toPoint());
        QTRY_VERIFY(editor->property("opened").toBool());
        QCOMPARE(app.items("pronunciationSpoken").first()->property("text").toString(), QString("Her-my-oh-nee"));
        QTRY_COMPARE(app.items("removePronunciation").size(), 1);
        app.grab("pronunciation-editor-existing");
        QQuickItem *remove = app.items("removePronunciation").first();
        QTest::mouseClick(app.window, Qt::LeftButton, {},
                          remove->mapToScene(QPointF(remove->width() / 2, remove->height() / 2)).toPoint());
        QTRY_VERIFY(!editor->property("opened").toBool());
        QCOMPARE(app.pronunciations.count(), 0);
        QTRY_COMPARE(app.items("pronunciationRow").size(), 0);
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!settings->property("visible").toBool());

        // The word card offers it too, for the word looked up.
        QTest::mouseClick(app.window, Qt::RightButton, {}, at(3));
        QObject *card = app.window->findChild<QObject *>("wordCard");
        QTRY_VERIFY(card->property("opened").toBool());
        QTRY_COMPARE(app.items("wordCardPronounce").size(), 1);
        QQuickItem *fix = app.items("wordCardPronounce").first();
        shot = app.grab("word-card-pronounce");
        QVERIFY2(brightPixels(shot, app.area(shot, fix).intersected(shot.rect()), 120) > 30, "the button isn't drawn");
        QTest::mouseClick(app.window, Qt::LeftButton, {},
                          fix->mapToScene(QPointF(fix->width() / 2, fix->height() / 2)).toPoint());
        QTRY_VERIFY(editor->property("opened").toBool());
        QTRY_VERIFY(!card->property("opened").toBool());
        QCOMPARE(app.items("pronunciationWritten").first()->property("text").toString().toLower(), QString("hermione"));
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!editor->property("opened").toBool());
        QCOMPARE(app.pronunciations.count(), 0);
    }

    void wordCardShowsWhatTheDictionariesSay() {
        QTemporaryDir dir;
        AppWindow app(dir.path());
        QVERIFY(app.show());
        QVERIFY(app.reader.addText("Hill", "The houses stood on the hill above the river, and the road ran past them."));
        QTRY_COMPARE(app.items("bookParagraph").size(), 1);
        QQuickItem *paragraph = app.items("bookParagraph").first();
        QObject *card = app.window->findChild<QObject *>("wordCard");
        QVERIFY(card);
        // Where a character of the paragraph is on screen.
        auto at = [&](int position) {
            QVariant rect;
            QMetaObject::invokeMethod(paragraph, "rectFor", Q_RETURN_ARG(QVariant, rect), Q_ARG(QVariant, position));
            return paragraph->mapRectToScene(rect.toRectF());
        };
        const QRectF houses = at(4).united(at(10));
        const QPoint onHouses = QPoint(int(at(6).center().x()), int(at(6).center().y()));

        // Without a dictionary, the card says how to install one.
        QTest::mouseClick(app.window, Qt::RightButton, {}, onHouses);
        QTRY_VERIFY(card->property("opened").toBool());
        QTRY_VERIFY(!app.lookup.busy());
        QCOMPARE(app.lookup.word(), QString("houses"));
        QTRY_VERIFY(!app.items("wordCardCommand").isEmpty());
        QCOMPARE(app.items("wordCardCommand").first()->property("text").toString(),
                 QString("omareader dictionaries --available"));
        app.grab("word-card-no-dictionary");
        // A right-click doesn't start the voice.
        QCOMPARE(app.narrator.state(), Narrator::Stopped);
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!card->property("opened").toBool());

        writeStarDict(dir.filePath("dictionaries/eng-por"), "English-Portuguese FreeDict Dictionary",
                      {{"house", "<div>/<font color=\"gray\">haus</font>/<br><ol><li><div>casa</div></li>"
                                 "<li><div>habitação</div></li><li><div>vivenda</div></li></ol></div>"},
                       {"run", "<div>/rʌn/<br><ol><li>correr</li><li>corrida</li></ol></div>"}},
                      "sametypesequence=h\n", true, {{"ran", 1}});
        QTest::mouseClick(app.window, Qt::RightButton, {}, onHouses);
        QTRY_VERIFY(card->property("opened").toBool());
        QTRY_COMPARE(app.items("wordCardEntry").size(), 1);
        QQuickItem *text = app.items("wordCardText").first();
        QCOMPARE(text->property("text").toString(), QString("/haus/\n1. casa\n2. habitação\n3. vivenda"));
        QVERIFY(app.items("wordCardCommand").isEmpty());
        const QImage shot = app.grab("word-card");

        // The card sits just below the word, and its text is drawn, light on dark.
        QQuickItem *cardItem = app.items("wordCardWord").first()->parentItem()->parentItem();
        const QRectF cardArea = cardItem->mapRectToScene(QRectF(0, 0, cardItem->width(), cardItem->height()));
        QVERIFY2(cardArea.top() > houses.bottom() && cardArea.top() < houses.bottom() + 24,
                 qPrintable(QString("card at %1, word ends at %2").arg(cardArea.top()).arg(houses.bottom())));
        QVERIFY(cardArea.left() <= houses.left() && cardArea.right() >= houses.right());
        const QRect drawn = app.area(shot, text).intersected(shot.rect());
        QVERIFY2(brightPixels(shot, drawn) > 150, qPrintable(QString::number(brightPixels(shot, drawn))));
        // The word itself is outlined in the accent while its card is open.
        QQuickItem *mark = app.items("lookedUpWord").value(0);
        QVERIFY(mark);
        const QRectF marked = mark->mapRectToScene(QRectF(0, 0, mark->width(), mark->height()));
        QVERIFY2(marked.contains(houses.center()) && marked.width() < houses.width() + 12,
                 qPrintable(QString("mark %1 wide at %2, word %3 wide at %4").arg(marked.width()).arg(marked.left())
                                .arg(houses.width()).arg(houses.left())));
        int accent = 0;
        const QRect markArea = app.area(shot, mark);
        for (int y = markArea.top(); y <= markArea.bottom(); ++y)
            for (int x = markArea.left(); x <= markArea.right(); ++x) {
                const QColor c = shot.pixelColor(x, y);
                accent += c.red() > 200 && c.green() > 170 && c.blue() < 80;
            }
        QVERIFY2(accent > 20, qPrintable(QString::number(accent)));
        if (!qEnvironmentVariableIsEmpty("OMAREADER_SCREENSHOTS"))
            shot.copy(app.area(shot, cardItem).united(markArea).adjusted(-8, -8, 8, 8))
                .save(qEnvironmentVariable("OMAREADER_SCREENSHOTS") + "/word-card-crop.png");

        // D closes it again.
        QTest::keyClick(app.window, Qt::Key_D);
        QTRY_VERIFY(!card->property("opened").toBool());
        QTRY_VERIFY(!mark->isVisible());

        // D looks up the word being read, pausing the voice, and closing the
        // card lets it go on. "ran" is the twelfth word.
        app.narrator.showForTests(Narrator::Playing, 0, 12);
        QTest::keyClick(app.window, Qt::Key_D);
        QTRY_VERIFY(card->property("opened").toBool());
        QCOMPARE(app.lookup.word(), QString("ran"));
        QCOMPARE(app.narrator.state(), Narrator::Paused);
        QTRY_COMPARE(app.items("wordCardEntry").size(), 1);
        QCOMPARE(app.items("wordCardText").first()->property("text").toString(), QString("/rʌn/\n1. correr\n2. corrida"));
        app.grab("word-card-spoken");
        QTest::keyClick(app.window, Qt::Key_D);
        QTRY_VERIFY(!card->property("opened").toBool());
        QTRY_VERIFY(app.narrator.state() != Narrator::Paused);
        app.narrator.stop();
        app.reader.closeBook();
    }

private:
    QTemporaryDir m_settings;
    QTemporaryDir m_home;

    static void write(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }

    // gzip of data, as gzip(1) writes it.
    static QByteArray gzipped(const QByteArray &data) {
        z_stream stream{};
        deflateInit2(&stream, 9, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        QByteArray out(qsizetype(deflateBound(&stream, uLong(data.size()))) + 64, '\0');
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
        stream.avail_in = uInt(data.size());
        stream.next_out = reinterpret_cast<Bytef *>(out.data());
        stream.avail_out = uInt(out.size());
        deflate(&stream, Z_FINISH);
        out.resize(out.size() - stream.avail_out);
        deflateEnd(&stream);
        return out;
    }

    // data as dictzip writes it: gzip with a table of chunks in its extra
    // field, each compressed to inflate on its own.
    static QByteArray dictzipped(const QByteArray &data, int chunkLength) {
        z_stream stream{};
        deflateInit2(&stream, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        QByteArray body;
        QList<quint16> sizes;
        for (qsizetype at = 0; at < data.size(); at += chunkLength) {
            const QByteArray chunk = data.mid(at, chunkLength);
            QByteArray buffer(chunkLength * 2 + 64, '\0');
            stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(chunk.constData()));
            stream.avail_in = uInt(chunk.size());
            stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
            stream.avail_out = uInt(buffer.size());
            deflate(&stream, at + chunkLength >= data.size() ? Z_FINISH : Z_FULL_FLUSH);
            const qsizetype size = buffer.size() - stream.avail_out;
            body += buffer.left(size);
            sizes.append(quint16(size));
        }
        deflateEnd(&stream);
        auto le16 = [](QByteArray &to, quint16 v) {
            to.append(char(v & 0xff));
            to.append(char(v >> 8));
        };
        QByteArray extra("RA");
        le16(extra, quint16(6 + 2 * sizes.size()));
        le16(extra, 1);
        le16(extra, quint16(chunkLength));
        le16(extra, quint16(sizes.size()));
        for (const quint16 size : std::as_const(sizes))
            le16(extra, size);
        QByteArray out = QByteArray::fromHex("1f8b0804000000000203");
        le16(out, quint16(extra.size()));
        const quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef *>(data.constData()), uInt(data.size())));
        const quint32 length = quint32(data.size());
        out += extra + body;
        out.append(reinterpret_cast<const char *>(&crc), 4);
        out.append(reinterpret_cast<const char *>(&length), 4);
        return out;
    }

    // A StarDict dictionary in folder, named after it: the articles in the
    // order given, synonyms pointing at them by number, and the .idx and
    // .dict compressed (gzip and dictzip) when asked.
    static void writeStarDict(const QString &folder, const QString &name, const QList<QPair<QByteArray, QByteArray>> &articles,
                              const QByteArray &ifoLines, bool compressed,
                              const QList<QPair<QByteArray, quint32>> &synonyms = {}) {
        QDir().mkpath(folder);
        const QString base = folder + "/" + QFileInfo(folder).fileName();
        QByteArray dict, idx, syn;
        auto be32 = [](QByteArray &to, quint32 v) {
            v = qToBigEndian(v);
            to.append(reinterpret_cast<const char *>(&v), 4);
        };
        for (const auto &[word, article] : articles) {
            idx += word + '\0';
            be32(idx, quint32(dict.size()));
            be32(idx, quint32(article.size()));
            dict += article;
        }
        for (const auto &[word, entry] : synonyms) {
            syn += word + '\0';
            be32(syn, entry);
        }
        write(base + ".ifo", "StarDict's dict ifo file\nversion=2.4.2\nbookname=" + name.toUtf8() + "\nwordcount="
                                 + QByteArray::number(articles.size()) + "\nidxfilesize=" + QByteArray::number(idx.size())
                                 + "\n" + ifoLines);
        write(base + (compressed ? ".idx.gz" : ".idx"), compressed ? gzipped(idx) : idx);
        write(base + (compressed ? ".dict.dz" : ".dict"), compressed ? dictzipped(dict, 16) : dict);
        if (!synonyms.isEmpty())
            write(base + ".syn", syn);
    }

    // A stand-in agent CLI: a shell script with the given body.
    static void fakeAgent(QTemporaryDir &dir, const QString &name, const QByteArray &body) {
        const QString path = dir.filePath(name);
        write(path, "#!/bin/sh\n" + body);
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    static QByteArray wavHeader(quint16 format, quint16 channels, quint32 rate, quint16 bits, quint32 dataSize) {
        QByteArray h;
        auto u32 = [&](quint32 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 4); };
        auto u16 = [&](quint16 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 2); };
        h.append("RIFF");
        u32(36 + dataSize);
        h.append("WAVEfmt ");
        u32(16);
        u16(format);
        u16(channels);
        u32(rate);
        u32(rate * channels * bits / 8);
        u16(channels * bits / 8);
        u16(bits);
        h.append("data");
        u32(dataSize);
        return h;
    }

    static QByteArray wav(quint32 rate, quint16 channels, const QList<float> &samples) {
        QByteArray data(reinterpret_cast<const char *>(samples.constData()), samples.size() * 4);
        return wavHeader(3, channels, rate, 32, quint32(data.size())) + data;
    }

    // A stand-in for koko that writes a tiny WAV and TSV per line and
    // reports them the way Kokoros does; a line reading FAIL errors.
    KokoroSetup fakeKoko(QTemporaryDir &dir) {
        const QString sample = dir.filePath("sample.wav");
        write(sample, wav(24000, 1, {0.1f, 0.2f, 0.3f}));
        const QString koko = dir.filePath("koko");
        write(koko, QByteArray("#!/bin/sh\n"
                               "[ -n \"${FAKE_KOKO_LOG:-}\" ] && echo \"$*\" >> \"$FAKE_KOKO_LOG\"\n"
                               "while [ $# -gt 0 ]; do case \"$1\" in file) input=\"$2\"; shift;; --output) pattern=\"$2\"; shift;; esac; shift; done\n"
                               "[ -n \"${FAKE_KOKO_TEXT:-}\" ] && cat \"$input\" >> \"$FAKE_KOKO_TEXT\"\n"
                               "i=0\n"
                               "while IFS= read -r line; do\n"
                               "  out=$(printf '%s' \"$pattern\" | sed \"s/{line}/$i/\")\n"
                               "  tsv=\"${out%.wav}.tsv\"\n"
                               "  if [ \"$line\" = FAIL ]; then echo \"Error processing line $((i+1)): boom\" >&2\n"
                               "  else cp '") + sample.toUtf8() + QByteArray("' \"$out\"; printf 'word\\tstart_sec\\tend_sec\\nHello\\t0.000\\t0.100\\n' > \"$tsv\"\n"
                               "    echo \"Audio saved to $out\" >&2; echo \"Timestamps saved to $tsv\" >&2; fi\n"
                               "  i=$((i+1))\n"
                               "done < \"$input\"\n"));
        QFile::setPermissions(koko, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        write(dir.filePath("kokoro-v1.0.onnx"), "x");
        write(dir.filePath("voices-v1.0.bin"), "x");
        return KokoroSetup::find(koko, dir.path());
    }

    // The fake worker, run by a stand-in "python" that ignores the real
    // worker script it is handed, with verified folders for the models.
    QwenSetup fakeQwen(QTemporaryDir &dir, const QString &modelName = "Qwen3-TTS-12Hz-0.6B-CustomVoice") {
        const QString script = dir.filePath("fake-worker.py");
        write(script, Fixtures::fakeQwenWorker());
        const QString python = dir.filePath("python");
        write(python, "#!/bin/sh\nshift 2\nexec python3 -I '" + script.toUtf8() + "' \"$@\"\n");
        QFile::setPermissions(python, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QwenSetup setup;
        setup.python = python;
        setup.model = dir.filePath(modelName);
        setup.aligner = dir.filePath("Qwen3-ForcedAligner-0.6B");
        QDir().mkpath(setup.model);
        QDir().mkpath(setup.aligner);
        return setup;
    }

    // A book of whole sentences to highlight and bookmark, written to path
    // so it has a file to be known by.
    static Book notedBook(const QString &path) {
        write(path, "noted");
        Book book;
        book.path = path;
        book.title = "Noted";
        book.format = "txt";
        book.chapters = {{"One",
                          {{"It was a bright cold day in April. The clocks were striking thirteen.", 0, {}},
                           {"Another paragraph, with words that don't end soon.", 0, {}}}},
                         {"Two", {{"Far away in another chapter. The end.", 0, {}}}}};
        book.segment();
        return book;
    }

    static Book sampleBook(const QString &path) {
        write(path, "sample");
        Book book;
        book.path = path;
        book.title = "Sample";
        book.format = "txt";
        book.chapters = {{"One", {{"First. Second.", 0}, {"Third.", 0}}}, {"Two", {{"Fourth.", 0}}}};
        book.segment();
        return book;
    }
};

QTEST_MAIN(OmareaderTests)
#include "omareader_tests.moc"
