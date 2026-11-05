#include "speech.h"

#include "book.h"

#include <QCoreApplication>
#include <QFile>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
// Reads a little-endian value from unaligned bytes.
template <typename T> T readLE(const char *bytes) {
    T value;
    std::memcpy(&value, bytes, sizeof value);
    return qFromLittleEndian(value);
}

// The same, bounds-checked against the buffer; 0 past its end.
template <typename T> T readAt(const QByteArray &data, qsizetype offset) {
    if (offset < 0 || offset + qsizetype(sizeof(T)) > data.size())
        return 0;
    return readLE<T>(data.constData() + offset);
}
}

namespace Speech {

QList<SpokenWord> parseTimestamps(const QByteArray &tsv) {
    QList<SpokenWord> words;
    const QList<QByteArray> lines = tsv.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QList<QByteArray> columns = lines.at(i).trimmed().split('\t');
        if (columns.size() < 3)
            continue;
        bool startOk = false, endOk = false;
        const double start = columns.at(1).toDouble(&startOk);
        const double end = columns.at(2).toDouble(&endOk);
        if (!startOk || !endOk) // the header row
            continue;
        words.append({QString::fromUtf8(columns.at(0)), start, qMax(start, end)});
    }
    return words;
}

QList<WordTiming> estimate(const QStringList &textWords, double duration) {
    QList<WordTiming> timings;
    double total = 0;
    for (const auto &w : textWords)
        total += w.size() + 2; // a little weight for the gap after each word
    double cursor = 0;
    for (const auto &w : textWords) {
        const double length = total > 0 ? duration * (w.size() + 2) / total : 0;
        timings.append({cursor, cursor + length});
        cursor += length;
    }
    return timings;
}

QList<WordTiming> align(const QStringList &textWords, const QList<SpokenWord> &spoken, double duration) {
    const int count = int(textWords.size());
    if (count == 0)
        return {};
    QStringList text;
    for (const auto &w : textWords)
        text.append(Text::normalizeWord(w));

    QList<WordTiming> timings(count, WordTiming{-1, -1});
    int matched = 0;
    int next = 0;
    for (const auto &s : spoken) {
        const QString word = Text::normalizeWord(s.word);
        if (word.isEmpty())
            continue;
        // Look a few words ahead, so one word Kokoro skipped or normalised
        // differently doesn't derail the rest of the sentence.
        for (int k = next; k < count && k < next + 4; ++k) {
            const QString &t = text.at(k);
            if (t.isEmpty())
                continue;
            const bool whole = t == word;
            const bool piece = !whole && (t.startsWith(word) || t.endsWith(word));
            const bool merged = !whole && !piece && word.startsWith(t);
            if (!whole && !piece && !merged)
                continue;
            if (timings[k].start < 0) {
                timings[k] = {s.start, s.end};
                ++matched;
            } else {
                timings[k].end = s.end;
            }
            // A piece of a longer word ("well" of "well-known") keeps the
            // word open for the rest of it.
            next = piece && !t.endsWith(word) ? k : k + 1;
            break;
        }
    }

    if (matched * 10 < count * 4)
        return estimate(textWords, duration);

    // Interpolate the gaps, sharing each by word length.
    int i = 0;
    while (i < count) {
        if (timings[i].start >= 0) {
            ++i;
            continue;
        }
        int j = i;
        while (j < count && timings[j].start < 0)
            ++j;
        const double from = i > 0 ? timings[i - 1].end : 0;
        const double to = j < count ? timings[j].start : qMax(from, duration);
        double weight = 0;
        for (int k = i; k < j; ++k)
            weight += text.at(k).size() + 1;
        double cursor = from;
        for (int k = i; k < j; ++k) {
            const double length = weight > 0 ? (to - from) * (text.at(k).size() + 1) / weight : 0;
            timings[k] = {cursor, cursor + length};
            cursor += length;
        }
        i = j;
    }
    // Keep it monotonic for the player's lookups.
    for (int k = 1; k < count; ++k)
        if (timings[k].start < timings[k - 1].start)
            timings[k].start = timings[k - 1].start;
    return timings;
}

bool parseWav(const QByteArray &data, Audio *audio, QString *error) {
    auto fail = [&](const char *message) {
        if (error)
            *error = QCoreApplication::translate("Speech", message);
        return false;
    };
    if (data.size() < 12 || !data.startsWith("RIFF") || data.mid(8, 4) != "WAVE")
        return fail("The voice produced audio that is not a WAV file.");

    quint16 format = 0, channels = 0, bits = 0;
    quint32 rate = 0;
    qsizetype offset = 12;
    QByteArray pcm;
    while (offset + 8 <= data.size()) {
        const QByteArray id = data.mid(offset, 4);
        quint32 size = readAt<quint32>(data, offset + 4);
        const qsizetype body = offset + 8;
        if (id == "fmt " && body + 16 <= data.size()) {
            format = readAt<quint16>(data, body);
            channels = readAt<quint16>(data, body + 2);
            rate = readAt<quint32>(data, body + 4);
            bits = readAt<quint16>(data, body + 14);
            if (format == 0xFFFE && size >= 40 && body + 26 <= data.size()) // WAVE_FORMAT_EXTENSIBLE
                format = readAt<quint16>(data, body + 24);
        } else if (id == "data") {
            // A streaming writer may leave the size unset; take what's there.
            if (size == 0 || size == 0xFFFFFFFF || body + size > data.size())
                size = quint32(data.size() - body);
            pcm = data.mid(body, size);
            break;
        }
        offset = body + size + (size & 1);
    }
    if (channels == 0 || rate == 0)
        return fail("The voice's WAV audio has no format chunk.");
    const bool isFloat = format == 3 && bits == 32;
    const bool isInt16 = format == 1 && bits == 16;
    if (!isFloat && !isInt16)
        return fail("The voice's WAV audio uses an unsupported sample format.");

    const int bytesPerSample = bits / 8;
    const qsizetype frames = pcm.size() / (bytesPerSample * channels);
    QByteArray mono(frames * qsizetype(sizeof(float)), Qt::Uninitialized);
    float *out = reinterpret_cast<float *>(mono.data());
    const char *in = pcm.constData();
    for (qsizetype f = 0; f < frames; ++f) {
        float sum = 0;
        for (int c = 0; c < channels; ++c) {
            const char *sample = in + (f * channels + c) * bytesPerSample;
            if (isFloat) {
                const quint32 raw = readLE<quint32>(sample);
                float value;
                std::memcpy(&value, &raw, sizeof value);
                sum += value;
            } else {
                sum += readLE<qint16>(sample) / 32768.0f;
            }
        }
        out[f] = sum / channels;
    }
    audio->sampleRate = int(rate);
    audio->samples = mono;
    return true;
}

