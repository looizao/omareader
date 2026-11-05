#pragma once

#include "book.h"
#include "kokoro.h"
#include "qwen.h"
#include "speech.h"
#include "voicesettings.h"

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
    // Work nobody is waiting to hear (exports) yields to work someone is.
    bool background = false;
    // Previews jump ahead of everything else.
    bool preview = false;
};

namespace Synthesis {
// The silence after a sentence: the paragraph or heading pause when it ends
// one, otherwise none. Narration and exports pause alike.
double pauseAfter(const Book &book, int sentence, const VoiceSettings &voice);
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
    struct Line {
        int id;
        QString text;
    };

    static constexpr int SampleRate = 24000;

    static std::unique_ptr<Synthesizer> create(const QString &engine, QObject *parent = nullptr);
    // Whether the engine can run with these options, and why not.
    static QString problem(const SynthesisOptions &options);

    using QObject::QObject;
    ~Synthesizer() override = default;

    virtual QString engine() const = 0;
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
