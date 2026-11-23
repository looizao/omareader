#include "synthesizer.h"

#include "dialogue.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <climits>
#include <cmath>

namespace {
QString tr(const char *text) { return QCoreApplication::translate("Synthesizer", text); }

// Qwen clients' priorities in the shared worker.
constexpr int PreviewPriority = 3;
constexpr int ListeningPriority = 2;
constexpr int BackgroundPriority = 1;

QString cleanLine(const QString &text) { return QString(text).replace('\n', ' ').simplified(); }
}

double Synthesis::pauseAfter(const Book &book, int sentence, const VoiceSettings &voice) {
    if (sentence < 0 || sentence >= book.sentenceCount())
        return 0;
    const Sentence &current = book.sentences.at(sentence);
    // The pause belongs to the gap before the next sentence the voice
    // reads, so a footnote or a sentence of nothing but clutter in between
    // doesn't swallow the paragraph or chapter break it sits on.
    int next = sentence + 1;
    while (next < book.sentenceCount() && !book.sentences.at(next).speakable())
        ++next;
    // The end of the book ends its last chapter too, which leaves an
    // exported book's last file with the same tail as the others.
    if (next >= book.sentenceCount() || book.sentences.at(next).chapter != current.chapter)
        return voice.number("chapterPause");
    if (book.sentences.at(next).paragraph != current.paragraph) {
        const int level = book.chapters.at(current.chapter).paragraphs.at(current.paragraph).level;
        return level > 0 ? voice.number("headingPause") : voice.number("paragraphPause");
    }
    // A long sentence broken at a comma or a semicolon is still one
    // sentence, and a pause in the middle of it would sound like a stumble.
    return current.ends ? voice.number("sentencePause") : 0;
}

qint64 Synthesis::pauseFramesAfter(const Book &book, int sentence, const VoiceSettings &voice) {
    return qint64(std::llround(pauseAfter(book, sentence, voice) * Synthesizer::SampleRate));
}

SynthesisOptions Synthesis::optionsFor(const VoiceSettings &voice) {
    QSettings settings;
    SynthesisOptions options;
    options.voice = voice;
    options.kokoro = KokoroSetup::find(settings.value("voice/koko").toString(), settings.value("voice/models").toString());
    options.qwen = QwenSetup::find(voice.engine == Engine::Qwen ? voice.text("model") : QString(),
                                   voice.engine == Engine::Qwen ? voice.text("device") : QString());
    options.lexicon = Lexicon::load();
    return options;
}

std::unique_ptr<Synthesizer> Synthesizer::create(const QString &engine, QObject *parent) {
    if (engine == Engine::Qwen)
        return std::make_unique<QwenSynthesizer>(parent);
    return std::make_unique<KokoroSynthesizer>(parent);
}

std::unique_ptr<Synthesizer> Synthesizer::forVoice(const VoiceSettings &voice, QObject *parent) {
    if (voice.dialogue().isEmpty())
        return create(voice.engine, parent);
    return std::make_unique<DialogueSynthesizer>(voice.engine, parent);
}

QList<Synthesizer::Line> Synthesis::lines(const Book &book, const QList<int> &sentences,
                                          const SynthesisOptions &options) {
    QList<Synthesizer::Line> out;
    const bool dialogue = !options.voice.dialogue().isEmpty();
    // Found a chapter at a time, as a quotation may open paragraphs before
    // the sentences asked for.
    QHash<int, QList<QList<Dialogue::Range>>> chapters;
    for (const int index : sentences) {
        if (index < 0 || index >= book.sentenceCount())
            continue;
        Synthesizer::Line line{index, Pronounce::utterance(book, index, options.lexicon).text};
        if (dialogue) {
            const Sentence &sentence = book.sentences.at(index);
            auto found = chapters.find(sentence.chapter);
            if (found == chapters.end())
                found = chapters.insert(sentence.chapter, Dialogue::find(book.chapters.at(sentence.chapter).paragraphs));
            const QList<Dialogue::Part> parts = Dialogue::parts(sentence, found->value(sentence.paragraph));
            if (parts.size() > 1 || (parts.size() == 1 && parts.first().dialogue)) {
                for (const Dialogue::Part &part : parts) {
                    const Sentence piece = Dialogue::slice(sentence, part.start, part.end);
                    const QString text = Pronounce::utterance(book, piece, options.lexicon).text;
                    if (!text.isEmpty())
                        line.pieces.append({text, part.dialogue});
                }
            }
        }
        out.append(line);
    }
    return out;
}

