#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// The pieces between Kokoro's output files and the highlight: its word
// timestamps, matched back onto the words the reader shows, and its WAV audio.

struct SpokenWord {
    QString word;
    double start = 0;
    double end = 0;
};

struct WordTiming {
    double start = 0;
    double end = 0;
};

struct Audio {
    int sampleRate = 0;
    QByteArray samples; // mono, 32-bit float
    qint64 frames() const { return samples.size() / qint64(sizeof(float)); }
    double seconds() const { return sampleRate > 0 ? double(frames()) / sampleRate : 0; }
};

namespace Speech {
// Kokoros' timestamp sidecar: a header row, then word, start_sec, end_sec.
QList<SpokenWord> parseTimestamps(const QByteArray &tsv);

// One timing per text word. Kokoro's words are matched in order onto the
// text's (it reports punctuation as words of its own and can split or merge
// hyphenated ones); words it skipped are interpolated between their
// neighbours, and when too little matches the timing falls back to spreading
// the sentence's duration over the words by length.
QList<WordTiming> align(const QStringList &textWords, const QList<SpokenWord> &spoken, double duration);
QList<WordTiming> estimate(const QStringList &textWords, double duration);

// Linear interpolation to another sample rate. Engines that speak at a rate
// other than the narration's are brought to it before playing.
Audio resample(const Audio &audio, int sampleRate);

// Plays the audio speed times faster (0.5 to 2.0) without changing its
// pitch, by waveform-similarity overlap-add (WSOLA): short windows are taken
// from the input at the new pace, each nudged to where it best continues the
// one before, and cross-faded. A word at t seconds lands at t / speed.
Audio timeStretch(const Audio &audio, double speed);

// Multiplies the samples by gain, clipped to [-1, 1].
void applyGain(Audio *audio, double gain);

// Reads a RIFF WAV with 16-bit integer or 32-bit float samples, downmixed to mono.
bool readWav(const QString &path, Audio *audio, QString *error);
bool parseWav(const QByteArray &data, Audio *audio, QString *error);
}
