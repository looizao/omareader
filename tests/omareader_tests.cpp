#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtEndian>
#include <QtTest>

#include "audioplayer.h"
#include "book.h"
#include "bookloader.h"
#include "drive.h"
#include "dropbox.h"
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
#include "sync.h"
#include "syncengine.h"
#include "syncstate.h"
#include "synthesizer.h"
#include "theme.h"
#include "titleagent.h"
#include "tokenstore.h"
#include "translator.h"
#include "voicesettings.h"
#include "zipwriter.h"

// The real window (qrc:/Main.qml) with everything main() gives it, drawn
// offscreen, so tests can check what is actually on screen. Set
// OMAREADER_SCREENSHOTS to a folder to keep each shot for a look.
class AppWindow {
public:
    AppWindow(const QString &dir)
        : library(dir + "/library.json", dir + "/covers"), reader(&library, &narrator, nullptr),
          sync(&library, &tokens, dir + "/sync.json") {
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
        context->setContextProperty("sync", &sync);
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
    MemoryTokenStore tokens;
    SyncService sync;
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

    // The sync screens and messages are new; check each of their strings so
    // none ships in English to a Portuguese reader.
    void everySyncStringHasAPortugueseTranslation() {
        QFile file(QString(OMAREADER_SOURCE) + "/src/i18n/pt-BR.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject catalog = QJsonDocument::fromJson(file.readAll()).object();
        static const QRegularExpression call("\\b(?:qsTr|tr)\\(\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
        int found = 0;
        for (const QString name :
             {"src/SettingsPanel.qml", "src/ExportDialog.qml", "src/sync.cpp", "src/oauth.cpp", "src/drive.cpp"}) {
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

    void zipWriterWritesStoredEntries() {
        QCOMPARE(ZipWriter::crc32("123456789"), 0xCBF43926u);
        QCOMPARE(ZipWriter::crc32("6789", ZipWriter::crc32("12345")), 0xCBF43926u);
        QTemporaryDir dir;
        QByteArray big(3 * 1024 * 1024 + 17, '\0');
        for (int i = 0; i < big.size(); ++i)
            big[i] = char((i * 31) ^ (i >> 7));
        write(dir.filePath("big.bin"), big);
        const QString path = dir.filePath("out.zip");
        ZipWriter zip(path);
        QVERIFY(zip.open());
        QVERIFY(zip.addData("manifest.json", "{\"a\":1}"));
        QVERIFY(zip.addFile("audio/001.m4a", dir.filePath("big.bin")));
        QVERIFY(zip.addData("ã.txt", ""));
        QVERIFY(!zip.addFile("missing", dir.filePath("nope")));
        QVERIFY(!zip.error().isEmpty());

        ZipWriter good(path);
        QVERIFY(good.open());
        QVERIFY(good.addData("manifest.json", "{\"a\":1}"));
        QVERIFY(good.addFile("audio/001.m4a", dir.filePath("big.bin")));
        QVERIFY(good.addData("ã.txt", ""));
        QVERIFY(good.finish());
        QStringList names;
        const QHash<QString, QByteArray> entries = readZip(path, &names);
        QCOMPARE(names, (QStringList{"manifest.json", "audio/001.m4a", "ã.txt"}));
        QCOMPARE(entries.value("audio/001.m4a"), big);
        QCOMPARE(entries.value("manifest.json"), QByteArray("{\"a\":1}"));

        // An independent reader agrees, when one is installed.
        const QString python = QStandardPaths::findExecutable("python3");
        if (!python.isEmpty()) {
            QProcess check;
            check.start(python, {"-I", "-c",
                                 "import sys, zipfile\n"
                                 "z = zipfile.ZipFile(sys.argv[1])\n"
                                 "assert z.testzip() is None\n"
                                 "print('|'.join(z.namelist()))",
                                 path});
            QVERIFY(check.waitForFinished());
            QCOMPARE(check.exitCode(), 0);
            QCOMPARE(QString::fromUtf8(check.readAllStandardOutput()).trimmed(),
                     QString("manifest.json|audio/001.m4a|ã.txt"));
        }
    }

    void phoneBundleDescribesTheBookAndItsWords() {
        QTemporaryDir dir;
        Book book = sampleBook(dir.filePath("a.txt"));
        book.author = "Ann";
        QCOMPARE(Exporter::extension(Exporter::Phone, Exporter::ChapterFiles), QString("omabook"));
        QCOMPARE(Exporter::phoneEntryName("audio", 3, "m4a"), QString("audio/003.m4a"));
        QCOMPARE(Exporter::phoneBookId(book).size(), 16);
        Book other = book;
        other.author = "Bob";
        QVERIFY(Exporter::phoneBookId(other) != Exporter::phoneBookId(book));

        VoiceSettings voice = VoiceSettings::recommended(Engine::Kokoro);
        const QJsonObject manifest =
            QJsonDocument::fromJson(
                Exporter::phoneManifest(book, voice, {"One", "Two"}, {36000, 12000}, 24000, true, {1, 3}))
                .object();
        QCOMPARE(manifest.value("format").toString(), QString("omabook"));
        QCOMPARE(manifest.value("version").toInt(), 1);
        QCOMPARE(manifest.value("id").toString(), Exporter::phoneBookId(book));
        QCOMPARE(manifest.value("author").toString(), QString("Ann"));
        QCOMPARE(manifest.value("cover").toString(), QString("cover.jpg"));
        QCOMPARE(manifest.value("voice").toObject().value("engine").toString(), QString("kokoro"));
        const QJsonArray chapters = manifest.value("chapters").toArray();
        QCOMPARE(chapters.size(), 2);
        QCOMPARE(chapters.at(0).toObject().value("duration").toInteger(), 1500);
        QCOMPARE(chapters.at(1).toObject().value("audio").toString(), QString("audio/002.m4a"));
        QCOMPARE(chapters.at(1).toObject().value("text").toString(), QString("text/002.json"));
        // Each bundle chapter knows its chapter in the book, skipping the
        // ones with nothing to read, so sync can carry a position across.
        QCOMPARE(chapters.at(0).toObject().value("chapter").toInt(-1), 1);
        QCOMPARE(chapters.at(1).toObject().value("chapter").toInt(-1), 3);
        QVERIFY(!QJsonDocument::fromJson(Exporter::phoneManifest(book, voice, {"One"}, {1}, 24000, false))
                     .object()
                     .contains("cover"));

        const QJsonObject chapter =
            QJsonDocument::fromJson(Exporter::phoneChapter(book, 0, "One", {1, 0, 5, 10, 400}, {0})).object();
        QCOMPARE(chapter.value("title").toString(), QString("One"));
        QCOMPARE(chapter.value("paragraphs").toArray().size(), 2);
        QCOMPARE(chapter.value("paragraphs").toArray().at(1).toObject().value("text").toString(), QString("Third."));
        QCOMPARE(chapter.value("words").toArray().size(), 5);
        QCOMPARE(chapter.value("words").toArray().at(4).toInt(), 400);
        QCOMPARE(chapter.value("sentences").toArray(), QJsonArray{0});
    }

    void exportWritesAPhoneBundle() {
        if (Exporter::findFfmpeg().isEmpty())
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        Exporter exporter;
        QSignalSpy finished(&exporter, &Exporter::finished);
        Exporter::Options options;
        options.synthesis.kokoro = fakeKoko(dir);
        options.folder = dir.filePath("out");
        options.format = Exporter::Phone;
        options.layout = Exporter::ChapterFiles; // a bundle is one file whatever the layout
        QVERIFY(options.synthesis.voice.set("paragraphPause", 0.5));
        QVERIFY(exporter.start(std::make_shared<Book>(sampleBook(dir.filePath("a.txt"))), options));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QCOMPARE(exporter.result(), dir.filePath("out/Sample.omabook"));
        QCOMPARE(QDir(options.folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
                 QStringList{"Sample.omabook"});

        QStringList names;
        const QHash<QString, QByteArray> entries = readZip(exporter.result(), &names);
        QCOMPARE(names, (QStringList{"manifest.json", "audio/001.m4a", "text/001.json", "audio/002.m4a",
                                     "text/002.json"}));
        const QJsonObject manifest = QJsonDocument::fromJson(entries.value("manifest.json")).object();
        QCOMPARE(manifest.value("title").toString(), QString("Sample"));
        const QJsonArray chapters = manifest.value("chapters").toArray();
        QCOMPARE(chapters.size(), 2);
        QCOMPARE(chapters.at(1).toObject().value("title").toString(), QString("Two"));
        QVERIFY(chapters.at(0).toObject().value("duration").toInteger() > 0);
        // An MP4 file starts with its ftyp box.
        QCOMPARE(entries.value("audio/001.m4a").mid(4, 4), QByteArray("ftyp"));

        // "First. Second." and "Third.": three words, in reading order, each
        // pointing at its paragraph and its characters there.
        const QJsonObject one = QJsonDocument::fromJson(entries.value("text/001.json")).object();
        const QJsonArray words = one.value("words").toArray();
        QCOMPARE(words.size(), 3 * 5);
        QCOMPARE(words.at(0).toInt(), 0);
        QCOMPARE(words.at(1).toInt(), 0);
        QCOMPARE(words.at(2).toInt(), 5);
        QCOMPARE(words.at(5).toInt(), 0);
        QCOMPARE(words.at(6).toInt(), 7);
        QCOMPARE(words.at(10).toInt(), 1);
        QCOMPARE(one.value("sentences").toArray(), (QJsonArray{0, 1, 2}));
        int previous = -1;
        for (int i = 0; i < words.size(); i += 5) {
            QVERIFY(words.at(i + 3).toInt() >= previous);
            QVERIFY(words.at(i + 4).toInt() >= words.at(i + 3).toInt());
            previous = words.at(i + 3).toInt();
        }
        // The words sit on the chapter's timeline: "Third." comes after the
        // paragraph pause, not at the start of its own sentence's audio.
        QVERIFY(words.at(13).toInt() >= 500);
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
        QCOMPARE(exporter.format(), int(Exporter::Phone));
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

    // Sync through Dropbox (docs/sync.md).

    void syncMergesAsThePhoneDoes() {
        // The same cases the Android tests check, so both apps agree.
        const QJsonObject vectors = readJson(syncFixture("sync/merge.json"));
        const QJsonArray cases = vectors.value("cases").toArray();
        QVERIFY(!cases.isEmpty());
        for (const QJsonValue &value : cases) {
            const QJsonObject c = value.toObject();
            BookState local, remote, merged;
            QVERIFY(BookState::fromJson(QJsonDocument(c.value("local").toObject()).toJson(), &local));
            QVERIFY(BookState::fromJson(QJsonDocument(c.value("remote").toObject()).toJson(), &remote));
            QVERIFY(BookState::fromJson(QJsonDocument(c.value("merged").toObject()).toJson(), &merged));
            QVERIFY2(BookState::merge(local, remote) == merged, qPrintable(c.value("name").toString()));
            QVERIFY2(BookState::merge(remote, local) == merged, qPrintable(c.value("name").toString()));
            BookState again;
            QVERIFY(BookState::fromJson(merged.toJson(), &again));
            QVERIFY(again == merged);
        }
        const QJsonObject settings = vectors.value("settings").toObject();
        SyncSettings local, remote, merged;
        QVERIFY(SyncSettings::fromJson(QJsonDocument(settings.value("local").toObject()).toJson(), &local));
        QVERIFY(SyncSettings::fromJson(QJsonDocument(settings.value("remote").toObject()).toJson(), &remote));
        QVERIFY(SyncSettings::fromJson(QJsonDocument(settings.value("merged").toObject()).toJson(), &merged));
        QVERIFY(SyncSettings::merge(local, remote) == merged);
        QVERIFY(SyncSettings::merge(remote, local) == merged);
        QCOMPARE(merged.value("future.setting").value.toString(), QString("kept"));

        const QJsonObject hashes = vectors.value("contentHash").toObject();
        QFile bundle(syncFixture(hashes.value("file").toString()));
        QVERIFY(bundle.open(QIODevice::ReadOnly));
        QCOMPARE(SyncFormat::contentHash(&bundle), hashes.value("hash").toString());
        const QJsonObject pattern = hashes.value("pattern").toObject();
        QByteArray data(pattern.value("size").toInt(), Qt::Uninitialized);
        for (qsizetype i = 0; i < data.size(); ++i)
            data[i] = char(i % 251);
        QCOMPARE(SyncFormat::contentHash(data), pattern.value("hash").toString());
    }

    void syncStateRefusesWhatItCantRead() {
        BookState state;
        QVERIFY(!BookState::fromJson("{\"format\":\"omareader-state\",\"version\":2,\"id\":\"abc\"}", &state));
        QVERIFY(!BookState::fromJson("{\"format\":\"omareader-state\",\"version\":1,\"id\":\"../x\"}", &state));
        QVERIFY(!BookState::fromJson("not json", &state));
        QVERIFY(BookState::fromJson("{\"format\":\"omareader-state\",\"version\":1,\"id\":\"abc\"}", &state));
        QVERIFY(!state.present.isSet() && !state.hasPosition());
        // A newer change beats an older one; on a tie the larger device id wins.
        const Register older{true, 1, "b"}, newer{false, 2, "a"}, tie{false, 2, "c"};
        QVERIFY(newer.beats(older) && !older.beats(newer));
        QVERIFY(tie.beats(newer));
        QVERIFY(older.beats(Register{}));
    }

    void syncHeaderStaysAscii() {
        const QByteArray json = SyncFormat::headerJson(
            QJsonObject{{"path", QString::fromUtf8("/state/café") + QChar(0x7F) + QString::fromUtf8("\U0001F600")}});
        QCOMPARE(json, QByteArray("{\"path\":\"/state/caf\\u00e9\\u007f\\ud83d\\ude00\"}"));
        QCOMPARE(SyncFormat::idFromPath("/state/7218324797ea94e3.json", "state", ".json"), QString("7218324797ea94e3"));
        QCOMPARE(SyncFormat::idFromPath("/books/7218324797ea94e3.omabook", "books", ".omabook"),
                 QString("7218324797ea94e3"));
        QVERIFY(SyncFormat::idFromPath("/state/../x.json", "state", ".json").isEmpty());
        QVERIFY(SyncFormat::idFromPath("/other/a.json", "state", ".json").isEmpty());
        QCOMPARE(SyncFormat::newDeviceId().size(), 16);
    }

    void syncReadsABundlesManifest() {
        const QJsonObject manifest = SyncFormat::bundleManifest(syncFixture("fixture.omabook"));
        QCOMPARE(manifest.value("id").toString(), QString("7218324797ea94e3"));
        QCOMPARE(SyncFormat::bookId(manifest.value("title").toString(), manifest.value("author").toString()),
                 QString("7218324797ea94e3"));
        QTemporaryDir dir;
        write(dir.filePath("not.omabook"), "PK nothing");
        QVERIFY(SyncFormat::bundleManifest(dir.filePath("not.omabook")).isEmpty());
    }

    void libraryStampsOnlyRealMoves() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.json"), dir.filePath("covers"));
        Book book = sampleBook(dir.filePath("a.txt"));
        library.opened(book);
        QVERIFY(library.entry(book.path).addedAt > 0);
        library.savePosition(book.path, 0, 1, 2, book.sentenceCount(), 0);
        const qint64 moved = library.entry(book.path).positionAt;
        QVERIFY(moved > 0);
        // The same place saved again, with the sentence count filled in, is
        // not a move.
        QTest::qWait(5);
        library.savePosition(book.path, 0, 1, 2, book.sentenceCount() + 1, 0);
        QCOMPARE(library.entry(book.path).positionAt, moved);

        QSignalSpy synced(&library, &Library::positionSynced);
        library.applySyncedPosition(book.path, 1, 0, 0, 1.0, 1234);
        QCOMPARE(synced.count(), 1);
        QCOMPARE(library.entry(book.path).positionAt, qint64(1234));
        QCOMPARE(library.entry(book.path).chapter, 1);

        QSignalSpy removed(&library, &Library::removed);
        library.forget(book.path);
        QCOMPARE(removed.count(), 0); // a removal from elsewhere isn't sent back
        library.opened(book);
        library.remove(book.path);
        QCOMPARE(removed.count(), 1);
    }

    void readerFollowsASyncedPlace() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.json"), dir.filePath("covers"));
        Book book = sampleBook(dir.filePath("a.txt"));
        library.opened(book);
        Reader reader(&library, nullptr, nullptr);
        reader.openBook(std::make_shared<Book>(book));
        QCOMPARE(reader.sentence(), 0);
        QSignalSpy moved(&reader, &Reader::movedBySync);
        // "Second." starts at offset 7 of the first paragraph; a phone
        // stopping on its word lands in that sentence.
        library.applySyncedPosition(book.path, 0, 0, 8, 0.3, 1000);
        QCOMPARE(moved.count(), 1);
        QCOMPARE(reader.sentence(), 1);
        // Saving keeps the phone's finer place, so it doesn't sync back as new.
        reader.saveNow();
        QCOMPARE(library.entry(book.path).offset, 8);
        QCOMPARE(library.entry(book.path).positionAt, qint64(1000));
        reader.closeBook();

        // Reopened, it resumes from the offset.
        Reader again(&library, nullptr, nullptr);
        again.openBook(std::make_shared<Book>(book));
        QCOMPARE(again.sentence(), 1);
    }

    void syncEngineSendsAndSettles() {
        MemoryRemote remote;
        SyncInput in = syncInput("aaaaaaaaaaaaaaaa");
        in.local.insert("7218324797ea94e3", bookState("7218324797ea94e3", true, 100, "aaaaaaaaaaaaaaaa"));
        in.settings.values.insert("speed", Register{1.25, 50, "aaaaaaaaaaaaaaaa"});
        SyncOutput out = SyncEngine::run(in, remote);
        QVERIFY2(out.ok, qPrintable(out.error));
        QCOMPARE(out.writtenFiles, 2);
        QVERIFY(remote.files.contains("/state/7218324797ea94e3.json"));
        QVERIFY(remote.files.contains("/settings.json"));

        // Nothing changed: nothing is read again or written.
        in.cache = out.cache;
        in.settingsRev = out.settingsRev;
        in.settingsCache = out.settingsCache;
        remote.downloads = 0;
        out = SyncEngine::run(in, remote);
        QVERIFY(out.ok);
        QCOMPARE(out.writtenFiles, 0);
        QCOMPARE(remote.downloads, 0);
    }

    void syncEngineTakesNewerChangesAndRemovesBundles() {
        MemoryRemote remote;
        const QString id = "7218324797ea94e3";
        BookState phone = bookState(id, false, 300, "bbbbbbbbbbbbbbbb");
        phone.position = Register{SyncPosition{2, 1, 4, 0.9}.toJson(), 250, "bbbbbbbbbbbbbbbb"};
        remote.put("/state/" + id + ".json", phone.toJson());
        remote.put("/books/" + id + ".omabook", "bundle");
        SyncInput in = syncInput("aaaaaaaaaaaaaaaa");
        BookState mine = bookState(id, true, 100, "aaaaaaaaaaaaaaaa");
        mine.position = Register{SyncPosition{1, 0, 0, 0.5}.toJson(), 400, "aaaaaaaaaaaaaaaa"};
        in.local.insert(id, mine);
        const SyncOutput out = SyncEngine::run(in, remote);
        QVERIFY(out.ok);
        const BookState merged = out.merged.value(id);
        QVERIFY(merged.isRemoved());                       // the phone's removal is newer
        QCOMPARE(merged.syncPosition().chapter, 1);         // this device's place is newer
        QCOMPARE(out.removedBundles, 1);
        QVERIFY(!remote.files.contains("/books/" + id + ".omabook"));
        BookState written;
        QVERIFY(BookState::fromJson(remote.files.value("/state/" + id + ".json").data, &written));
        QVERIFY(written == merged);
    }

    void syncEngineRetriesWhenAnotherDeviceWrites() {
        MemoryRemote remote;
        const QString id = "7218324797ea94e3";
        remote.put("/state/" + id + ".json", bookState(id, true, 100, "bbbbbbbbbbbbbbbb").toJson());
        // Just before this device writes, the phone moves on.
        BookState phone = bookState(id, true, 100, "bbbbbbbbbbbbbbbb");
        phone.position = Register{SyncPosition{2, 0, 0, 0.8}.toJson(), 900, "bbbbbbbbbbbbbbbb"};
        remote.beforeUpload = [&](const QString &path) {
            if (path == "/state/" + id + ".json") {
                remote.put(path, phone.toJson());
                remote.beforeUpload = nullptr;
            }
        };
        SyncInput in = syncInput("aaaaaaaaaaaaaaaa");
        BookState mine;
        mine.id = id;
        mine.position = Register{SyncPosition{1, 0, 0, 0.5}.toJson(), 500, "aaaaaaaaaaaaaaaa"};
        in.local.insert(id, mine);
        const SyncOutput out = SyncEngine::run(in, remote);
        QVERIFY2(out.ok, qPrintable(out.error));
        QCOMPARE(remote.conflicts, 1);
        QCOMPARE(out.merged.value(id).syncPosition().chapter, 2); // the phone's later move
    }

    void syncEngineLeavesNewerFilesAlone() {
        MemoryRemote remote;
        const QString id = "7218324797ea94e3";
        const QByteArray future = "{\"format\":\"omareader-state\",\"version\":9,\"id\":\"7218324797ea94e3\"}";
        remote.put("/state/" + id + ".json", future);
        SyncInput in = syncInput("aaaaaaaaaaaaaaaa");
        in.local.insert(id, bookState(id, true, 100, "aaaaaaaaaaaaaaaa"));
        SyncEngine::run(in, remote);
        QCOMPARE(remote.files.value("/state/" + id + ".json").data, future);
    }

    void syncEngineUploadsExports() {
        QTemporaryDir dir;
        MemoryRemote remote;
        const QString id = "7218324797ea94e3";
        remote.put("/state/" + id + ".json", bookState(id, false, 100, "bbbbbbbbbbbbbbbb").toJson());
        write(dir.filePath("book.omabook"), "bundle bytes");
        SyncInput in = syncInput("aaaaaaaaaaaaaaaa");
        in.now = 200;
        in.uploads = {{id, "fixture", "", dir.filePath("book.omabook")}, {"gone", "x", "", dir.filePath("gone")}};
        const SyncOutput out = SyncEngine::run(in, remote);
        QVERIFY(out.ok);
        QCOMPARE(out.uploadsDone, (QStringList{id, "gone"}));
        QCOMPARE(remote.files.value("/books/" + id + ".omabook").data, QByteArray("bundle bytes"));
        // Exporting again brings a removed book back.
        QVERIFY(out.merged.value(id).isPresent());
    }

    void syncServiceAppliesWhatOtherDevicesDid() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.json"), dir.filePath("covers"));
        Book book = sampleBook(dir.filePath("a.txt"));
        library.opened(book);
        library.savePosition(book.path, 0, 0, 1, book.sentenceCount(), 7);
        QVERIFY(library.entry(book.path).positionAt > 0);
        const QString id = SyncFormat::bookId(book.title, book.author);

        auto remote = std::make_shared<MemoryRemote>();
        MemoryTokenStore tokens;
        SyncService sync(&library, &tokens, dir.filePath("sync.json"));
        sync.setRemoteFactory([remote](const QString &) { return std::make_unique<SharedRemote>(remote); });
        sync.setSynchronous(true);
        Reader reader(&library, nullptr, nullptr);
        sync.setReader(&reader);
        QVERIFY(sync.connectWith(SyncService::Dropbox, "refresh"));
        QVERIFY2(sync.error().isEmpty(), qPrintable(sync.error()));
        QVERIFY(remote->files.contains("/state/" + id + ".json"));
        QVERIFY(remote->files.contains("/settings.json"));

        // The phone reached chapter two later on, and asked for bigger text
        // on desktops.
        BookState phone;
        QVERIFY(BookState::fromJson(remote->files.value("/state/" + id + ".json").data, &phone));
        const qint64 later = library.entry(book.path).positionAt + 1;
        phone.position = Register{SyncPosition{1, 0, 0, 1.0}.toJson(), later, "ffffffffffffffff"};
        remote->put("/state/" + id + ".json", phone.toJson());
        SyncSettings settings;
        QVERIFY(SyncSettings::fromJson(remote->files.value("/settings.json").data, &settings));
        settings.values.insert("textSize.desktop", Register{30, later, "ffffffffffffffff"});
        remote->put("/settings.json", settings.toJson());
        sync.syncNow();
        QVERIFY2(sync.error().isEmpty(), qPrintable(sync.error()));
        QCOMPARE(library.entry(book.path).chapter, 1);
        QCOMPARE(library.entry(book.path).positionAt, later);
        QCOMPARE(reader.fontSize(), 30);
        // Taking those changes wrote nothing back.
        const int writes = remote->writes;
        sync.syncNow();
        QCOMPARE(remote->writes, writes);

        // Removed on the phone: forgotten here, file untouched.
        phone.position = Register{SyncPosition{1, 0, 0, 1.0}.toJson(), later, "ffffffffffffffff"};
        phone.present = Register{false, later + 1, "ffffffffffffffff"};
        remote->put("/state/" + id + ".json", phone.toJson());
        sync.syncNow();
        QVERIFY(!library.contains(book.path));
        QVERIFY(QFile::exists(book.path));

        // Opened again here: back in the library, and Dropbox hears of it.
        QTest::qWait(5);
        library.opened(book);
        sync.syncNow();
        QVERIFY(stateOf(remote->files.value("/state/" + id + ".json").data).isPresent());

        // Removed here: the removal goes to Dropbox.
        library.remove(book.path);
        sync.syncNow();
        QVERIFY(stateOf(remote->files.value("/state/" + id + ".json").data).isRemoved());

        sync.disconnectSync();
        QVERIFY(!sync.connected());
        QVERIFY(tokens.read(SyncService::Dropbox).isEmpty());
    }

    void dropboxClientSpeaksDropbox() {
        int tokenCalls = 0;
        QByteArray stateFile = "{\"format\":\"omareader-state\"}";
        FakeHttpServer server([&](const FakeHttpServer::Request &r) -> FakeHttpServer::Response {
            if (r.target == "/oauth2/token") {
                ++tokenCalls;
                return {200, "{\"access_token\":\"fresh\",\"expires_in\":14400}", {}};
            }
            if (r.headers.value("authorization") != "Bearer fresh")
                return {401, "{\"error_summary\":\"expired_access_token/\"}", {}};
            if (r.target == "/2/files/list_folder")
                return {200, "{\"entries\":[{\".tag\":\"file\",\"path_lower\":\"/state/a.json\",\"rev\":\"1\","
                             "\"content_hash\":\"h\",\"size\":3},{\".tag\":\"folder\",\"path_lower\":\"/state\"}],"
                             "\"cursor\":\"c1\",\"has_more\":true}",
                        {}};
            if (r.target == "/2/files/list_folder/continue")
                return {200, "{\"entries\":[{\".tag\":\"file\",\"path_lower\":\"/settings.json\",\"rev\":\"2\"}],"
                             "\"cursor\":\"c2\",\"has_more\":false}",
                        {}};
            if (r.target == "/2/files/download")
                return {200, stateFile, {{"Dropbox-API-Result", "{\"rev\":\"7\"}"}}};
            if (r.target == "/2/files/upload") {
                const QJsonObject arg = QJsonDocument::fromJson(r.headers.value("dropbox-api-arg")).object();
                if (arg.value("mode").toObject().value("update").toString() == "stale")
                    return {409, "{\"error_summary\":\"path/conflict/file/..\"}", {}};
                return {200, "{\"rev\":\"8\"}", {}};
            }
            if (r.target == "/2/files/upload_session/start")
                return {200, "{\"session_id\":\"s\"}", {}};
            if (r.target == "/2/files/upload_session/append_v2")
                return {200, "null", {}};
            if (r.target == "/2/files/upload_session/finish")
                return {200, "{\"rev\":\"9\"}", {}};
            if (r.target == "/2/files/delete_v2")
                return {409, "{\"error_summary\":\"path_lookup/not_found/..\"}", {}};
            return {404, "", {}};
        });
        DropboxHosts hosts;
        hosts.api = hosts.content = server.url();
        DropboxRemote remote("key", "refresh", hosts);
        remote.setAccessToken("stale", QDateTime::currentMSecsSinceEpoch() + 100000);

        QList<RemoteFile> files;
        QCOMPARE(remote.list(&files), SyncRemote::Ok);
        QCOMPARE(tokenCalls, 1); // the 401 refreshed the token once
        QCOMPARE(files.size(), 2);
        QCOMPARE(files.at(0).path, QString("/state/a.json"));
        QCOMPARE(files.at(1).rev, QString("2"));
        QByteArray data;
        QString rev;
        QCOMPARE(remote.download("/state/a.json", &data, &rev), SyncRemote::Ok);
        QCOMPARE(data, stateFile);
        QCOMPARE(rev, QString("7"));
        QCOMPARE(remote.upload("/state/a.json", "{}", "stale", &rev), SyncRemote::Conflict);
        QCOMPARE(remote.upload("/state/b.json", "{}", {}, &rev), SyncRemote::Ok);
        QCOMPARE(rev, QString("8"));
        const auto &add = server.requests.last();
        QCOMPARE(QJsonDocument::fromJson(add.headers.value("dropbox-api-arg")).object().value("mode").toString(),
                 QString("add"));
        QCOMPARE(remote.remove("/books/x.omabook"), SyncRemote::NotFound);

        // A bundle larger than one piece goes through an upload session.
        QTemporaryDir dir;
        write(dir.filePath("big.omabook"), QByteArray(25, 'x'));
        remote.setChunkSize(10);
        QCOMPARE(remote.uploadFile("/books/x.omabook", dir.filePath("big.omabook"), &rev), SyncRemote::Ok);
        QCOMPARE(rev, QString("9"));
        QStringList calls;
        qsizetype bytes = 0;
        for (const auto &r : server.requests)
            if (r.target.startsWith("/2/files/upload_session/")) {
                calls.append(QString::fromLatin1(r.target.mid(24)));
                bytes += r.body.size();
            }
        QCOMPARE(calls, (QStringList{"start", "append_v2", "finish"}));
        QCOMPARE(bytes, 25);
        const QJsonObject finish = QJsonDocument::fromJson(server.requests.last().headers.value("dropbox-api-arg")).object();
        QCOMPARE(finish.value("cursor").toObject().value("offset").toInt(), 20);
        QCOMPARE(finish.value("commit").toObject().value("mode").toString(), QString("overwrite"));
    }

    void dropboxSignInUsesPkceOnLocalhost() {
        QByteArray tokenRequest;
        FakeHttpServer server([&](const FakeHttpServer::Request &r) -> FakeHttpServer::Response {
            tokenRequest = r.body;
            return {200, "{\"refresh_token\":\"r\",\"access_token\":\"a\",\"expires_in\":100}", {}};
        });
        DropboxHosts hosts;
        hosts.api = server.url();
        DropboxAuth auth("key", nullptr, hosts);
        const QUrl url(auth.start());
        if (url.isEmpty())
            QSKIP("Ports 53682 to 53684 are all in use here.");
        const QUrlQuery query(url);
        QCOMPARE(query.queryItemValue("code_challenge_method"), QString("S256"));
        QCOMPARE(query.queryItemValue("token_access_type"), QString("offline"));
        QCOMPARE(query.queryItemValue("redirect_uri", QUrl::FullyDecoded),
                 QString("http://localhost:%1/dropbox").arg(auth.port()));
        QCOMPARE(DropboxAuth::challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
                 QString("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));

        QSignalSpy connected(&auth, &DropboxAuth::connected);
        // Someone else's request is turned away.
        QCOMPARE(httpGet(auth.port(), "/dropbox?code=evil&state=wrong").left(12), QByteArray("HTTP/1.1 404"));
        QCOMPARE(httpGet(auth.port(), "/dropbox?code=good&state=" + QUrl::toPercentEncoding(auth.state())).left(12),
                 QByteArray("HTTP/1.1 200"));
        QTRY_COMPARE(connected.count(), 1);
        QCOMPARE(connected.first().value(0).toString(), QString("r"));
        const QUrlQuery sent(QString::fromUtf8(tokenRequest));
        QCOMPARE(sent.queryItemValue("code"), QString("good"));
        QCOMPARE(sent.queryItemValue("client_id"), QString("key"));
        QCOMPARE(sent.queryItemValue("code_verifier").size(), 64);
        QVERIFY(!auth.running()); // it stopped listening
    }

    void driveClientKeepsToOmareadersFolder() {
        FakeDrive drive;
        FakeHttpServer server([&](const FakeHttpServer::Request &r) { return drive.handle(r); });
        drive.base = server.url();
        DriveHosts hosts;
        hosts.api = hosts.oauth = server.url();
        DriveRemote remote("id", "secret", "refresh", hosts);
        QList<int> sleeps;
        remote.setSleep([&](int ms) { sleeps.append(ms); });
        remote.setAccessToken("stale", QDateTime::currentMSecsSinceEpoch() + 100000);

        // A stale token is renewed once, and a busy Drive is waited out.
        drive.busy = 1;
        QList<RemoteFile> files;
        QCOMPARE(remote.list(&files), SyncRemote::Ok);
        QCOMPARE(drive.tokenRequests, 1);
        QVERIFY(QUrlQuery(QString::fromUtf8(drive.lastTokenRequest)).queryItemValue("client_secret") == "secret");
        QCOMPARE(sleeps.size(), 1);
        QVERIFY(sleeps.first() >= 1000 && sleeps.first() < 2000);
        QVERIFY(files.isEmpty());

        // The first write makes the folder, and the file carries its path.
        QString rev;
        QCOMPARE(remote.upload("/state/a.json", "one", {}, &rev), SyncRemote::Ok);
        QCOMPARE(drive.withPath("/state/a.json").size(), 1);
        const FakeDrive::File *made = drive.withPath("/state/a.json").first();
        const FakeDrive::File *folder = drive.file(made->parent);
        QVERIFY(folder);
        QCOMPARE(folder->name, QString("omareader"));
        QCOMPARE(folder->mimeType, QString(DriveRemote::FolderMime));
        QCOMPARE(made->name, QString("a.json"));
        QCOMPARE(rev, QString("rev1"));
        // The fake's files move as it grows; keep the id, not the pointer.
        const QString folderId = folder->id;

        // Adding what exists, or updating from a revision that moved on, is a
        // conflict, as with Dropbox.
        QCOMPARE(remote.upload("/state/a.json", "again", {}, nullptr), SyncRemote::Conflict);
        QString newer;
        QCOMPARE(remote.upload("/state/a.json", "two", rev, &newer), SyncRemote::Ok);
        QCOMPARE(newer, QString("rev2"));
        QCOMPARE(remote.upload("/state/a.json", "three", rev, nullptr), SyncRemote::Conflict);
        drive.withPath("/state/a.json").first()->revision = 5; // another device wrote
        QCOMPARE(remote.upload("/state/a.json", "three", newer, nullptr), SyncRemote::Conflict);
        QByteArray data;
        QCOMPARE(remote.download("/state/a.json", &data, &rev), SyncRemote::Ok);
        QCOMPARE(data, QByteArray("two"));
        QCOMPARE(rev, QString("rev5"));

        // Two copies of one path: records keep the first made, bundles the
        // last written, and the rest go to the trash.
        drive.add("/state/dup.json", "first", folderId);
        drive.add("/state/dup.json", "second", folderId);
        drive.add("/books/b.omabook", "old", folderId);
        drive.add("/books/b.omabook", "new", folderId);
        drive.add("/settings.json", "{}", folderId);
        QCOMPARE(remote.list(&files), SyncRemote::Ok); // over three pages
        QCOMPARE(files.size(), 4);
        QHash<QString, RemoteFile> byPath;
        for (const RemoteFile &f : std::as_const(files))
            byPath.insert(f.path, f);
        QCOMPARE(QSet<QString>(byPath.keyBegin(), byPath.keyEnd()),
                 (QSet<QString>{"/state/a.json", "/state/dup.json", "/books/b.omabook", "/settings.json"}));
        QCOMPARE(remote.download("/state/dup.json", &data, nullptr), SyncRemote::Ok);
        QCOMPARE(data, QByteArray("first"));
        QCOMPARE(remote.download("/books/b.omabook", &data, nullptr), SyncRemote::Ok);
        QCOMPARE(data, QByteArray("new"));
        QCOMPARE(byPath.value("/books/b.omabook").contentHash,
                 QString::fromLatin1(QCryptographicHash::hash("new", QCryptographicHash::Sha256).toHex()));
        QVERIFY(drive.withPath("/state/dup.json").at(1)->trashed);
        QVERIFY(drive.withPath("/books/b.omabook").at(0)->trashed);

        // A second folder made later by another device: everyone keeps
        // writing to the first.
        DriveRemote other("id", "secret", "refresh", hosts);
        other.setSleep([](int) {});
        drive.files.prepend(FakeDrive::File{"z-late", "omareader", DriveRemote::FolderMime, {},
                                            {{"omareader", "folder"}}, {}, "2027-01-01T00:00:00.000Z",
                                            "2027-01-01T00:00:00.000Z", 1, false});
        QCOMPARE(other.upload("/state/c.json", "c", {}, nullptr), SyncRemote::Ok);
        QCOMPARE(drive.withPath("/state/c.json").first()->parent, folderId);

        // Deleting moves to the trash, and a second time there is nothing.
        QCOMPARE(remote.remove("/state/a.json"), SyncRemote::Ok);
        QVERIFY(drive.withPath("/state/a.json").first()->trashed);
        QCOMPARE(remote.remove("/state/a.json"), SyncRemote::NotFound);
        QCOMPARE(remote.upload("/state/a.json", "back", "rev5", nullptr), SyncRemote::Conflict);

        QString name, email;
        QVERIFY(remote.account(&name, &email));
        QCOMPARE(email, QString("ana@example.com"));
        QVERIFY(remote.revoke());
    }

    void driveUploadsBundlesInPieces() {
        FakeDrive drive;
        FakeHttpServer server([&](const FakeHttpServer::Request &r) { return drive.handle(r); });
        drive.base = server.url();
        DriveHosts hosts;
        hosts.api = hosts.oauth = server.url();
        DriveRemote remote("id", "secret", "refresh", hosts);
        remote.setSleep([](int) {});
        const QString bundle = syncFixture("fixture.omabook");
        QFile file(bundle);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        const qint64 piece = 4096;
        QVERIFY(bytes.size() > 3 * piece);
        remote.setChunkSize(piece);

        // A piece lost on the way is sent again from where Drive says it
        // stopped.
        drive.losePiece = true;
        QString rev;
        QCOMPARE(remote.uploadFile("/books/7218324797ea94e3.omabook", bundle, &rev), SyncRemote::Ok);
        QCOMPARE(drive.withPath("/books/7218324797ea94e3.omabook").size(), 1);
        FakeDrive::File *up = drive.withPath("/books/7218324797ea94e3.omabook").first();
        QCOMPARE(up->data, bytes);
        QCOMPARE(up->name, SyncFormat::bundleManifest(bundle).value("title").toString() + ".omabook");
        QVERIFY(!drive.losePiece);
        QCOMPARE(rev, QString("rev1"));

        // Exported again: the same file gets new content, even when the
        // upload session has expired on the way.
        drive.expireSession = true;
        QCOMPARE(remote.uploadFile("/books/7218324797ea94e3.omabook", bundle, &rev), SyncRemote::Ok);
        QCOMPARE(drive.withPath("/books/7218324797ea94e3.omabook").size(), 1);
        QCOMPARE(rev, QString("rev2"));
        QCOMPARE(drive.withPath("/books/7218324797ea94e3.omabook").first()->data, bytes);
    }

    void driveSyncsLikeDropbox() {
        // The engine runs over Drive as it does over Dropbox: one device's
        // place reaches another.
        FakeDrive drive;
        FakeHttpServer server([&](const FakeHttpServer::Request &r) { return drive.handle(r); });
        drive.base = server.url();
        DriveHosts hosts;
        hosts.api = hosts.oauth = server.url();
        auto device = [&] {
            auto remote = std::make_unique<DriveRemote>("id", "secret", "refresh", hosts);
            remote->setSleep([](int) {});
            return remote;
        };
        SyncInput desk = syncInput("aaaaaaaaaaaaaaaa");
        BookState book = bookState("abc", true, 10, "aaaaaaaaaaaaaaaa");
        book.position = Register{SyncPosition{2, 3, 0, 0.5}.toJson(), 20, "aaaaaaaaaaaaaaaa"};
        desk.local.insert("abc", book);
        SyncOutput out = SyncEngine::run(desk, *device());
        QVERIFY2(out.ok, qPrintable(out.error));

        SyncInput phone = syncInput("bbbbbbbbbbbbbbbb");
        out = SyncEngine::run(phone, *device());
        QVERIFY2(out.ok, qPrintable(out.error));
        QCOMPARE(out.merged.value("abc").syncPosition().chapter, 2);

        // The phone moves on; the desk hears of it.
        BookState later = out.merged.value("abc");
        later.position = Register{SyncPosition{4, 0, 0, 0.8}.toJson(), 30, "bbbbbbbbbbbbbbbb"};
        phone.local.insert("abc", later);
        phone.cache = out.cache;
        out = SyncEngine::run(phone, *device());
        QVERIFY2(out.ok, qPrintable(out.error));
        out = SyncEngine::run(desk, *device());
        QCOMPARE(out.merged.value("abc").syncPosition().chapter, 4);
        QCOMPARE(drive.withPath("/state/abc.json").size(), 1);
    }

    void driveSignInUsesGooglesLoopback() {
        FakeDrive drive;
        FakeHttpServer server([&](const FakeHttpServer::Request &r) { return drive.handle(r); });
        DriveHosts hosts;
        hosts.oauth = server.url();
        DriveAuth auth("client", "secret", nullptr, hosts);
        const QUrl url(auth.start());
        QVERIFY(!url.isEmpty());
        const QUrlQuery query(url);
        QCOMPARE(query.queryItemValue("redirect_uri", QUrl::FullyDecoded),
                 QString("http://127.0.0.1:%1/drive").arg(auth.port()));
        QCOMPARE(query.queryItemValue("scope", QUrl::FullyDecoded),
                 QString("https://www.googleapis.com/auth/drive.file"));
        QCOMPARE(query.queryItemValue("access_type"), QString("offline"));
        QCOMPARE(query.queryItemValue("code_challenge_method"), QString("S256"));

        QSignalSpy connected(&auth, &LoopbackAuth::connected);
        QCOMPARE(httpGet(auth.port(), "/favicon.ico").left(12), QByteArray("HTTP/1.1 404"));
        QVERIFY(auth.running()); // still waiting for the answer
        QCOMPARE(httpGet(auth.port(), "/drive?code=c&state=" + QUrl::toPercentEncoding(auth.state())).left(12),
                 QByteArray("HTTP/1.1 200"));
        QTRY_COMPARE(drive.tokenRequests, 1);
        const QUrlQuery sent(QString::fromUtf8(drive.lastTokenRequest));
        QCOMPARE(sent.queryItemValue("client_secret"), QString("secret"));
        QCOMPARE(sent.queryItemValue("code_verifier").size(), 64);
        QCOMPARE(sent.queryItemValue("redirect_uri", QUrl::FullyDecoded),
                 QString("http://127.0.0.1:%1/drive").arg(auth.port()));
        // The fake answers without a refresh token, which isn't a connection.
        QSignalSpy failed(&auth, &LoopbackAuth::failed);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(connected.count(), 0);
    }

    void syncServiceSwitchesProviders() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.json"), dir.filePath("covers"));
        auto remote = std::make_shared<MemoryRemote>();
        MemoryTokenStore tokens;
        {
            SyncService sync(&library, &tokens, dir.filePath("sync.json"));
            sync.setRemoteFactory([remote](const QString &) { return std::make_unique<SharedRemote>(remote); });
            sync.setSynchronous(true);
            QVERIFY(sync.connectWith(SyncService::Dropbox, "box"));
            QCOMPARE(sync.provider(), QString("dropbox"));
            // Connecting Drive ends the Dropbox connection and its sign-in.
            QVERIFY(sync.connectWith(SyncService::Drive, "drive"));
            QCOMPARE(sync.provider(), QString("drive"));
            QCOMPARE(sync.providerName(), QString("Google Drive"));
            QVERIFY(tokens.read(SyncService::Dropbox).isEmpty());
            QCOMPARE(tokens.read(SyncService::Drive), QString("drive"));
        }
        // The choice is remembered.
        SyncService again(&library, &tokens, dir.filePath("sync.json"));
        QVERIFY(again.connected());
        QCOMPARE(again.provider(), QString("drive"));

        // A sync.json from before Drive, connected to Dropbox, stays so.
        QJsonObject old = readJson(dir.filePath("sync.json"));
        old.remove("provider");
        old.insert("connected", true);
        write(dir.filePath("old.json"), QJsonDocument(old).toJson());
        SyncService before(&library, &tokens, dir.filePath("old.json"));
        QCOMPARE(before.provider(), QString("dropbox"));
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

private:
    QTemporaryDir m_settings;
    QTemporaryDir m_home;

    static QString syncFixture(const QString &name) {
        return QStringLiteral(OMAREADER_SOURCE "/android/app/src/test/resources/") + name;
    }

    static QJsonObject readJson(const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
    }

    static SyncInput syncInput(const QString &device) {
        SyncInput in;
        in.device = device;
        in.now = 1000;
        return in;
    }

    static BookState bookState(const QString &id, bool present, qint64 at, const QString &by) {
        BookState state;
        state.id = id;
        state.title = "fixture";
        state.present = Register{present, at, by};
        return state;
    }

    static BookState stateOf(const QByteArray &json) {
        BookState state;
        BookState::fromJson(json, &state);
        return state;
    }

    // An app folder in memory, counting what the engine does with it.
    struct MemoryRemote : SyncRemote {
        struct File {
            QByteArray data;
            QString rev;
        };
        QHash<QString, File> files;
        int revs = 0, downloads = 0, writes = 0, conflicts = 0;
        std::function<void(const QString &)> beforeUpload;

        void put(const QString &path, const QByteArray &data) { files.insert(path, {data, QString::number(++revs)}); }
        Status list(QList<RemoteFile> *out) override {
            out->clear();
            for (auto it = files.cbegin(); it != files.cend(); ++it)
                out->append({it.key(), it.value().rev, SyncFormat::contentHash(it.value().data), it.value().data.size()});
            return Ok;
        }
        Status download(const QString &path, QByteArray *data, QString *rev) override {
            ++downloads;
            if (!files.contains(path))
                return NotFound;
            *data = files.value(path).data;
            *rev = files.value(path).rev;
            return Ok;
        }
        Status upload(const QString &path, const QByteArray &data, const QString &rev, QString *newRev) override {
            if (beforeUpload)
                beforeUpload(path);
            const bool exists = files.contains(path);
            if ((rev.isEmpty() && exists) || (!rev.isEmpty() && (!exists || files.value(path).rev != rev))) {
                ++conflicts;
                return Conflict;
            }
            put(path, data);
            ++writes;
            *newRev = files.value(path).rev;
            return Ok;
        }
        Status uploadFile(const QString &path, const QString &localFile, QString *newRev) override {
            QFile file(localFile);
            if (!file.open(QIODevice::ReadOnly))
                return Failed;
            put(path, file.readAll());
            ++writes;
            *newRev = files.value(path).rev;
            return Ok;
        }
        Status remove(const QString &path) override { return files.remove(path) ? Ok : NotFound; }
        QString error() const override { return {}; }
    };

    // The same folder for every sync a service runs.
    struct SharedRemote : SyncRemote {
        explicit SharedRemote(std::shared_ptr<MemoryRemote> remote) : m(std::move(remote)) {}
        std::shared_ptr<MemoryRemote> m;
        Status list(QList<RemoteFile> *files) override { return m->list(files); }
        Status download(const QString &p, QByteArray *d, QString *r) override { return m->download(p, d, r); }
        Status upload(const QString &p, const QByteArray &d, const QString &r, QString *n) override {
            return m->upload(p, d, r, n);
        }
        Status uploadFile(const QString &p, const QString &f, QString *n) override { return m->uploadFile(p, f, n); }
        Status remove(const QString &p) override { return m->remove(p); }
        QString error() const override { return {}; }
    };

    // A browser coming back from Dropbox, without blocking the event loop the
    // listener runs on.
    static QByteArray httpGet(quint16 port, const QByteArray &target) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        if (!QTest::qWaitFor([&] { return socket.state() == QAbstractSocket::ConnectedState; }, 5000))
            return {};
        socket.write("GET " + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
        QByteArray reply;
        QTest::qWaitFor([&] {
            reply += socket.readAll();
            return socket.state() == QAbstractSocket::UnconnectedState;
        }, 5000);
        return reply + socket.readAll();
    }

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

    // Reads the stored entries of an archive ZipWriter wrote, checking each
    // one's checksum; empty if anything is off.
    static QHash<QString, QByteArray> readZip(const QString &path, QStringList *names = nullptr) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        const QByteArray data = file.readAll();
        const auto u16 = [&](qsizetype at) { return qFromLittleEndian<quint16>(data.constData() + at); };
        const auto u32 = [&](qsizetype at) { return qFromLittleEndian<quint32>(data.constData() + at); };
        QHash<QString, QByteArray> entries;
        qsizetype pos = 0;
        while (pos + 30 <= data.size() && u32(pos) == 0x04034b50) {
            const quint32 crc = u32(pos + 14);
            const quint32 size = u32(pos + 18);
            const int nameLength = u16(pos + 26);
            const int extra = u16(pos + 28);
            if (u16(pos + 8) != 0 || u32(pos + 22) != size)
                return {};
            const QString name = QString::fromUtf8(data.mid(pos + 30, nameLength));
            const QByteArray content = data.mid(pos + 30 + nameLength + extra, size);
            if (ZipWriter::crc32(content) != crc)
                return {};
            entries.insert(name, content);
            if (names)
                names->append(name);
            pos += 30 + nameLength + extra + size;
        }
        return u32(pos) == 0x02014b50 ? entries : QHash<QString, QByteArray>{};
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
