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

// The sleep timer: how much listening is left before the narration pauses on
// its own, or that it pauses where the chapter being heard ends. It counts
// only time the voice is heard, so a pause to look a word up costs nothing.
// It stops the stream at the start of a sentence, so the one being heard is
// finished and nothing of the next one is heard.
class SleepTimer {
public:
    enum Mode { Off, Minutes, ChapterEnd };

    // How long past its time it lets a sentence run on before cutting it,
    // for the odd sentence that lasts a minute.
    static constexpr qint64 GraceMs = 15000;

    // A sentence queued on the stream: its chapter and where it starts.
    struct Queued {
        int chapter;
        qint64 startFrame;
    };

    Mode mode() const { return m_mode; }
    bool isOn() const { return m_mode != Off; }
    // The minutes it was set to, 0 when it isn't counting minutes.
    int minutes() const { return m_mode == Minutes ? m_minutes : 0; }
    // What is left, never below zero; -1 when it isn't counting minutes.
    qint64 msLeft() const { return m_mode == Minutes ? qMax<qint64>(0, m_leftMs) : -1; }
    // Past its time by more than the grace: stop now, mid-sentence or not.
    bool overdue() const { return m_mode == Minutes && m_leftMs <= -GraceMs; }

    void setMinutes(int minutes);
    void setChapterEnd();
    void cancel() { m_mode = Off; }
    void listen(qint64 ms);
    // Where the stream should run dry, as a frame on it: the start of the
    // first queued sentence after the time is up, or of the first one in
    // another chapter than the sentence before it. Only sentences after the
    // one at playedFrame count, so a timer set again where the last one
    // stopped goes on to the next chapter. -1 while that sentence isn't
    // queued yet.
    qint64 stopFrame(const QList<Queued> &queued, qint64 playedFrame, int sampleRate) const;

private:
    Mode m_mode = Off;
    int m_minutes = 0;
    qint64 m_leftMs = 0;
};

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
    // The sleep timer: the minutes it was set to (0 when off or at the end
    // of the chapter), whether it waits for the chapter to end, and the
    // seconds of listening left (-1 unless it counts minutes).
    Q_PROPERTY(bool sleepTimerOn READ sleepTimerOn NOTIFY sleepChanged)
    Q_PROPERTY(int sleepMinutes READ sleepMinutes NOTIFY sleepChanged)
    Q_PROPERTY(bool sleepAtChapterEnd READ sleepAtChapterEnd NOTIFY sleepChanged)
    Q_PROPERTY(int sleepSecondsLeft READ sleepSecondsLeft NOTIFY sleepChanged)

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
    bool sleepTimerOn() const { return m_sleep.isOn(); }
    int sleepMinutes() const { return m_sleep.minutes(); }
    bool sleepAtChapterEnd() const { return m_sleep.mode() == SleepTimer::ChapterEnd; }
    int sleepSecondsLeft() const;

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
    // Says these words with the current voice, as a preview: how a
    // respelling sounds before it is saved.
    Q_INVOKABLE void previewText(const QString &text);
    Q_INVOKABLE void stopPreview();
    // Pauses the narration after this many minutes of listening (0 turns
    // the timer off), or where the chapter being heard ends.
    Q_INVOKABLE void sleepAfter(int minutes);
    Q_INVOKABLE void sleepAtEndOfChapter();
    Q_INVOKABLE void cancelSleep();
    // For tests: counts listening time as the voice would, with no audio.
    void listenForTests(qint64 ms);
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
    void sleepChanged();
    // The sleep timer paused the narration.
    void fellAsleep();
    // This much more of the book's audio was played, counted from the frames
    // the player sent out, so a wait for the voice to catch up never counts.
    void heard(qint64 ms);

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
    // Counts what was heard since the last check and pauses if the stream
    // reached the sleep timer's stopping point; true when it did.
    bool checkSleep(qint64 heardMs, qint64 frame);
    // Tells the player where the sleep timer wants the stream to stop.
    void updateSleepLimit();
    // Announces the audio played since the last count, once there is at
    // least minimumMs of it.
    void countHeard(qint64 minimumMs);

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
    Lexicon m_runLexicon;
    int m_nextToAppend = 0;
    QHash<int, Ready> m_ready;
    QSet<int> m_failed;
    QList<Entry> m_timeline;
    int m_entry = 0;

    std::unique_ptr<Synthesizer> m_previewSynth;
    AudioPlayer m_previewPlayer;
    QTimer m_previewTicker;
    bool m_previewing = false;

    SleepTimer m_sleep;
    // The played frame the sleep timer last counted up to.
    qint64 m_sleepFrame = 0;
    // The played frame the time heard was last counted up to.
    qint64 m_heardFrame = 0;
};
