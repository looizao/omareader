#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPainter>
#include <QPdfWriter>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include "book.h"
#include "bookloader.h"
#include "filepicker.h"
#include "fixtures.h"
#include "kokoro.h"
#include "library.h"
#include "narrator.h"
#include "reader.h"
#include "speech.h"
#include "theme.h"
#include "translator.h"

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

    void readerAppearanceIsBounded() {
        Reader reader(nullptr, nullptr, nullptr);
        reader.setFontSize(200);
        QCOMPARE(reader.fontSize(), 48);
        reader.setLineHeight(0.2);
        QCOMPARE(reader.lineHeight(), 1.0);
    }

private:
    QTemporaryDir m_settings;
    QTemporaryDir m_home;

    static void write(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
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
