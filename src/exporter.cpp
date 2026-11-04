#include "exporter.h"

#include "filepicker.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

namespace {
// Speech needs far less than music: 64 kbit/s mono keeps an hour near 28 MB.
const char *Bitrate = "64k";

// FFMETADATA1 escapes these with a backslash.
QString escapeMetadata(const QString &value) {
    QString out;
    for (const QChar c : value) {
        if (c == '=' || c == ';' || c == '#' || c == '\\' || c == '\n')
            out += '\\';
        out += c;
    }
    return out;
}

// The concat demuxer's list quotes names in single quotes.
QString quoteForConcat(const QString &name) {
    return "'" + QString(name).replace("'", "'\\''") + "'";
}

void writeCoverArguments(QStringList &arguments, int coverInput) {
    arguments << "-map" << QString("%1:v").arg(coverInput) << "-c:v" << "copy"
              << "-disposition:v:0" << "attached_pic";
}

void writeFormatArguments(QStringList &arguments, Exporter::Format format) {
    if (format == Exporter::Mp3)
        arguments << "-id3v2_version" << "3";
    else
        arguments << "-f" << "mp4" << "-movflags" << "+faststart";
}
}

Exporter::Exporter(QObject *parent) : QObject(parent) {
    connect(&m_runner, &KokoroRunner::sentenceReady, this, &Exporter::onSentenceReady);
    connect(&m_runner, &KokoroRunner::sentenceFailed, this, [this](int id) {
        m_failed.insert(id);
        writeReady();
    });
    connect(&m_runner, &KokoroRunner::finished, this, &Exporter::onRunFinished);
    QSettings settings;
    m_layout = qBound(0, settings.value("export/layout", int(SingleFile)).toInt(), int(ChapterFiles));
    m_format = qBound(0, settings.value("export/format", int(Mp3)).toInt(), int(M4b));
    m_folder = settings.value("export/folder").toString();
    if (m_folder.isEmpty())
        m_folder = defaultFolder();
}

QString Exporter::defaultFolder() {
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return QDir(music.isEmpty() ? QDir::homePath() : music).filePath("Audiobooks");
}

void Exporter::setLayout(int layout) {
    layout = qBound(0, layout, int(ChapterFiles));
    if (layout == m_layout)
        return;
    m_layout = layout;
    QSettings().setValue("export/layout", layout);
    emit preferencesChanged();
}

void Exporter::setFormat(int format) {
    format = qBound(0, format, int(M4b));
    if (format == m_format)
        return;
    m_format = format;
    QSettings().setValue("export/format", format);
    emit preferencesChanged();
}

void Exporter::setFolder(const QString &folder) {
    if (folder.isEmpty() || folder == m_folder)
        return;
    m_folder = folder;
    QSettings().setValue("export/folder", folder);
    emit preferencesChanged();
}

void Exporter::setPicker(FilePicker *picker) {
    m_picker = picker;
    if (m_picker)
        connect(m_picker, &FilePicker::folderSelected, this, [this](const QUrl &url) {
            if (url.isLocalFile())
                setFolder(url.toLocalFile());
        });
}

void Exporter::chooseFolder() {
    if (m_picker)
        m_picker->chooseFolder(m_folder);
}

void Exporter::showResult() {
    if (m_result.isEmpty())
        return;
    const QFileInfo info(m_result);
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? info.absoluteFilePath() : info.absolutePath()));
}

Exporter::~Exporter() { reset(); }

double Exporter::progress() const {
    if (!m_running && m_track >= m_tracks.size())
        return 1;
    return m_totalWords > 0 ? qBound(0.0, double(m_doneWords) / m_totalWords, 1.0) : 0;
}

QString Exporter::chapterTitle() const {
    return m_track >= 0 && m_track < m_tracks.size() ? m_tracks.at(m_track).title : QString();
}

QString Exporter::findFfmpeg() { return QStandardPaths::findExecutable("ffmpeg"); }

