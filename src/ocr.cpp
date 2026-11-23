#include "ocr.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QLockFile>
#include <QMutex>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent>

namespace {

enum class Run { Done, Failed, Stopped };

// Runs a command to the end, or until stop() says to give up, and keeps
// what it wrote. Tesseract would otherwise spread each page over every
// core, and here the pages are already shared out among them.
Run run(const QString &program, const QStringList &arguments, const std::function<bool()> &stop,
        QByteArray *output, QString *message) {
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("OMP_THREAD_LIMIT", "1");
    process.setProcessEnvironment(env);
    if (!output)
        process.setStandardOutputFile(QProcess::nullDevice());
    process.start(program, arguments);
    if (!process.waitForStarted()) {
        *message = process.errorString();
        return Run::Failed;
    }
    QElapsedTimer clock;
    clock.start();
    // A page that takes five minutes is not going to finish.
    while (!process.waitForFinished(100) && process.state() != QProcess::NotRunning) {
        if (stop() || clock.elapsed() > 5 * 60 * 1000) {
            process.kill();
            process.waitForFinished();
            if (clock.elapsed() > 5 * 60 * 1000)
                *message = QCoreApplication::translate("PdfOcr", "%1 took too long.").arg(QFileInfo(program).fileName());
            return stop() ? Run::Stopped : Run::Failed;
        }
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *message = QString::fromUtf8(process.readAllStandardError()).trimmed().section('\n', -1);
        if (message->isEmpty())
            *message = QCoreApplication::translate("PdfOcr", "%1 failed.").arg(QFileInfo(program).fileName());
        return Run::Failed;
    }
    if (output)
        *output = process.readAllStandardOutput();
    return Run::Done;
}

QString pageFile(const QString &folder, int page) {
    return QStringLiteral("%1/%2.txt").arg(folder).arg(page, 5, 10, QLatin1Char('0'));
}

} // namespace

QString PdfOcr::tesseract() {
    return QStandardPaths::findExecutable("tesseract");
}

QStringList PdfOcr::installedLanguages(const QString &tesseract) {
    if (tesseract.isEmpty())
        return {};
    QProcess process;
    process.start(tesseract, {"--list-langs"});
    if (!process.waitForFinished(15000)) {
        process.kill();
        return {};
    }
    // "List of available languages in "/usr/share/tessdata/" (2):" and then
    // one name a line. Older versions wrote the list to stderr.
    QStringList languages;
    const QString listing = QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
    static const QRegularExpression name(R"(^[A-Za-z][A-Za-z0-9_]*$)");
    for (const QString &line : listing.split('\n')) {
        const QString trimmed = line.trimmed();
        if (name.match(trimmed).hasMatch() && trimmed != "osd" && trimmed != "equ")
            languages.append(trimmed);
    }
    return languages;
}

QString PdfOcr::chooseLanguages(const QStringList &installed, const QString &uiLanguage) {
    const QLocale locale(uiLanguage);
    QString own = QLocale::languageToCode(locale.language(), QLocale::ISO639Part2T);
    // Tesseract names Chinese by its script.
    if (own == "zho")
        own = locale.script() == QLocale::TraditionalChineseScript
                      || locale.territory() == QLocale::Taiwan || locale.territory() == QLocale::HongKong
                  ? "chi_tra"
                  : "chi_sim";
    QStringList chosen;
    for (const QString &language : {own, QStringLiteral("eng")})
        if (!language.isEmpty() && installed.contains(language) && !chosen.contains(language))
            chosen.append(language);
    if (chosen.isEmpty() && !installed.isEmpty())
        chosen.append(installed.first());
    return chosen.join('+');
}

bool PdfOcr::looksScanned(const QString &output) {
    // pdftotext ends every page with a form feed, those without text too,
    // so what follows the last one is no page.
    QStringList pages = output.split('\f');
    if (pages.size() > 1 || pages.last().trimmed().isEmpty())
        pages.removeLast();
    if (pages.isEmpty())
        return false;
    // A page counts as text with a few words on it, so a short page of a
    // real text layer (a title page, a poem) is read as it is. A page number
    // or a roman numeral ("xviii") is not enough.
    int withText = 0;
    for (const QString &page : std::as_const(pages)) {
        int letters = 0;
        for (const QChar c : page)
            if (c.isLetter() && ++letters >= 10)
                break;
        if (letters >= 10)
            ++withText;
    }
    return withText * 10 <= pages.size();
}

QString PdfOcr::cacheFile(const QString &fingerprint) {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/ocr/" + fingerprint + ".txt";
}

bool PdfOcr::cached(const QString &fingerprint, QString *text) {
    if (fingerprint.isEmpty())
        return false;
    QFile file(cacheFile(fingerprint));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *text = QString::fromUtf8(file.readAll());
    return true;
}

