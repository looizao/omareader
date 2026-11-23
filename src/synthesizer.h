#pragma once

#include "book.h"
#include "kokoro.h"
#include "pronunciation.h"
#include "qwen.h"
#include "speech.h"
#include "voicesettings.h"

#include <QHash>
#include <QObject>
#include <memory>

// A sentence's audio, ready to play or encode, and the words heard in it
// with their times in that audio.
struct SpokenSentence {
    Audio audio;
    QList<SpokenWord> words;
};
Q_DECLARE_METATYPE(SpokenSentence)

// What to synthesise with: the voice settings plus where each engine lives.
struct SynthesisOptions {
    VoiceSettings voice;
    KokoroSetup kokoro;
    QwenSetup qwen;
    // How the reader wants names and terms said; the engine never sees it,
    // since its callers put the respellings in the text they give it.
    Lexicon lexicon;
    // Work nobody is waiting to hear (exports) yields to work someone is.
    bool background = false;
    // Previews jump ahead of everything else.
    bool preview = false;
    // Kokoro: the language to read in when it isn't the voice's own, as when
    // a second voice reads the dialogue of a book in the narration's language.
    QString kokoroLanguage = QString();
};

namespace Synthesis {
// The silence after a sentence, before the next one the voice reads: the
// chapter pause where a chapter ends, the heading or paragraph pause where
// one of those does, and otherwise the sentence pause, unless the sentence
// is only part of a long one broken for the voice. Narration and exports
// pause alike.
double pauseAfter(const Book &book, int sentence, const VoiceSettings &voice);
// The same pause as silent frames at the engines' sample rate, rounded the
// same way for narration and exports so both last exactly as long.
qint64 pauseFramesAfter(const Book &book, int sentence, const VoiceSettings &voice);
// Everything needed to speak with these voice settings, finding each engine
// where the user's settings (the koko and model overrides) say it is.
SynthesisOptions optionsFor(const VoiceSettings &voice);
}

// The contract both engines meet for narration, previews and exports: given
// numbered sentences, deliver each one's audio and word timings in order, at
// SampleRate, at the requested speed and volume, or say it failed. How an
// engine gets there (one Kokoros run per batch, a shared Qwen3-TTS process,
// different sample rates, speed in the model or by time-stretching) stays
// behind it.
class Synthesizer : public QObject {
    Q_OBJECT

public:
    // Part of a line read in one voice: the narration's, or the dialogue's.
    struct Piece {
        QString text;
        bool dialogue = false;
    };
    struct Line {
        int id;
        QString text;
        // When the dialogue has a voice of its own and the line holds some:
        // the line cut where speech starts and stops, in order. Empty when
        // the narration's voice reads it all.
        QList<Piece> pieces = {};
    };

    static constexpr int SampleRate = 24000;

    static std::unique_ptr<Synthesizer> create(const QString &engine, QObject *parent = nullptr);
    // The synthesizer for these voice settings: the engine's own, or, when
    // the dialogue has a voice of its own, one that reads each line's
    // pieces in their voices and delivers the line whole.
    static std::unique_ptr<Synthesizer> forVoice(const VoiceSettings &voice, QObject *parent = nullptr);
    // Whether the engine can run with these options, and why not.
    static QString problem(const SynthesisOptions &options);

    using QObject::QObject;
    ~Synthesizer() override = default;

    virtual QString engine() const = 0;
    // Whether it reads the lines' dialogue pieces in a voice of their own.
    virtual bool readsDialogue() const { return false; }
    // Replaces any run in progress; its sentences are never reported.
    virtual bool start(const SynthesisOptions &options, const QList<Line> &lines) = 0;
    virtual void stop() = 0;
    // Pauses synthesis while playback has plenty buffered.
    virtual void setHeld(bool held) = 0;
    virtual bool isRunning() const = 0;
    virtual bool isHeld() const = 0;

signals:
    void sentenceReady(int id, const SpokenSentence &sentence);
    void sentenceFailed(int id, const QString &message);
    void finished(bool ok, const QString &error);

protected:
    // Reads the WAV and TSV an engine wrote and removes them, then brings
    // the audio to SampleRate, plays it stretch times faster (scaling the
    // word times to match) and applies the gain.
    bool deliver(int id, const QString &wav, const QString &tsv, double stretch, double gain);
};

