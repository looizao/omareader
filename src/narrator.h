#pragma once

#include "audioplayer.h"
#include "book.h"
#include "kokoro.h"
#include "speech.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

// Reads the book aloud with Kokoros and tracks the word being heard. Kokoros
// synthesises ahead in batches of sentences; each sentence's audio joins one
// continuous stream, and a timer maps the stream's position back to a
// sentence and a word for the highlight.
class Narrator : public QObject {
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY setupChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY setupChanged)
    Q_PROPERTY(QVariantMap setup READ setupInfo NOTIFY setupChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int sentence READ sentence NOTIFY positionChanged)
    Q_PROPERTY(int chapter READ chapter NOTIFY positionChanged)
    Q_PROPERTY(int paragraph READ paragraph NOTIFY positionChanged)
    Q_PROPERTY(int sentenceStart READ sentenceStart NOTIFY positionChanged)
    Q_PROPERTY(int sentenceEnd READ sentenceEnd NOTIFY positionChanged)
    Q_PROPERTY(int wordStart READ wordStart NOTIFY wordChanged)
    Q_PROPERTY(int wordEnd READ wordEnd NOTIFY wordChanged)
    Q_PROPERTY(QString voice READ voice WRITE setVoice NOTIFY voiceChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(QVariantList voices READ voices CONSTANT)

public:
    enum State { Stopped, Preparing, Playing, Paused };
    Q_ENUM(State)

    explicit Narrator(QObject *parent = nullptr);
    ~Narrator() override;

    void setBook(BookPtr book);
    BookPtr book() const { return m_book; }

    State state() const { return m_state; }
    bool active() const { return m_state != Stopped; }
    bool available() const { return m_setup.ready(); }
    QString problem() const { return m_setup.problem(); }
    QVariantMap setupInfo() const;
    KokoroSetup kokoroSetup() const { return m_setup; }
    QString error() const { return m_error; }
    int sentence() const { return m_sentence; }
    int chapter() const;
    int paragraph() const;
    int sentenceStart() const;
    int sentenceEnd() const;
    int wordStart() const;
    int wordEnd() const;
    QString voice() const { return m_voice; }
    double speed() const { return m_speed; }
    QVariantList voices() const;

    void setVoice(const QString &voice);
    void setSpeed(double speed);
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

    // How many sentences Kokoros gets per run, and how far ahead of the
    // listener synthesis may go before it is held.
    static constexpr int RunLength = 120;
    static constexpr int HoldAhead = 24;
    static constexpr int ReleaseAhead = 8;

signals:
    void stateChanged();
    void setupChanged();
    void errorChanged();
    void positionChanged();
    void wordChanged();
    void voiceChanged();
    void speedChanged();
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
    void setPosition(int sentence, int word);
    void startRun(int from);
    void onSentenceReady(int id, const QString &wav, const QString &tsv);
    void onSentenceFailed(int id);
    void onRunFinished(bool ok, const QString &error);
    void appendReady();
    void updateHold();
    void tick();
    bool skippable(int sentence) const;
    void halt();

    BookPtr m_book;
    KokoroSetup m_setup;
    QString m_binaryOverride, m_modelOverride;
    KokoroRunner m_runner;
    AudioPlayer m_player;
    QTimer m_ticker;
    State m_state = Stopped;
    QString m_error;
    QString m_voice;
    double m_speed = 1.0;

    int m_sentence = -1;
    int m_word = -1;
    int m_runEnd = 0;
    int m_nextToAppend = 0;
    QHash<int, Ready> m_ready;
    QSet<int> m_failed;
    QList<Entry> m_timeline;
    int m_entry = 0;
    int m_sampleRate = 24000;
};