QList<Exporter::Track> Exporter::plan(const Book &book, int firstChapter, int lastChapter) {
    QList<Track> tracks;
    const int chapters = int(book.chapters.size());
    if (lastChapter < 0 || lastChapter >= chapters)
        lastChapter = chapters - 1;
    for (int c = qMax(0, firstChapter); c <= lastChapter && c < book.chapterFirstSentence.size(); ++c) {
        const int first = book.chapterFirstSentence.at(c);
        const int end = c + 1 < book.chapterFirstSentence.size() ? book.chapterFirstSentence.at(c + 1)
                                                                 : book.sentenceCount();
        Track track;
        track.chapter = c;
        track.title = book.chapters.at(c).title.simplified();
        if (track.title.isEmpty())
            track.title = tr("Chapter %1").arg(c + 1);
        for (int s = first; s < end; ++s) {
            if (!book.sentences.at(s).speakable())
                continue;
            track.sentences.append(s);
            track.words += int(book.sentences.at(s).words.size());
        }
        // A cover page or an empty section has nothing to listen to.
        if (!track.sentences.isEmpty())
            tracks.append(track);
    }
    return tracks;
}

QString Exporter::extension(Format format, Layout layout) {
    if (format == Mp3)
        return "mp3";
    // A single file is an audiobook; chapters on their own are plain AAC.
    return layout == SingleFile ? "m4b" : "m4a";
}

QString Exporter::safeName(const QString &name, const QString &fallback) {
    static const QRegularExpression forbidden(R"([\\/:*?"<>|\x00-\x1F])");
    QString out = QString(name).replace(forbidden, " ").simplified();
    // Leading dots hide files, and trailing dots or spaces confuse FAT.
    while (out.startsWith('.'))
        out.remove(0, 1);
    while (out.endsWith('.') || out.endsWith(' '))
        out.chop(1);
    out = out.trimmed();
    if (out.size() > 80) {
        out.truncate(80);
        const int space = int(out.lastIndexOf(' '));
        if (space > 40)
            out.truncate(space);
        out = out.trimmed();
    }
    return out.isEmpty() ? fallback : out;
}

QString Exporter::trackFileName(int number, int total, const QString &title, const QString &extension) {
    const int width = qMax(2, int(QString::number(total).size()));
    return QString("%1 %2.%3").arg(number, width, 10, QChar('0')).arg(safeName(title, tr("Chapter %1").arg(number)))
        .arg(extension);
}

QString Exporter::uniquePath(const QString &path, bool directory) {
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink())
        return path;
    const QFileInfo info(path);
    const QString suffix = directory || info.suffix().isEmpty() ? QString() : "." + info.suffix();
    const QString base = directory ? info.fileName() : info.completeBaseName();
    for (int n = 2;; ++n) {
        const QString candidate = info.dir().filePath(QString("%1 (%2)%3").arg(base).arg(n).arg(suffix));
        if (!QFileInfo::exists(candidate) && !QFileInfo(candidate).isSymLink())
            return candidate;
    }
}

QString Exporter::chapterMetadata(const QString &title, const QString &author, const QStringList &chapterTitles,
                                  const QList<qint64> &frames, int sampleRate) {
    QString out = ";FFMETADATA1\n";
    out += "title=" + escapeMetadata(title) + "\n";
    out += "album=" + escapeMetadata(title) + "\n";
    if (!author.isEmpty()) {
        out += "artist=" + escapeMetadata(author) + "\n";
        out += "album_artist=" + escapeMetadata(author) + "\n";
    }
    out += "genre=Audiobook\n";
    qint64 start = 0;
    for (int i = 0; i < chapterTitles.size() && i < frames.size(); ++i) {
        // Frames in the sample rate's timebase keep the marks exact.
        out += QString("\n[CHAPTER]\nTIMEBASE=1/%1\nSTART=%2\nEND=%3\ntitle=%4\n")
                   .arg(sampleRate)
                   .arg(start)
                   .arg(start + frames.at(i))
                   .arg(escapeMetadata(chapterTitles.at(i)));
        start += frames.at(i);
    }
    return out;
}