namespace Synthesis {
// What the engine is given for these sentences of the book: each one as the
// voice says it, with the reader's pronunciations, and, when the voice
// settings give the dialogue a voice of its own, cut into the pieces the
// narrator and the characters say. Narration, previews and exports share it.
QList<Synthesizer::Line> lines(const Book &book, const QList<int> &sentences, const SynthesisOptions &options);
}

// Reads a book's narration in one voice and its dialogue in another, with
// two synthesizers of the same engine, and delivers each line whole: its
// pieces' audio one after another, and their words with times in it, so
// the highlight follows it as it would a line read in one voice. Of the
// two, only the one with the earlier piece to say runs at a time, so they
// don't fight over the CPU or GPU and the next line is always the one
// being worked on.
class DialogueSynthesizer : public Synthesizer {
    Q_OBJECT

public:
    explicit DialogueSynthesizer(const QString &engine, QObject *parent = nullptr);
    QString engine() const override { return m_engine; }
    bool readsDialogue() const override { return true; }
    bool start(const SynthesisOptions &options, const QList<Line> &lines) override;
    void stop() override;
    void setHeld(bool held) override;
    bool isRunning() const override { return m_running; }
    bool isHeld() const override { return m_running && m_held; }

    // The options the dialogue is read with: the narration's, with the
    // dialogue voice in place of its own and no blend, in the narration's
    // language.
    static SynthesisOptions dialogueOptions(const SynthesisOptions &narration);
    // A line's pieces as one: their audio one after another, with the
    // silence each engine run starts and ends with cut down where two meet
    // so the voices hand over as quickly as a reader pauses at a comma, and
    // each piece's word times spread over what is kept of it. The line's own start and
    // end keep theirs, so the pauses between lines stay as they were.
    static SpokenSentence join(const QList<SpokenSentence> &pieces);
    // How much silence is kept on each side where two pieces meet.
    static constexpr double KeptBefore = 0.1;
    static constexpr double KeptAfter = 0.15;

private:
    enum { Narration, Speech, VoiceCount };
    struct Pending {
        int id;
        QList<int> pieces;
    };

    void onReady(int voice, int piece, const SpokenSentence &sentence);
    void onFailed(int voice, int piece, const QString &message);
    void onFinished(int voice, bool ok, const QString &error);
    // Delivers the lines whose pieces are all in, in order; false when the
    // run was stopped or replaced while it did.
    bool deliverDone();
    void balance();

    QString m_engine;
    std::unique_ptr<Synthesizer> m_voices[VoiceCount];
    // Each voice's pieces still to come, in the order it says them.
    QList<int> m_waiting[VoiceCount];
    bool m_finished[VoiceCount] = {true, true};
    QList<Pending> m_lines;
    int m_next = 0;
    QHash<int, SpokenSentence> m_spoken;
    QHash<int, QString> m_failed;
    bool m_running = false;
    bool m_held = false;
    quint64 m_run = 0;
};

class KokoroSynthesizer : public Synthesizer {
    Q_OBJECT

public:
    explicit KokoroSynthesizer(QObject *parent = nullptr);
    QString engine() const override { return Engine::Kokoro; }
    bool start(const SynthesisOptions &options, const QList<Line> &lines) override;
    void stop() override { m_runner.stop(); }
    void setHeld(bool held) override { m_runner.setHeld(held); }
    bool isRunning() const override { return m_runner.isRunning(); }
    bool isHeld() const override { return m_runner.isHeld(); }

    // Kokoros' --style: the voice, or two voices with weights in tenths.
    static QString style(const VoiceSettings &voice);

private:
    KokoroRunner m_runner;
    double m_gain = 1.0;
};

class QwenSynthesizer : public Synthesizer {
    Q_OBJECT

public:
    explicit QwenSynthesizer(QObject *parent = nullptr, QwenWorker *worker = nullptr);
    QString engine() const override { return Engine::Qwen; }
    bool start(const SynthesisOptions &options, const QList<Line> &lines) override;
    void stop() override;
    void setHeld(bool held) override;
    bool isRunning() const override { return m_running; }
    bool isHeld() const override { return m_running && m_client && m_client->isHeld(); }

    // The request fields for the worker: speaker, language, style, sampling.
    static QJsonObject voiceRequest(const VoiceSettings &voice);

private:
    void ensureClient(bool background, bool preview);

    QwenWorker *m_worker;
    std::unique_ptr<QwenClient> m_client;
    int m_priority = -1;
    std::unique_ptr<QTemporaryDir> m_directory;
    bool m_running = false;
    double m_speed = 1.0;
    double m_gain = 1.0;
};