QString Synthesizer::problem(const SynthesisOptions &options) {
    return options.voice.engine == Engine::Qwen ? options.qwen.problem() : options.kokoro.problem();
}

bool Synthesizer::deliver(int id, const QString &wav, const QString &tsv, double stretch, double gain) {
    SpokenSentence sentence;
    QString error;
    const bool ok = Speech::readWav(wav, &sentence.audio, &error);
    QFile timestamps(tsv);
    const QByteArray tsvData = timestamps.open(QIODevice::ReadOnly) ? timestamps.readAll() : QByteArray();
    timestamps.close();
    QFile::remove(wav);
    QFile::remove(tsv);
    if (!ok || sentence.audio.frames() == 0) {
        emit sentenceFailed(id, error.isEmpty() ? tr("The voice produced no audio.") : error);
        return false;
    }
    sentence.words = Speech::parseTimestamps(tsvData);
    if (sentence.audio.sampleRate != SampleRate)
        sentence.audio = Speech::resample(sentence.audio, SampleRate);
    if (std::abs(stretch - 1.0) > 1e-3) {
        sentence.audio = Speech::timeStretch(sentence.audio, stretch);
        for (SpokenWord &word : sentence.words) {
            word.start /= stretch;
            word.end /= stretch;
        }
    }
    Speech::applyGain(&sentence.audio, gain);
    emit sentenceReady(id, sentence);
    return true;
}

KokoroSynthesizer::KokoroSynthesizer(QObject *parent) : Synthesizer(parent) {
    connect(&m_runner, &KokoroRunner::sentenceReady, this,
            [this](int id, const QString &wav, const QString &tsv) { deliver(id, wav, tsv, 1.0, m_gain); });
    connect(&m_runner, &KokoroRunner::sentenceFailed, this, &Synthesizer::sentenceFailed);
    connect(&m_runner, &KokoroRunner::finished, this, &Synthesizer::finished);
}

QString KokoroSynthesizer::style(const VoiceSettings &voice) {
    const QString blend = voice.text("blend");
    if (blend.isEmpty() || blend == voice.voice || !EngineCatalog::hasVoice(Engine::Kokoro, blend))
        return voice.voice;
    // Kokoros weighs voices in tenths: af_heart.7+af_bella.3.
    const int second = qBound(1, int(std::lround(voice.number("blendAmount") / 10)), 9);
    return QString("%1.%2+%3.%4").arg(voice.voice).arg(10 - second).arg(blend).arg(second);
}

bool KokoroSynthesizer::start(const SynthesisOptions &options, const QList<Line> &lines) {
    m_gain = options.voice.number("volume") / 100.0;
    QList<KokoroRunner::Line> runnerLines;
    for (const Line &line : lines)
        runnerLines.append({line.id, line.text});
    KokoroRunner::Options runnerOptions;
    runnerOptions.setup = options.kokoro;
    runnerOptions.voice = style(options.voice);
    runnerOptions.speed = options.voice.speed;
    runnerOptions.background = options.background;
    runnerOptions.language = options.kokoroLanguage;
    return m_runner.start(runnerOptions, runnerLines);
}

QwenSynthesizer::QwenSynthesizer(QObject *parent, QwenWorker *worker)
    : Synthesizer(parent), m_worker(worker ? worker : QwenWorker::shared()) {}