QStringList Exporter::encodeArguments(Format format, int sampleRate, const QString &coverPath,
                                      const QHash<QString, QString> &tags, const QString &output) {
    QStringList arguments{"-hide_banner", "-loglevel", "error", "-n",
                          "-f", "f32le", "-ar", QString::number(sampleRate), "-ac", "1", "-i", "pipe:0"};
    if (!coverPath.isEmpty())
        arguments << "-i" << coverPath;
    arguments << "-map" << "0:a";
    if (!coverPath.isEmpty())
        writeCoverArguments(arguments, 1);
    if (format == Mp3)
        arguments << "-c:a" << "libmp3lame" << "-b:a" << Bitrate;
    else
        arguments << "-c:a" << "aac" << "-b:a" << Bitrate;
    QStringList keys = tags.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString &key : std::as_const(keys))
        arguments << "-metadata" << key + "=" + tags.value(key);
    writeFormatArguments(arguments, format);
    arguments << output;
    return arguments;
}

QStringList Exporter::joinArguments(Format format, const QString &list, const QString &metadata,
                                    const QString &coverPath, const QString &output) {
    QStringList arguments{"-hide_banner", "-nostdin", "-loglevel", "error", "-n",
                          "-f", "concat", "-safe", "0", "-i", list, "-i", metadata};
    if (!coverPath.isEmpty())
        arguments << "-i" << coverPath;
    arguments << "-map" << "0:a" << "-map_metadata" << "1" << "-map_chapters" << "1";
    if (!coverPath.isEmpty())
        writeCoverArguments(arguments, 2);
    arguments << "-c:a" << "copy";
    writeFormatArguments(arguments, format);
    arguments << output;
    return arguments;
}

bool Exporter::start(BookPtr book, const Options &options) {
    if (m_running)
        return false;
    reset();
    clearError();
    clearResult();
    m_book = std::move(book);
    m_options = options;
    m_title = m_book ? m_book->title : QString();
    emit runningChanged();

    if (!m_book) {
        fail(tr("Open a book to export it."));
        return false;
    }
    m_tracks = plan(*m_book, options.firstChapter, options.lastChapter);
    if (m_tracks.isEmpty()) {
        fail(tr("This book has nothing to read aloud."));
        return false;
    }
    if (!options.setup.ready()) {
        fail(options.setup.problem());
        return false;
    }
    if (m_options.ffmpeg.isEmpty())
        m_options.ffmpeg = findFfmpeg();
    if (m_options.ffmpeg.isEmpty() || !QFileInfo(m_options.ffmpeg).isExecutable()) {
        fail(tr("Exporting audio needs ffmpeg, which was not found. Install it with: omarchy-pkg-add ffmpeg"));
        return false;
    }
    if (m_options.folder.isEmpty() || !QDir().mkpath(m_options.folder)) {
        fail(tr("Could not create the folder %1.").arg(m_options.folder));
        return false;
    }
    // Beside the destination, so finished files are renamed into place
    // rather than copied, and never sit half-written under their real name.
    m_work = std::make_unique<QTemporaryDir>(QDir(m_options.folder).filePath(".omareader-export-XXXXXX"));
    if (!m_work->isValid()) {
        fail(tr("Could not write in the folder %1.").arg(m_options.folder));
        return false;
    }
    m_cover = writeCover();
    for (const Track &track : std::as_const(m_tracks))
        m_totalWords += track.words;

    m_running = true;
    emit runningChanged();
    emit progressChanged();
    startTrack();
    return true;
}