bool PdfOcr::recognise(const QString &pdf, const QString &fingerprint, int pages, const Control &control,
                       QString *text, QString *error, bool *cancelled) {
    if (cancelled)
        *cancelled = false;
    if (cached(fingerprint, text))
        return true;

    const QString command = tesseract();
    if (command.isEmpty()) {
        *error = tr("This PDF is a scan: pictures of its pages, with no text in them. omareader can recognise the "
                    "text on this computer with Tesseract, which isn't installed. Install it with: "
                    "omarchy-pkg-add tesseract tesseract-data-eng");
        return false;
    }
    const QString pdftoppm = QStandardPaths::findExecutable("pdftoppm");
    if (pdftoppm.isEmpty()) {
        *error = tr("Opening PDF files needs pdftotext. Install it with: omarchy-pkg-add poppler");
        return false;
    }
    const QString languages = chooseLanguages(installedLanguages(command), QLocale().name());
    if (languages.isEmpty()) {
        *error = tr("Tesseract has no language data to recognise this scanned PDF with. Install English with: "
                    "omarchy-pkg-add tesseract-data-eng");
        return false;
    }
    if (fingerprint.isEmpty() || pages <= 0) {
        *error = tr("pdftotext could not read this PDF.");
        return false;
    }

    // Each page is kept as it is done, so a stop loses at most the pages
    // being read, and a book is put together once all are there.
    const QString target = cacheFile(fingerprint);
    const QString folder = QFileInfo(target).path() + "/" + fingerprint + ".pages";
    if (!QDir().mkpath(QFileInfo(target).path())) {
        *error = tr("The recognised text couldn't be saved in %1.").arg(QFileInfo(target).path());
        return false;
    }
    // The window and a command can open the same scan at once. One of them
    // recognises it while the other waits, then reads what it kept.
    QLockFile lock(target + ".lock");
    lock.setStaleLockTime(0);
    while (!lock.tryLock(200)) {
        if (lock.error() != QLockFile::LockFailedError) {
            *error = tr("The recognised text couldn't be saved in %1.").arg(QFileInfo(target).path());
            return false;
        }
        if (control.cancel && control.cancel->load()) {
            if (cancelled)
                *cancelled = true;
            return false;
        }
    }
    if (cached(fingerprint, text))
        return true;
    QTemporaryDir images;
    if (!QDir().mkpath(folder) || !images.isValid()) {
        *error = tr("The recognised text couldn't be saved in %1.").arg(QFileInfo(target).path());
        return false;
    }
    QList<int> todo;
    for (int page = 1; page <= pages; ++page)
        if (!QFileInfo::exists(pageFile(folder, page)))
            todo.append(page);
    std::atomic<int> done(pages - int(todo.size()));
    if (control.progress)
        control.progress(done, pages);

    std::atomic<bool> failed(false);
    QMutex mutex;
    QString failure;
    auto stop = [&] { return failed.load() || (control.cancel && control.cancel->load()); };
    auto fail = [&](const QString &message) {
        QMutexLocker lock(&mutex);
        if (!failed.exchange(true))
            failure = message;
    };

    QThreadPool pool;
    pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 2, 8));
    QtConcurrent::blockingMap(&pool, todo, [&](int page) {
        if (stop())
            return;
        const QString number = QString::number(page);
        const QString root = images.filePath("page-" + number);
        QString message;
        // 300 dpi in grey is what Tesseract reads best, and an uncompressed
        // grey image is the quickest to write and read back.
        Run result = run(pdftoppm, {"-r", "300", "-gray", "-singlefile", "-f", number, "-l", number, pdf, root}, stop,
                         nullptr, &message);
        QByteArray recognised;
        if (result == Run::Done)
            result = run(command, {root + ".pgm", "stdout", "-l", languages}, stop, &recognised, &message);
        QFile::remove(root + ".pgm");
        if (result == Run::Stopped)
            return;
        if (result == Run::Failed) {
            fail(tr("Tesseract could not read page %1 of this PDF: %2").arg(page).arg(message));
            return;
        }
        // Tesseract ends each page with a form feed of its own.
        recognised.replace('\f', '\n');
        QSaveFile file(pageFile(folder, page));
        if (!file.open(QIODevice::WriteOnly) || file.write(recognised) != recognised.size() || !file.commit()) {
            fail(tr("The recognised text couldn't be saved in %1.").arg(folder));
            return;
        }
        const int now = ++done;
        if (control.progress)
            control.progress(now, pages);
    });

    if (control.cancel && control.cancel->load() && !failed) {
        if (cancelled)
            *cancelled = true;
        return false;
    }
    if (failed) {
        *error = failure;
        return false;
    }

    QByteArray book;
    for (int page = 1; page <= pages; ++page) {
        QFile file(pageFile(folder, page));
        if (!file.open(QIODevice::ReadOnly)) {
            *error = tr("The recognised text couldn't be saved in %1.").arg(folder);
            return false;
        }
        book += file.readAll() + '\f';
    }
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly) || file.write(book) != book.size() || !file.commit()) {
        *error = tr("The recognised text couldn't be saved in %1.").arg(QFileInfo(target).path());
        return false;
    }
    QDir(folder).removeRecursively();
    *text = QString::fromUtf8(book);
    return true;
}
