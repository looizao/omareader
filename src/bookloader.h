#pragma once

#include "book.h"

#include <QCoreApplication>
#include <QDateTime>

struct PdfOutlineItem {
    int page = 0; // 1-based
    int level = 0;
    QString title;
};

// Turns a file on disk into a Book. Everything runs locally: EPUB, FB2, DOCX,
// ODT, HTML, Markdown and plain text are parsed in-process, PDF goes through
// poppler's pdftotext, and Kindle and other formats through Calibre's
// ebook-convert when it is installed. Safe to call from a worker thread.
class BookLoader {
    Q_DECLARE_TR_FUNCTIONS(BookLoader)

public:
    static std::shared_ptr<Book> load(const QString &path, QString *error);
    static QStringList nativeExtensions();
    static QStringList convertedExtensions();

    // Exposed for tests.
    static QList<Paragraph> htmlParagraphs(const QByteArray &html, QString *title = nullptr);
    static QList<Chapter> textChapters(const QString &text);
    // Top-level headings (# and ##) start chapters in a Markdown book; pasted
    // text passes false and stays one chapter, its headings kept as headings.
    static QList<Chapter> markdownChapters(const QString &text, bool headingChapters = true);
    // One paragraph's inline Markdown as the text it reads, with the
    // emphasis, code and strikethrough it marked kept as styles.
    static QString markdownInline(const QString &markdown, QList<TextStyle> *styles = nullptr);
    static QList<Chapter> pdfChapters(const QString &pdftotextOutput, const QList<PdfOutlineItem> &outline = {},
                                      int pagesPerChapter = 10);
    static QList<PdfOutlineItem> pdfOutline(const QString &pdftohtmlXml);
    static QString decodeText(const QByteArray &bytes);
    // Text pasted into the library, as the JSON file that keeps it.
    static QByteArray pastedText(const QString &title, const QString &text, const QDateTime &created);

private:
    static bool loadEpub(Book &book, QString *error);
    static bool loadFb2(Book &book, const QByteArray &data, QString *error);
    static bool loadDocx(Book &book, QString *error);
    static bool loadOdt(Book &book, QString *error);
    static bool loadPdf(Book &book, QString *error);
    static bool loadPastedText(Book &book, const QByteArray &data, QString *error);
    static bool loadConverted(Book &book, QString *error);
};