QString Exporter::writeCover() const {
    if (!m_book || m_book->cover.isEmpty() || !m_work)
        return {};
    QImage image;
    if (!image.loadFromData(m_book->cover) || image.isNull())
        return {};
    // Players show covers small; a phone gains nothing past 1400 pixels.
    if (image.width() > 1400 || image.height() > 1400)
        image = image.scaled(1400, 1400, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (image.hasAlphaChannel()) {
        QImage opaque(image.size(), QImage::Format_RGB32);
        opaque.fill(Qt::white);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                const double a = c.alphaF();
                opaque.setPixel(x, y, qRgb(int(c.red() * a + 255 * (1 - a)), int(c.green() * a + 255 * (1 - a)),
                                           int(c.blue() * a + 255 * (1 - a))));
            }
        image = opaque;
    }
    const QString path = m_work->filePath("cover.jpg");
    return image.convertToFormat(QImage::Format_RGB888).save(path, "JPEG", 90) ? path : QString();
}

void Exporter::startTrack() {
    if (!m_running)
        return;
    ++m_track;
    m_next = 0;
    m_ready.clear();
    m_failed.clear();
    m_runDone = false;
    m_trackFrames = 0;
    emit progressChanged();
    if (m_track >= m_tracks.size()) {
        assemble();
        return;
    }
    QList<KokoroRunner::Line> lines;
    for (const int sentence : std::as_const(m_tracks.at(m_track).sentences))
        lines.append({sentence, m_book->sentenceText(sentence)});
    m_runner.start({m_options.setup, m_options.voice, m_options.speed, true}, lines);
}

void Exporter::onSentenceReady(int id, const QString &wav, const QString &tsv) {
    Audio audio;
    const bool ok = Speech::readWav(wav, &audio, nullptr);
    // Exports don't need the word timings, and a long book's worth of WAVs
    // would fill the temporary directory.
    QFile::remove(wav);
    QFile::remove(tsv);
    if (ok && audio.frames() > 0)
        m_ready.insert(id, audio);
    else
        m_failed.insert(id);
    writeReady();
}

void Exporter::onRunFinished(bool ok, const QString &error) {
    if (!m_running)
        return;
    if (!ok) {
        fail(error);
        return;
    }
    // Sentences Kokoros never reported on are left out rather than waited for.
    const Track &track = m_tracks.at(m_track);
    for (int k = m_next; k < track.sentences.size(); ++k)
        if (!m_ready.contains(track.sentences.at(k)))
            m_failed.insert(track.sentences.at(k));
    m_runDone = true;
    writeReady();
}

void Exporter::writeReady() {
    if (!m_running || m_track < 0 || m_track >= m_tracks.size())
        return;
    const Track &track = m_tracks.at(m_track);
    bool advanced = false;
    while (m_next < track.sentences.size()) {
        const int id = track.sentences.at(m_next);
        const Sentence &sentence = m_book->sentences.at(id);
        if (!m_failed.contains(id)) {
            auto it = m_ready.find(id);
            if (it == m_ready.end())
                break;
            if (!m_encoder && !startEncoder(it->sampleRate))
                return;
            if (it->sampleRate != m_sampleRate) {
                fail(tr("Kokoros produced %1 Hz audio partway through; the export expects %2 Hz.")
                         .arg(it->sampleRate)
                         .arg(m_sampleRate));
                return;
            }
            writeSamples(it->samples);
            m_ready.erase(it);
            const bool lastInParagraph = id + 1 >= m_book->sentenceCount()
                                         || m_book->sentences.at(id + 1).paragraph != sentence.paragraph
                                         || m_book->sentences.at(id + 1).chapter != sentence.chapter;
            if (lastInParagraph) {
                const int level = m_book->chapters.at(sentence.chapter).paragraphs.at(sentence.paragraph).level;
                const double pause = level > 0 ? HeadingPause : ParagraphPause;
                writeSamples(QByteArray(qsizetype(pause * m_sampleRate) * qsizetype(sizeof(float)), '\0'));
            }
        }
        m_doneWords += int(sentence.words.size());
        ++m_next;
        advanced = true;
    }
    if (advanced)
        emit progressChanged();
    if (m_runDone && m_next >= track.sentences.size())
        finishEncoder();
}