QJsonObject QwenSynthesizer::voiceRequest(const VoiceSettings &voice) {
    QJsonObject generation{{"temperature", voice.number("temperature")},
                           {"top_p", voice.number("topP")},
                           {"repetition_penalty", voice.number("repetitionPenalty")}};
    QJsonObject request{{"speaker", voice.voice},
                        {"language", voice.language.isEmpty() ? QString("Auto") : voice.language},
                        {"generation", generation},
                        {"seed", 1000 + int(voice.number("take"))}};
    // The 0.6B model has no instruction input; sending one would be ignored
    // at best, so only the 1.7B model gets the style.
    if (voice.text("model") == "1.7b" && !voice.text("style").isEmpty())
        request.insert("instruct", voice.text("style"));
    return request;
}

void QwenSynthesizer::ensureClient(bool background, bool preview) {
    const int priority = preview ? PreviewPriority : background ? BackgroundPriority : ListeningPriority;
    if (m_client && m_priority == priority)
        return;
    m_client = std::make_unique<QwenClient>(priority, m_worker);
    m_priority = priority;
    connect(m_client.get(), &QwenClient::itemDone, this,
            [this](int id, bool ok, const QString &wav, const QString &tsv, const QString &error) {
                if (ok) {
                    deliver(id, wav, tsv, m_speed, m_gain);
                    return;
                }
                QFile::remove(wav);
                QFile::remove(tsv);
                emit sentenceFailed(id, error);
            });
    connect(m_client.get(), &QwenClient::drained, this, [this] {
        m_running = false;
        emit finished(true, {});
    });
    connect(m_client.get(), &QwenClient::failed, this, [this](const QString &error) {
        // A run that was stopped has nobody waiting to hear why it ended.
        if (!m_running)
            return;
        m_running = false;
        emit finished(false, error);
    });
}

bool QwenSynthesizer::start(const SynthesisOptions &options, const QList<Line> &lines) {
    stop();
    const QString problem = options.qwen.problem();
    if (!problem.isEmpty()) {
        emit finished(false, problem);
        return false;
    }
    ensureClient(options.background, options.preview);
    m_speed = VoiceSettings::clampSpeed(options.voice.speed);
    m_gain = options.voice.number("volume") / 100.0;
    // Audio lands in a private temporary directory that goes away with the run.
    m_directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/omareader-XXXXXX");
    if (!m_directory->isValid()) {
        emit finished(false, tr("Could not create a temporary directory for the voice's audio."));
        return false;
    }
    QList<QwenItem> items;
    for (const Line &line : lines) {
        const QString text = cleanLine(line.text);
        if (text.isEmpty())
            continue;
        const QString base = m_directory->filePath(QString("s_%1").arg(line.id));
        items.append({line.id, text, base + ".wav", base + ".tsv"});
    }
    if (items.isEmpty()) {
        emit finished(true, {});
        return true;
    }
    QwenWorker::Config config{options.qwen.python, options.qwen.model, options.qwen.aligner,
                              options.voice.text("device")};
    m_running = true;
    m_client->submit(config, voiceRequest(options.voice), items);
    return true;
}

void QwenSynthesizer::stop() {
    if (m_client)
        m_client->cancel();
    m_running = false;
    m_directory.reset();
}

void QwenSynthesizer::setHeld(bool held) {
    if (m_client && m_running)
        m_client->setHeld(held);
}

DialogueSynthesizer::DialogueSynthesizer(const QString &engine, QObject *parent)
    : Synthesizer(parent), m_engine(engine) {
    for (int voice = 0; voice < VoiceCount; ++voice) {
        m_voices[voice] = Synthesizer::create(engine);
        Synthesizer *synth = m_voices[voice].get();
        connect(synth, &Synthesizer::sentenceReady, this,
                [this, voice](int piece, const SpokenSentence &sentence) { onReady(voice, piece, sentence); });
        connect(synth, &Synthesizer::sentenceFailed, this,
                [this, voice](int piece, const QString &message) { onFailed(voice, piece, message); });
        connect(synth, &Synthesizer::finished, this,
                [this, voice](bool ok, const QString &error) { onFinished(voice, ok, error); });
    }
}

