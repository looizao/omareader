#pragma once

#include "book.h"

#include <QCoreApplication>

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
    static QList<Chapter> markdownChapters(const QString &text);
    static QList<Chapter> pdfChapters(const QString &pdftotextOutput, int pagesPerChapter = 10);
    static QString decodeText(const QByteArray &bytes);

private:
    static bool loadEpub(Book &book, QString *error);
    static bool loadFb2(Book &book, const QByteArray &data, QString *error);
    static bool loadDocx(Book &book, QString *error);
    static bool loadOdt(Book &book, QString *error);
    static bool loadPdf(Book &book, QString *error);
    static bool loadConverted(Book &book, QString *error);
};