void Exporter::writeSamples(const QByteArray &samples) {
    m_encoder->write(samples);
    m_trackFrames += samples.size() / qsizetype(sizeof(float));
}

bool Exporter::startEncoder(int sampleRate) {
    if (m_sampleRate == 0)
        m_sampleRate = sampleRate;
    const Track &track = m_tracks.at(m_track);
    const QString extension = Exporter::extension(m_options.format, m_options.layout);
    QString name;
    QString cover;
    QHash<QString, QString> tags;
    if (m_options.layout == ChapterFiles) {
        name = trackFileName(m_track + 1, int(m_tracks.size()), track.title, extension);
        cover = m_cover;
        tags = {{"title", track.title},
                {"album", m_book->title},
                {"track", QString("%1/%2").arg(m_track + 1).arg(m_tracks.size())},
                {"genre", "Audiobook"}};
        if (!m_book->author.isEmpty()) {
            tags.insert("artist", m_book->author);
            tags.insert("album_artist", m_book->author);
        }
    } else {
        // Parts of the one file only need names the concat list can take.
        name = QString("part-%1.%2").arg(m_track + 1, 4, 10, QChar('0')).arg(extension == "m4b" ? "m4a" : extension);
    }
    m_encoder = std::make_unique<QProcess>();
    m_encoder->setProcessChannelMode(QProcess::SeparateChannels);
    m_encoder->setStandardOutputFile(QProcess::nullDevice());
    m_encoder->setWorkingDirectory(m_work->path());
    connect(m_encoder.get(), &QProcess::finished, this, &Exporter::onEncoderFinished);
    m_encoder->setProperty("output", name);
    m_encoder->start(m_options.ffmpeg, encodeArguments(m_options.format, m_sampleRate, cover, tags, m_work->filePath(name)));
    if (!m_encoder->waitForStarted(10000)) {
        fail(tr("ffmpeg could not be started: %1").arg(m_encoder->errorString()));
        return false;
    }
    return true;
}

void Exporter::finishEncoder() {
    if (!m_encoder) {
        // Kokoros said nothing for this chapter, so it gets no file.
        startTrack();
        return;
    }
    m_encoder->closeWriteChannel();
}

QString Exporter::processError(QProcess &process) const {
    const QString message = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return message.size() > 400 ? message.right(400) : message;
}

void Exporter::onEncoderFinished(int code, QProcess::ExitStatus status) {
    if (!m_running || !m_encoder)
        return;
    std::unique_ptr<QProcess> encoder = std::move(m_encoder);
    encoder->disconnect(this);
    const QString name = encoder->property("output").toString();
    const QString error = processError(*encoder);
    encoder.release()->deleteLater();
    if (status != QProcess::NormalExit || code != 0) {
        fail(tr("ffmpeg could not encode the audio: %1").arg(error));
        return;
    }
    m_trackFiles.append(name);
    m_trackTitles.append(m_tracks.at(m_track).title);
    m_trackLengths.append(m_trackFrames);
    startTrack();
}

