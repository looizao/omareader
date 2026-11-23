#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

// Recognises the text of a scanned PDF, one with pictures of its pages and
// no text layer, with Tesseract. It all happens on this computer: poppler's
// pdftoppm draws each page and tesseract reads it, with the language data
// installed from pacman. Tesseract is optional; without it a scanned PDF
// says what to install.
//
// Recognition is slow (about a second a page on one core), so it runs
// several pages side by side, reports each page done, and can be stopped.
// The text is kept in app data under a hash of the PDF, so a book is
// recognised once wherever it is moved; pages done before a stop are kept
// too, and the next run picks up after them. The PDF itself is only read.
class PdfOcr {
    Q_DECLARE_TR_FUNCTIONS(PdfOcr)

public:
    struct Control {
        // Called from worker threads as pages finish, with how many of the
        // book's pages are recognised so far and how many there are.
        std::function<void(int done, int total)> progress;
        // Set to stop; recognise() then returns as soon as the pages being
        // read are abandoned, with cancelled set.
        const std::atomic<bool> *cancel = nullptr;
    };

    // The tesseract command on PATH, or empty when it isn't installed.
    static QString tesseract();
    // The languages whose data tesseract has, as it names them ("eng",
    // "por", "chi_sim"), leaving out osd and equ, which aren't languages.
    static QStringList installedLanguages(const QString &tesseract);
    // What to recognise with: the window's language and English, those of
    // them that are installed, or failing both the first one installed.
    // uiLanguage is a locale name such as "pt_BR". Empty when none is.
    static QString chooseLanguages(const QStringList &installed, const QString &uiLanguage);
    // Whether pdftotext's text, its pages split by form feeds, is too thin
    // to be the book: at most one page in ten with a line's worth of
    // letters. A scan with only its cover recognised counts.
    static bool looksScanned(const QString &pdftotextOutput);
    // Where the recognised text of the PDF with this fingerprint is kept.
    static QString cacheFile(const QString &fingerprint);
    // The cached text, if this PDF was recognised before.
    static bool cached(const QString &fingerprint, QString *text);

    // The text of every page, split by form feeds as pdftotext writes it,
    // so the same reflow applies. Taken from the cache when it is there;
    // otherwise recognised and cached. False with error set when it can't
    // be, or with *cancelled set when control.cancel stopped it.
    static bool recognise(const QString &pdf, const QString &fingerprint, int pages, const Control &control,
                          QString *text, QString *error, bool *cancelled = nullptr);
};
