#pragma once

#include "book.h"
#include "kokoro.h"
#include "speech.h"

#include <QHash>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <memory>

class FilePicker;

// Turns a whole book into audio files for listening elsewhere, such as a
// podcast or audiobook app on a phone. Kokoros reads one chapter per run and
// ffmpeg encodes each chapter as it arrives; at the end the chapters become
// either one file with chapter markers or a folder with one file per chapter.
// Everything is built in a hidden folder beside the destination and renamed
// into place, so an export that fails or is cancelled leaves nothing behind
// and never replaces an existing file.
class Exporter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int chapter READ chapter NOTIFY progressChanged)
    Q_PROPERTY(int chapterCount READ chapterCount NOTIFY progressChanged)
    Q_PROPERTY(QString chapterTitle READ chapterTitle NOTIFY progressChanged)
    Q_PROPERTY(QString bookTitle READ bookTitle NOTIFY runningChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString result READ result NOTIFY resultChanged)
    // The choices in the export dialog, remembered between exports.
    Q_PROPERTY(int layout READ layout WRITE setLayout NOTIFY preferencesChanged)
    Q_PROPERTY(int format READ format WRITE setFormat NOTIFY preferencesChanged)
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY preferencesChanged)

public:
    enum Layout { SingleFile, ChapterFiles };
    Q_ENUM(Layout)
    enum Format { Mp3, M4b };
    Q_ENUM(Format)

    struct Options {
        KokoroSetup setup;
        QString voice = "af_heart";
        double speed = 1.0;
        Layout layout = SingleFile;
        Format format = Mp3;
        QString folder;    // where the file or the book's folder goes
        QString ffmpeg;    // empty means look it up on PATH
        int firstChapter = 0;
        int lastChapter = -1; // inclusive; -1 means the last one
    };

    // A chapter that has something to say, and the sentences it says.
    struct Track {
        int chapter = 0;
        QString title;
        QList<int> sentences;
        int words = 0;
    };

    explicit Exporter(QObject *parent = nullptr);
    ~Exporter() override;

    bool running() const { return m_running; }
    double progress() const;
    int chapter() const { return m_track + 1; }
    int chapterCount() const { return int(m_tracks.size()); }
    QString chapterTitle() const;
    QString bookTitle() const { return m_title; }
    QString error() const { return m_error; }
    QString result() const { return m_result; }

    int layout() const { return m_layout; }
    int format() const { return m_format; }
    QString folder() const { return m_folder; }
    void setLayout(int layout);
    void setFormat(int format);
    void setFolder(const QString &folder);
    // The folder chooser behind the dialog's Change button.
    void setPicker(FilePicker *picker);
    Q_INVOKABLE void chooseFolder();
    // Opens the folder holding the last export in the file manager.
    Q_INVOKABLE void showResult();
    static QString defaultFolder();

    bool start(BookPtr book, const Options &options);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void clearResult();

    static QString findFfmpeg();
    static QList<Track> plan(const Book &book, int firstChapter = 0, int lastChapter = -1);
    static QString extension(Format format, Layout layout);
    // A name that is safe on phones and memory cards (FAT and exFAT forbid
    // more characters than Linux does), never empty, and not too long.
    static QString safeName(const QString &name, const QString &fallback = QStringLiteral("Audiobook"));
    // "03 Down the Rabbit-Hole.mp3", padded so the files sort in order.
    static QString trackFileName(int number, int total, const QString &title, const QString &extension);
    // path itself when nothing is there yet, otherwise "name (2).ext" and so on.
    static QString uniquePath(const QString &path, bool directory);
    // ffmpeg's FFMETADATA1 file: the book's tags and one chapter per track.
    static QString chapterMetadata(const QString &title, const QString &author, const QStringList &chapterTitles,
                                   const QList<qint64> &frames, int sampleRate);
    static QStringList encodeArguments(Format format, int sampleRate, const QString &coverPath,
                                       const QHash<QString, QString> &tags, const QString &output);
    static QStringList joinArguments(Format format, const QString &list, const QString &metadata,
                                     const QString &coverPath, const QString &output);

    // Silence after a paragraph, so the listener hears where it ends.
    static constexpr double ParagraphPause = 0.45;
    static constexpr double HeadingPause = 0.9;

signals:
    void runningChanged();
    void progressChanged();
    void errorChanged();
    void resultChanged();
    void preferencesChanged();
    void finished(bool ok, const QString &pathOrError);

private:
    void startTrack();
    void onSentenceReady(int id, const QString &wav, const QString &tsv);
    void onRunFinished(bool ok, const QString &error);
    void writeReady();
    void writeSamples(const QByteArray &samples);
    bool startEncoder(int sampleRate);
    void finishEncoder();
    void onEncoderFinished(int code, QProcess::ExitStatus status);
    void assemble();
    void onJoinFinished(int code, QProcess::ExitStatus status);
    void fail(const QString &message);
    void succeed(const QString &path);
    void reset();
    QString writeCover() const;
    QString processError(QProcess &process) const;

    BookPtr m_book;
    Options m_options;
    QList<Track> m_tracks;
    int m_totalWords = 0;
    int m_doneWords = 0;
    bool m_running = false;
    QString m_title;
    QString m_error;
    QString m_result;
    int m_layout = SingleFile;
    int m_format = Mp3;
    QString m_folder;
    FilePicker *m_picker = nullptr;

    std::unique_ptr<QTemporaryDir> m_work;
    QString m_cover;
    KokoroRunner m_runner;
    std::unique_ptr<QProcess> m_encoder;
    std::unique_ptr<QProcess> m_joiner;
    int m_track = -1;
    int m_next = 0; // index into the track's sentences of the next one to write
    QHash<int, Audio> m_ready;
    QSet<int> m_failed;
    bool m_runDone = false;
    int m_sampleRate = 0;
    qint64 m_trackFrames = 0;
    QStringList m_trackFiles;
    QStringList m_trackTitles;
    QList<qint64> m_trackLengths;
};
