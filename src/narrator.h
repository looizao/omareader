#pragma once

#include "audioplayer.h"
#include "book.h"
#include "kokoro.h"
#include "qwen.h"
#include "speech.h"
#include "synthesizer.h"
#include "voicesettings.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <optional>

// Reads the book aloud and tracks the word being heard. The selected engine
// (Kokoro or Qwen3-TTS) synthesises ahead in batches of sentences; each
// sentence's audio joins one continuous stream, with a pause after each
// paragraph, and a timer maps the stream's position back to a sentence and
// a word for the highlight.
class Narrator : public QObject {
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY setupChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY setupChanged)
    Q_PROPERTY(QVariantMap setup READ setupInfo NOTIFY setupChanged)
    Q_PROPERTY(QString hardwareAdvice READ hardwareAdvice NOTIFY setupChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int sentence READ sentence NOTIFY positionChanged)
    Q_PROPERTY(int chapter READ chapter NOTIFY positionChanged)
    Q_PROPERTY(int paragraph READ paragraph NOTIFY positionChanged)
    Q_PROPERTY(int sentenceStart READ sentenceStart NOTIFY positionChanged)
    Q_PROPERTY(int sentenceEnd READ sentenceEnd NOTIFY positionChanged)
    Q_PROPERTY(int wordStart READ wordStart NOTIFY wordChanged)
    Q_PROPERTY(int wordEnd READ wordEnd NOTIFY wordChanged)
    // The word after the spoken one, when it is in the same paragraph (-1
    // otherwise), and how long the voice takes from the start of the spoken
    // word to the start of that one (0 when unknown), so the reading glass
    // can move at the pace of the voice.
    Q_PROPERTY(int nextWordStart READ nextWordStart NOTIFY wordChanged)
    Q_PROPERTY(int nextWordEnd READ nextWordEnd NOTIFY wordChanged)
    Q_PROPERTY(double wordSeconds READ wordSeconds NOTIFY wordChanged)
    Q_PROPERTY(QString engine READ engine WRITE setEngine NOTIFY engineChanged)
    Q_PROPERTY(QVariantList engines READ engines NOTIFY setupChanged)
    Q_PROPERTY(QString voice READ voice WRITE setVoice NOTIFY voiceChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY voiceChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(QVariantList voices READ voices NOTIFY engineChanged)
    Q_PROPERTY(QVariantList languages READ languages NOTIFY engineChanged)
    Q_PROPERTY(QVariantList tuning READ tuning NOTIFY tuningChanged)
    Q_PROPERTY(bool tuned READ tuned NOTIFY tuningChanged)
    Q_PROPERTY(bool previewing READ previewing NOTIFY previewChanged)

public:
    enum State { Stopped, Preparing, Playing, Paused };
    Q_ENUM(State)

    explicit Narrator(QObject *parent = nullptr);
    ~Narrator() override;

    void setBook(BookPtr book);
    BookPtr book() const { return m_book; }

    State state() const { return m_state; }
    bool active() const { return m_state != Stopped; }
    bool available() const { return problem().isEmpty(); }
    QString problem() const;
    QVariantMap setupInfo() const;
    QString hardwareAdvice() const;
    KokoroSetup kokoroSetup() const { return m_kokoroSetup; }
    QwenSetup qwenSetup() const { return m_qwenSetup; }
    // A copy of everything the current voice is made of, for an export to keep.
    SynthesisOptions synthesisOptions() const;
    QString error() const { return m_error; }
    int sentence() const { return m_sentence; }
    int chapter() const;
    int paragraph() const;
    int sentenceStart() const;
    int sentenceEnd() const;
    int wordStart() const;
    int wordEnd() const;
    int nextWordStart() const;
    int nextWordEnd() const;
    double wordSeconds() const { return active() ? m_wordSeconds : 0; }
    QString engine() const { return m_settings.engine; }
    QVariantList engines() const;
    QString voice() const { return m_settings.voice; }
    QString language() const { return m_settings.language; }
    double speed() const { return m_settings.speed; }
    VoiceSettings voiceSettings() const { return m_settings; }
    QVariantList voices() const;
    QVariantList languages() const;
    QVariantList tuning() const;
    bool tuned() const;
    bool previewing() const { return m_previewing; }

    void setEngine(const QString &engine);
    void setVoice(const QString &voice);
    void setLanguage(const QString &language);
    void setSpeed(double speed);
    Q_INVOKABLE void setTuning(const QString &key, const QVariant &value);
    // Back to the recommended speed and tuning for the current engine.
    Q_INVOKABLE void resetTuning();
    // Settings may point at a koko binary or model directory of the user's choosing.
    void setOverrides(const QString &binary, const QString &modelDirectory);

    Q_INVOKABLE void play(int sentence);
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void toggle(int fromSentence);
    Q_INVOKABLE void skip(int sentences);
    Q_INVOKABLE void refreshSetup();
    Q_INVOKABLE void clearError();
    // Speaks a short sample with the current engine, voice and tuning,
    // pausing the narration first.
    Q_INVOKABLE void preview();
    Q_INVOKABLE void stopPreview();
    // For tests: puts the narration at a word in a state, with no audio, so
    // the window can be checked on machines without an audio device.
    void showForTests(State state, int sentence, int word, double wordSeconds = 0);

    // How many sentences the engine gets per run, and how far ahead of the
    // listener synthesis may go before it is held.
    static constexpr int RunLength = 120;
    static constexpr int HoldAhead = 24;
    static constexpr int ReleaseAhead = 8;
    // How long settings must stay unchanged before the audio is rebuilt.
    static constexpr int RestartDelay = 350;

signals:
    void stateChanged();
    void setupChanged();
    void errorChanged();
    void positionChanged();
    void wordChanged();
    void engineChanged();
    void voiceChanged();
    void speedChanged();
    void tuningChanged();
    void previewChanged();
    void finishedBook();

private:
    struct Ready {
        Audio audio;
        QList<WordTiming> timings;
    };
    struct Entry {
        int sentence;
        qint64 startFrame;
        qint64 frames;
        QList<WordTiming> timings;
    };

    void setState(State state);
    void setError(const QString &error);
    void setPosition(int sentence, int word, double wordSeconds = 0);
    void startRun(int from);
    void onSentenceReady(int id, const SpokenSentence &sentence);
    void onSentenceFailed(int id);
    void onRunFinished(bool ok, const QString &error);
    void appendReady();
    void updateHold();
    void tick();
    bool skippable(int sentence) const;
    void halt();
    // Rebuilds what is buffered with the current settings, from the
    // sentence being heard, keeping the narration paused if it was.
    void restart();
    void settingsChanged();
    void ensureSynthesizer();
    double pauseAfter(int sentence) const;
    void setPreviewing(bool previewing);

    BookPtr m_book;
    VoiceSettings m_settings;
    KokoroSetup m_kokoroSetup;
    QwenSetup m_qwenSetup;
    mutable std::optional<QwenHardware> m_hardware;
    QString m_binaryOverride, m_modelOverride;
    std::unique_ptr<Synthesizer> m_synth;
    AudioPlayer m_player;
    QTimer m_ticker;
    QTimer m_restartTimer;
    State m_state = Stopped;
    QString m_error;

    int m_sentence = -1;
    int m_word = -1;
    double m_wordSeconds = 0;
    int m_runEnd = 0;
    int m_nextToAppend = 0;
    QHash<int, Ready> m_ready;
    QSet<int> m_failed;
    QList<Entry> m_timeline;
    int m_entry = 0;

    std::unique_ptr<Synthesizer> m_previewSynth;
    AudioPlayer m_previewPlayer;
    QTimer m_previewTicker;
    bool m_previewing = false;
};