bool readWav(const QString &path, Audio *audio, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QCoreApplication::translate("Speech", "Could not read the voice's audio file %1.").arg(path);
        return false;
    }
    return parseWav(file.readAll(), audio, error);
}


Audio resample(const Audio &audio, int sampleRate) {
    if (audio.sampleRate <= 0 || sampleRate <= 0 || audio.sampleRate == sampleRate || audio.frames() == 0) {
        Audio out = audio;
        if (out.sampleRate <= 0)
            out.sampleRate = sampleRate;
        return out;
    }
    const auto *in = reinterpret_cast<const float *>(audio.samples.constData());
    const qint64 inFrames = audio.frames();
    const qint64 outFrames = qMax<qint64>(1, qint64(std::llround(double(inFrames) * sampleRate / audio.sampleRate)));
    Audio out;
    out.sampleRate = sampleRate;
    out.samples.resize(outFrames * qint64(sizeof(float)));
    auto *dst = reinterpret_cast<float *>(out.samples.data());
    const double step = double(audio.sampleRate) / sampleRate;
    for (qint64 i = 0; i < outFrames; ++i) {
        const double position = i * step;
        const qint64 index = qint64(position);
        const double fraction = position - double(index);
        const float a = in[qMin(index, inFrames - 1)];
        const float b = in[qMin(index + 1, inFrames - 1)];
        dst[i] = float(a + (b - a) * fraction);
    }
    return out;
}

Audio timeStretch(const Audio &audio, double speed) {
    speed = qBound(0.5, speed, 2.0);
    const qint64 inFrames = audio.frames();
    if (std::abs(speed - 1.0) < 1e-3 || audio.sampleRate <= 0 || inFrames == 0)
        return audio;
    const auto *in = reinterpret_cast<const float *>(audio.samples.constData());
    // 30 ms windows overlapping by half, each free to move 10 ms either way:
    // short enough to follow speech, long enough to hold a pitch period.
    const int window = qMax(16, int(audio.sampleRate * 0.03)) & ~1;
    const int hop = window / 2;
    const int tolerance = qMax(1, int(audio.sampleRate * 0.01));
    const qint64 outFrames = qMax<qint64>(1, qint64(std::llround(inFrames / speed)));

    std::vector<float> taper(window);
    for (int i = 0; i < window; ++i)
        taper[i] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / window));
    std::vector<double> sum(outFrames + window, 0.0);
    std::vector<double> weight(outFrames + window, 0.0);
    auto sample = [&](qint64 index) -> float { return index >= 0 && index < inFrames ? in[index] : 0.0f; };

    qint64 previous = 0; // where the last window was taken from
    for (qint64 out = 0, k = 0; out < outFrames; out += hop, ++k) {
        const qint64 ideal = qint64(std::llround(k * hop * speed));
        qint64 chosen = ideal;
        if (k > 0) {
            // Pick the window whose start best matches how the previous one
            // would naturally have continued.
            const qint64 natural = previous + hop;
            double best = -1e300;
            for (qint64 offset = -tolerance; offset <= tolerance; offset += 2) {
                const qint64 candidate = ideal + offset;
                if (candidate < 0)
                    continue;
                double score = 0;
                for (int i = 0; i < hop; i += 2)
                    score += double(sample(natural + i)) * sample(candidate + i);
                if (score > best) {
                    best = score;
                    chosen = candidate;
                }
            }
        }
        for (int i = 0; i < window && out + i < qint64(sum.size()); ++i) {
            sum[out + i] += double(sample(chosen + i)) * taper[i];
            weight[out + i] += taper[i];
        }
        previous = chosen;
    }
    Audio result;
    result.sampleRate = audio.sampleRate;
    result.samples.resize(outFrames * qint64(sizeof(float)));
    auto *dst = reinterpret_cast<float *>(result.samples.data());
    for (qint64 i = 0; i < outFrames; ++i)
        dst[i] = weight[i] > 1e-6 ? float(sum[i] / weight[i]) : 0.0f;
    return result;
}

void applyGain(Audio *audio, double gain) {
    if (!audio || std::abs(gain - 1.0) < 1e-6)
        return;
    auto *samples = reinterpret_cast<float *>(audio->samples.data());
    const qint64 frames = audio->frames();
    for (qint64 i = 0; i < frames; ++i)
        samples[i] = float(qBound(-1.0, samples[i] * gain, 1.0));
}

}
