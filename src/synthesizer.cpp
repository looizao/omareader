#include "synthesizer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
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
    const bool lastInParagraph = sentence + 1 >= book.sentenceCount()
                                 || book.sentences.at(sentence + 1).paragraph != current.paragraph
                                 || book.sentences.at(sentence + 1).chapter != current.chapter;
    if (!lastInParagraph)
        return 0;
    const int level = book.chapters.at(current.chapter).paragraphs.at(current.paragraph).level;
    return level > 0 ? voice.number("headingPause") : voice.number("paragraphPause");
}

SynthesisOptions Synthesis::optionsFor(const VoiceSettings &voice) {
    QSettings settings;
    SynthesisOptions options;
    options.voice = voice;
    options.kokoro = KokoroSetup::find(settings.value("voice/koko").toString(), settings.value("voice/models").toString());
    options.qwen = QwenSetup::find(voice.engine == Engine::Qwen ? voice.text("model") : QString(),
                                   voice.engine == Engine::Qwen ? voice.text("device") : QString());
    return options;
}

std::unique_ptr<Synthesizer> Synthesizer::create(const QString &engine, QObject *parent) {
    if (engine == Engine::Qwen)
        return std::make_unique<QwenSynthesizer>(parent);
    return std::make_unique<KokoroSynthesizer>(parent);
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

// Compiled as part of this file, not on its own; see omareader.pro.
#include "moc_synthesizer.cpp"