SynthesisOptions DialogueSynthesizer::dialogueOptions(const SynthesisOptions &narration) {
    SynthesisOptions options = narration;
    const QString voice = narration.voice.dialogue();
    if (voice.isEmpty())
        return options;
    options.voice.voice = voice;
    options.voice.dialogueVoice.clear();
    if (options.voice.engine == Engine::Kokoro) {
        // The blend shapes the narrator's voice; the characters get theirs plain.
        options.voice.tuning.insert("blend", QString());
        if (options.kokoroLanguage.isEmpty())
            options.kokoroLanguage = Voices::languageFor(narration.voice.voice);
    }
    return options;
}

namespace {
// Frames of near silence (below about -45 dB) at the start or the end.
qint64 silentFrames(const Audio &audio, bool atEnd) {
    const auto *samples = reinterpret_cast<const float *>(audio.samples.constData());
    const qint64 frames = audio.frames();
    qint64 quiet = 0;
    while (quiet < frames && std::abs(samples[atEnd ? frames - 1 - quiet : quiet]) < 0.006f)
        ++quiet;
    return quiet;
}
}

SpokenSentence DialogueSynthesizer::join(const QList<SpokenSentence> &pieces, bool spreadTimes) {
    SpokenSentence whole;
    whole.audio.sampleRate = SampleRate;
    double offset = 0;
    for (int i = 0; i < pieces.size(); ++i) {
        const Audio &audio = pieces.at(i).audio;
        const qint64 frames = audio.frames();
        qint64 from = 0, to = frames;
        if (i > 0)
            from = qMax<qint64>(0, silentFrames(audio, false) - qint64(KeptBefore * SampleRate));
        if (i + 1 < pieces.size())
            to = frames - qMax<qint64>(0, silentFrames(audio, true) - qint64(KeptAfter * SampleRate));
        if (to <= from) {
            // All silence: nothing of it is kept.
            from = to = 0;
        }
        whole.audio.samples += audio.samples.mid(from * qint64(sizeof(float)), (to - from) * qint64(sizeof(float)));
        const double length = double(to - from) / SampleRate;
        const double scale = frames > 0 ? double(to - from) / frames : 0;
        const double cut = double(from) / SampleRate;
        auto moved = [&](double time) {
            return offset + qBound(0.0, spreadTimes ? time * scale : time - cut, length);
        };
        for (SpokenWord word : pieces.at(i).words) {
            word.start = moved(word.start);
            word.end = moved(word.end);
            whole.words.append(word);
        }
        offset += length;
    }
    return whole;
}

bool DialogueSynthesizer::start(const SynthesisOptions &options, const QList<Line> &lines) {
    stop();
    const quint64 run = m_run;
    QList<Line> said[VoiceCount];
    int piece = 0;
    for (const Line &line : lines) {
        Pending pending{line.id, {}};
        QList<Piece> pieces = line.pieces;
        if (pieces.isEmpty())
            pieces.append({line.text, false});
        for (const Piece &p : pieces) {
            const int voice = p.dialogue ? Speech : Narration;
            said[voice].append({piece, p.text});
            m_waiting[voice].append(piece);
            pending.pieces.append(piece);
            ++piece;
        }
        m_lines.append(pending);
    }
    m_running = true;
    for (int voice = 0; voice < VoiceCount; ++voice)
        m_finished[voice] = said[voice].isEmpty();
    if (said[Narration].isEmpty() && said[Speech].isEmpty()) {
        m_running = false;
        emit finished(true, {});
        return true;
    }
    const SynthesisOptions voiced[VoiceCount] = {options, dialogueOptions(options)};
    for (int voice = 0; voice < VoiceCount; ++voice) {
        if (said[voice].isEmpty())
            continue;
        m_voices[voice]->start(voiced[voice], said[voice]);
        // Failing to start says so through finished, which may have
        // stopped or replaced this run already.
        if (run != m_run || !m_running)
            return false;
    }
    balance();
    return true;
}

