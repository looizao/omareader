// Test code gains nothing from the optimiser, and this file is by far the
// slowest to compile, so it is built without it. The code under test in
// src/ keeps the flags it is built with.
#pragma GCC optimize("O0")

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

#include "article.h"
#include "audioplayer.h"
#include "book.h"
#include "bookloader.h"
#include "exporter.h"
#include "filepicker.h"
#include "fixtures.h"
#include "highlight.h"
#include "kokoro.h"
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
        : library(dir + "/library.json", dir + "/covers"), reader(&library, &narrator, nullptr) {
        reader.setExporter(&exporter);
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
        context->setContextProperty("titleAgent", &titleAgent);
        context->setContextProperty("webImport", &webImport);
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
    Exporter exporter;
    TitleAgent titleAgent;
    WebImport webImport;
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
        for (const QString name : {"src/SettingsPanel.qml", "src/ExportDialog.qml", "src/ReaderView.qml"}) {
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
                             "each one of them in turn, and a long word like incomprehensibilities stretches it, and "
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

        // Over an ordinary word, one pane centred on it, wide enough for it and its neighbours.
        speak(4);
        QCOMPARE(spoken(), QString("plain"));
        QList<QRectF> one = panes();
        QCOMPARE(one.size(), 1);
        const qreal width = one.first().width();
        const QRectF text4 = glass()->mapRectToScene(QRectF(0, 0, glass()->width(), glass()->height()));
        QVERIFY2(width > app.reader.fontSize() * 6 && width < app.reader.fontSize() * 12, qPrintable(QString::number(width)));

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

        // While the voice plays, it moves on at the pace of speech, from the
        // spoken word toward the next over the time between them, before the
        // next word has even started.
        speak(4);
        const qreal still = panes().first().center().x();
        app.narrator.showForTests(Narrator::Playing, 0, 4, 2.0);
        QTest::qWait(1000);
        const qreal halfway = panes().first().center().x();
        QVERIFY2(halfway > still + 10 && halfway < to - 5,
                 qPrintable(QString("from %1 toward %2, at %3 after half the word").arg(still).arg(to).arg(halfway)));
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(panes().first().center().x() - to) < 2, 3000);
        app.narrator.showForTests(Narrator::Paused, 0, 4);

        // Only a word too long for it stretches it.
        int word = 0;
        for (; word < 60; ++word) {
            app.narrator.showForTests(Narrator::Paused, 0, word);
            if (spoken() == "incomprehensibilities")
                break;
        }
        QCOMPARE(spoken(), QString("incomprehensibilities"));
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
            app.narrator.showForTests(Narrator::Paused, sentence, 2);
            QTRY_VERIFY(glass() && glass()->property("settled").toBool());
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
            const QList<QQuickItem *> panes = app.items("readingGlassPane");
            QCOMPARE(panes.size(), 1);
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

private:
    QTemporaryDir m_settings;
    QTemporaryDir m_home;

    static void write(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
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