void Exporter::assemble() {
    if (m_trackFiles.isEmpty()) {
        fail(tr("Kokoros produced no audio for this book."));
        return;
    }
    const QString bookName = safeName(m_book->title);
    if (m_options.layout == ChapterFiles) {
        // The work folder becomes the book's folder in one rename.
        const QString target = uniquePath(QDir(m_options.folder).filePath(bookName), true);
        const QString work = m_work->path();
        if (!m_cover.isEmpty() && QFileInfo(m_cover).fileName() != "cover.jpg")
            QFile::rename(m_cover, QDir(work).filePath("cover.jpg"));
        if (!QDir().rename(work, target)) {
            fail(tr("Could not move the audio into %1.").arg(target));
            return;
        }
        m_work->setAutoRemove(false);
        m_work.reset();
        // Temporary folders are private; the book's folder is an ordinary one.
        QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup
                                          | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther);
        succeed(target);
        return;
    }

    QSaveFile list(m_work->filePath("parts.txt"));
    QSaveFile metadata(m_work->filePath("chapters.txt"));
    if (!list.open(QIODevice::WriteOnly) || !metadata.open(QIODevice::WriteOnly)) {
        fail(tr("Could not write in the folder %1.").arg(m_options.folder));
        return;
    }
    for (const QString &file : std::as_const(m_trackFiles))
        list.write(("file " + quoteForConcat(file) + "\n").toUtf8());
    metadata.write(chapterMetadata(m_book->title, m_book->author, m_trackTitles, m_trackLengths, m_sampleRate).toUtf8());
    if (!list.commit() || !metadata.commit()) {
        fail(tr("Could not write in the folder %1.").arg(m_options.folder));
        return;
    }

    const QString output = m_work->filePath("book." + extension(m_options.format, m_options.layout));
    m_joiner = std::make_unique<QProcess>();
    m_joiner->setProcessChannelMode(QProcess::SeparateChannels);
    m_joiner->setStandardOutputFile(QProcess::nullDevice());
    m_joiner->setWorkingDirectory(m_work->path());
    m_joiner->setProperty("output", output);
    connect(m_joiner.get(), &QProcess::finished, this, &Exporter::onJoinFinished);
    m_joiner->start(m_options.ffmpeg,
                    joinArguments(m_options.format, list.fileName(), metadata.fileName(), m_cover, output));
}

void Exporter::onJoinFinished(int code, QProcess::ExitStatus status) {
    if (!m_running || !m_joiner)
        return;
    std::unique_ptr<QProcess> joiner = std::move(m_joiner);
    joiner->disconnect(this);
    const QString output = joiner->property("output").toString();
    const QString error = processError(*joiner);
    joiner.release()->deleteLater();
    if (status != QProcess::NormalExit || code != 0) {
        fail(tr("ffmpeg could not join the chapters: %1").arg(error));
        return;
    }
    const QString target = uniquePath(
        QDir(m_options.folder).filePath(safeName(m_book->title) + "." + extension(m_options.format, m_options.layout)),
        false);
    if (!QFile::rename(output, target)) {
        fail(tr("Could not move the audio into %1.").arg(target));
        return;
    }
    succeed(target);
}

void Exporter::cancel() {
    if (!m_running)
        return;
    reset();
    emit runningChanged();
    emit progressChanged();
    emit finished(false, tr("The export was cancelled."));
}

void Exporter::clearError() {
    if (m_error.isEmpty())
        return;
    m_error.clear();
    emit errorChanged();
}

void Exporter::clearResult() {
    if (m_result.isEmpty())
        return;
    m_result.clear();
    emit resultChanged();
}

void Exporter::fail(const QString &message) {
    reset();
    m_error = message;
    emit errorChanged();
    emit runningChanged();
    emit progressChanged();
    emit finished(false, message);
}

void Exporter::succeed(const QString &path) {
    m_work.reset();
    m_running = false;
    m_book.reset();
    m_result = path;
    emit resultChanged();
    emit runningChanged();
    emit progressChanged();
    emit finished(true, path);
}

void Exporter::reset() {
    m_running = false;
    m_runner.stop();
    for (std::unique_ptr<QProcess> *process : {&m_encoder, &m_joiner}) {
        if (!*process)
            continue;
        (*process)->disconnect(this);
        (*process)->kill();
        (*process)->waitForFinished(2000);
        process->reset();
    }
    m_work.reset(); // removes everything written so far
    m_book.reset();
    m_cover.clear();
    m_tracks.clear();
    m_totalWords = 0;
    m_doneWords = 0;
    m_track = -1;
    m_next = 0;
    m_ready.clear();
    m_failed.clear();
    m_runDone = false;
    m_sampleRate = 0;
    m_trackFrames = 0;
    m_trackFiles.clear();
    m_trackTitles.clear();
    m_trackLengths.clear();
}