void DialogueSynthesizer::stop() {
    ++m_run;
    for (auto &voice : m_voices)
        voice->stop();
    for (int voice = 0; voice < VoiceCount; ++voice) {
        m_waiting[voice].clear();
        m_finished[voice] = true;
    }
    m_lines.clear();
    m_next = 0;
    m_spoken.clear();
    m_failed.clear();
    m_running = false;
    // A hold belongs to the run it was asked for; the next run starts free,
    // as the engines' own runs do, or both voices could wait for good.
    m_held = false;
}

void DialogueSynthesizer::setHeld(bool held) {
    m_held = held;
    balance();
}

void DialogueSynthesizer::balance() {
    if (!m_running)
        return;
    const int next[VoiceCount] = {m_waiting[Narration].isEmpty() ? INT_MAX : m_waiting[Narration].first(),
                              m_waiting[Speech].isEmpty() ? INT_MAX : m_waiting[Speech].first()};
    for (int voice = 0; voice < VoiceCount; ++voice) {
        if (m_finished[voice])
            continue;
        const bool hold = m_held || next[1 - voice] < next[voice];
        if (hold != m_voices[voice]->isHeld())
            m_voices[voice]->setHeld(hold);
    }
}

void DialogueSynthesizer::onReady(int voice, int piece, const SpokenSentence &sentence) {
    if (!m_running || !m_waiting[voice].removeOne(piece))
        return;
    m_spoken.insert(piece, sentence);
    if (deliverDone())
        balance();
}

void DialogueSynthesizer::onFailed(int voice, int piece, const QString &message) {
    if (!m_running || !m_waiting[voice].removeOne(piece))
        return;
    // Kokoros may give no reason ("No audio produced for line 3").
    m_failed.insert(piece, message.isEmpty() ? tr("The voice produced no audio.") : message);
    if (deliverDone())
        balance();
}

void DialogueSynthesizer::onFinished(int voice, bool ok, const QString &error) {
    if (!m_running || m_finished[voice])
        return;
    if (!ok) {
        // One voice failing stops the other: half a book's lines can't play.
        const quint64 run = m_run;
        for (auto &other : m_voices)
            other->stop();
        m_running = false;
        if (run == m_run)
            emit finished(false, error);
        return;
    }
    m_finished[voice] = true;
    // Whatever the voice never reported on is skipped rather than waited for.
    for (const int piece : std::as_const(m_waiting[voice]))
        m_failed.insert(piece, tr("The voice skipped it."));
    m_waiting[voice].clear();
    const quint64 run = m_run;
    if (!deliverDone() || run != m_run)
        return;
    if (m_finished[Narration] && m_finished[Speech]) {
        m_running = false;
        emit finished(true, {});
        return;
    }
    balance();
}

bool DialogueSynthesizer::deliverDone() {
    const quint64 run = m_run;
    while (m_next < m_lines.size()) {
        const Pending &line = m_lines.at(m_next);
        QString failure;
        bool failed = false, waiting = false;
        for (const int piece : line.pieces) {
            if (m_failed.contains(piece)) {
                if (!failed)
                    failure = m_failed.value(piece);
                failed = true;
            } else if (!m_spoken.contains(piece)) {
                waiting = true;
            }
        }
        // A line that failed still waits for its other pieces, so none of
        // them arrives after it is gone.
        if (waiting)
            return true;
        const int id = line.id;
        const QList<int> pieces = line.pieces;
        ++m_next;
        if (failed) {
            for (const int piece : pieces) {
                m_spoken.remove(piece);
                m_failed.remove(piece);
            }
            emit sentenceFailed(id, failure);
        } else {
            QList<SpokenSentence> parts;
            for (const int piece : pieces)
                parts.append(m_spoken.take(piece));
            emit sentenceReady(id, join(parts, m_engine == Engine::Kokoro));
        }
        if (run != m_run || !m_running)
            return false;
    }
    return true;
}

// Compiled as part of this file, not on its own; see omareader.pro.
#include "moc_synthesizer.cpp"
