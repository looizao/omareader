#include "narrator.h"

#include <QSettings>
#include <cmath>

Narrator::Narrator(QObject *parent) : QObject(parent) {
    QSettings settings;
    m_settings = VoiceSettings::loadSelected();
    m_binaryOverride = settings.value("voice/koko").toString();
    m_modelOverride = settings.value("voice/models").toString();
    m_kokoroSetup = KokoroSetup::find(m_binaryOverride, m_modelOverride);
    const VoiceSettings qwen = m_settings.engine == Engine::Qwen ? m_settings : VoiceSettings::load(Engine::Qwen);
    m_qwenSetup = QwenSetup::find(qwen.text("model"), qwen.text("device"));

    m_ticker.setInterval(25);
    connect(&m_ticker, &QTimer::timeout, this, &Narrator::tick);
    connect(&m_player, &AudioPlayer::errorOccurred, this, [this](const QString &message) {
        setError(message);
        halt();
    });
    // A slider dragged across several values rebuilds the audio once, when
    // it comes to rest, rather than once per step.
    m_restartTimer.setSingleShot(true);
    m_restartTimer.setInterval(RestartDelay);
    connect(&m_restartTimer, &QTimer::timeout, this, &Narrator::restart);
    m_previewTicker.setInterval(50);
    connect(&m_previewTicker, &QTimer::timeout, this, [this] {
        if (m_previewPlayer.queuedFrames() > 0 && m_previewPlayer.playedFrames() >= m_previewPlayer.queuedFrames())
            stopPreview();
    });
    connect(&m_previewPlayer, &AudioPlayer::errorOccurred, this, [this](const QString &message) {
        setError(message);
        stopPreview();
    });
}

Narrator::~Narrator() {
    if (m_synth)
        m_synth->stop();
    if (m_previewSynth)
        m_previewSynth->stop();
}

void Narrator::setBook(BookPtr book) {
    halt();
    m_book = std::move(book);
    m_sentence = -1;
    m_word = -1;
    emit positionChanged();
    emit wordChanged();
}

QString Narrator::problem() const {
    return m_settings.engine == Engine::Qwen ? m_qwenSetup.problem() : m_kokoroSetup.problem();
}

QVariantMap Narrator::setupInfo() const {
    QVariantMap info = m_settings.engine == Engine::Qwen ? m_qwenSetup.toJson().toVariantMap()
                                                         : m_kokoroSetup.toJson().toVariantMap();
    info.insert("engine", m_settings.engine);
    // One line per file for the panel: where the engine was found.
    info.insert("paths", m_settings.engine == Engine::Qwen
                             ? QStringList{m_qwenSetup.python, m_qwenSetup.model, m_qwenSetup.aligner}.join('\n')
                             : QStringList{m_kokoroSetup.binary, m_kokoroSetup.model}.join('\n'));
    return info;
}

QString Narrator::hardwareAdvice() const {
    if (m_settings.engine != Engine::Qwen)
        return {};
    if (m_settings.text("device") == "cpu")
        return tr("Qwen3-TTS is set to run on the CPU, at about a third of real time: fine for an export left "
                  "running, too slow to listen along.");
    if (!m_hardware)
        m_hardware = QwenHardware::probe();
    return m_hardware->advice(m_settings.text("model"));
}

SynthesisOptions Narrator::synthesisOptions() const {
    SynthesisOptions options;
    options.voice = m_settings;
    options.kokoro = m_kokoroSetup;
    options.qwen = m_qwenSetup;
    return options;
}

QVariantList Narrator::engines() const {
    QVariantList list;
    for (const QString &engine : Engine::all()) {
        const QString problem = engine == Engine::Qwen ? m_qwenSetup.problem() : m_kokoroSetup.problem();
        list.append(QVariantMap{{"id", engine},
                                {"name", Engine::displayName(engine)},
                                {"available", problem.isEmpty()},
                                {"problem", problem}});
    }
    return list;
}

QVariantList Narrator::voices() const {
    QVariantList list;
    for (const EngineVoice &v : EngineCatalog::voices(m_settings.engine))
        list.append(QVariantMap{{"id", v.id}, {"name", v.name}, {"language", v.language}, {"description", v.description}});
    return list;
}

QVariantList Narrator::languages() const {
    QVariantList list;
    for (const EngineLanguage &l : EngineCatalog::languages(m_settings.engine))
        list.append(QVariantMap{{"id", l.id}, {"name", l.name}});
    return list;
}

QVariantList Narrator::tuning() const {
    QVariantList list;
    for (const TuningParameter &p : Tuning::parameters(m_settings.engine)) {
        bool visible = true;
        if (!p.dependsOn.isEmpty()) {
            // "model=1.7b" or "blend!=": shown only when another setting allows it.
            const bool negate = p.dependsOn.contains("!=");
            const QStringList parts = p.dependsOn.split(negate ? "!=" : "=");
            const bool equal = m_settings.text(parts.value(0)) == parts.value(1);
            visible = negate ? !equal : equal;
        }
        QVariantList choices;
        for (int i = 0; i < p.choices.size(); ++i)
            choices.append(QVariantMap{{"value", p.choices.at(i)}, {"label", p.choiceLabels.value(i)}});
        static const char *kinds[] = {"number", "text", "choice"};
        list.append(QVariantMap{{"key", p.key},
                                {"kind", kinds[p.kind]},
                                {"label", p.label},
                                {"description", p.description},
                                {"minimum", p.minimum},
                                {"maximum", p.maximum},
                                {"step", p.step},
                                {"unit", p.unit},
                                {"recommended", p.recommended},
                                {"value", m_settings.tuning.value(p.key, p.recommended)},
                                {"choices", choices},
                                {"visible", visible}});
    }
    return list;
}

bool Narrator::tuned() const {
    const VoiceSettings recommended = VoiceSettings::recommended(m_settings.engine);
    if (!qFuzzyCompare(recommended.speed, m_settings.speed))
        return true;
    for (auto it = recommended.tuning.cbegin(); it != recommended.tuning.cend(); ++it)
        if (m_settings.tuning.value(it.key()) != it.value())
            return true;
    return false;
}

int Narrator::chapter() const {
    return m_book && m_sentence >= 0 && m_sentence < m_book->sentenceCount() ? m_book->sentences.at(m_sentence).chapter : -1;
}
int Narrator::paragraph() const {
    return m_book && m_sentence >= 0 && m_sentence < m_book->sentenceCount() ? m_book->sentences.at(m_sentence).paragraph : -1;
}
int Narrator::sentenceStart() const {
    return active() && m_book && m_sentence >= 0 && m_sentence < m_book->sentenceCount() ? m_book->sentences.at(m_sentence).start : -1;
}
int Narrator::sentenceEnd() const {
    return active() && m_book && m_sentence >= 0 && m_sentence < m_book->sentenceCount() ? m_book->sentences.at(m_sentence).end : -1;
}
int Narrator::wordStart() const {
    if (!active() || !m_book || m_sentence < 0 || m_sentence >= m_book->sentenceCount())
        return -1;
    const auto &words = m_book->sentences.at(m_sentence).words;
    return m_word >= 0 && m_word < words.size() ? words.at(m_word).start : -1;
}
int Narrator::wordEnd() const {
    if (!active() || !m_book || m_sentence < 0 || m_sentence >= m_book->sentenceCount())
        return -1;
    const auto &words = m_book->sentences.at(m_sentence).words;
    return m_word >= 0 && m_word < words.size() ? words.at(m_word).start + words.at(m_word).length : -1;
}

// The next word: later in the sentence, or the first of the next sentence
// when that is in the same paragraph.
static const WordSpan *nextWord(const BookPtr &book, int sentence, int word) {
    if (!book || sentence < 0 || sentence >= book->sentenceCount() || word < 0)
        return nullptr;
    const Sentence &current = book->sentences.at(sentence);
    if (word + 1 < current.words.size())
        return &current.words.at(word + 1);
    if (sentence + 1 >= book->sentenceCount())
        return nullptr;
    const Sentence &following = book->sentences.at(sentence + 1);
    if (following.chapter != current.chapter || following.paragraph != current.paragraph || following.words.isEmpty())
        return nullptr;
    return &following.words.first();
}

int Narrator::nextWordStart() const {
    const WordSpan *next = active() ? nextWord(m_book, m_sentence, m_word) : nullptr;
    return next ? next->start : -1;
}
int Narrator::nextWordEnd() const {
    const WordSpan *next = active() ? nextWord(m_book, m_sentence, m_word) : nullptr;
    return next ? next->start + next->length : -1;
}

void Narrator::setEngine(const QString &engine) {
    if (engine == m_settings.engine || !Engine::isKnown(engine))
        return;
    stopPreview();
    const bool wasActive = active();
    const bool wasPaused = m_state == Paused;
    const int sentence = m_sentence;
    halt();
    // The engine the user picks is the one used, ready or not: a missing
    // engine says so rather than reading on with the other one.
    VoiceSettings::setSelectedEngine(engine);
    m_settings = VoiceSettings::load(engine);
    m_synth.reset();
    m_hardware.reset();
    emit engineChanged();
    emit voiceChanged();
    emit speedChanged();
    emit tuningChanged();
    refreshSetup();
    emit setupChanged();
    if (wasActive) {
        play(sentence);
        if (wasPaused)
            pause();
    }
}

void Narrator::setVoice(const QString &voice) {
    if (voice == m_settings.voice || !EngineCatalog::hasVoice(m_settings.engine, voice))
        return;
    m_settings.voice = voice;
    m_settings.save();
    emit voiceChanged();
    settingsChanged();
}

void Narrator::setLanguage(const QString &language) {
    if (language == m_settings.language || !EngineCatalog::hasLanguage(m_settings.engine, language))
        return;
    m_settings.language = language;
    m_settings.save();
    emit voiceChanged();
    settingsChanged();
}

void Narrator::setSpeed(double speed) {
    speed = VoiceSettings::clampSpeed(speed);
    if (qFuzzyCompare(speed, m_settings.speed))
        return;
    m_settings.speed = speed;
    m_settings.save();
    emit speedChanged();
    emit tuningChanged(); // the reset button looks at the speed too
    settingsChanged();
}

void Narrator::setTuning(const QString &key, const QVariant &value) {
    VoiceSettings next = m_settings;
    if (!next.set(key, value) || next.tuning == m_settings.tuning)
        return;
    m_settings = next;
    m_settings.save();
    emit tuningChanged();
    if (key == "model" || key == "device") {
        m_hardware.reset();
        refreshSetup();
        emit setupChanged();
    }
    settingsChanged();
}

void Narrator::resetTuning() {
    VoiceSettings::reset(m_settings.engine);
    const VoiceSettings loaded = VoiceSettings::load(m_settings.engine);
    const bool setupChanges = loaded.text("model") != m_settings.text("model")
                              || loaded.text("device") != m_settings.text("device");
    const bool speedChanges = !qFuzzyCompare(loaded.speed, m_settings.speed);
    m_settings = loaded;
    if (speedChanges)
        emit speedChanged();
    emit tuningChanged();
    if (setupChanges) {
        m_hardware.reset();
        refreshSetup();
        emit setupChanged();
    }
    settingsChanged();
}

void Narrator::settingsChanged() {
    if (active())
        m_restartTimer.start();
}

void Narrator::setOverrides(const QString &binary, const QString &modelDirectory) {
    m_binaryOverride = binary;
    m_modelOverride = modelDirectory;
    QSettings settings;
    settings.setValue("voice/koko", binary);
    settings.setValue("voice/models", modelDirectory);
    refreshSetup();
}

void Narrator::refreshSetup() {
    const QVariantMap before = setupInfo();
    const QVariantList enginesBefore = engines();
    m_kokoroSetup = KokoroSetup::find(m_binaryOverride, m_modelOverride);
    const VoiceSettings qwen = m_settings.engine == Engine::Qwen ? m_settings : VoiceSettings::load(Engine::Qwen);
    m_qwenSetup = QwenSetup::find(qwen.text("model"), qwen.text("device"));
    // Only a real change is announced: the panel's bindings re-run on it.
    if (setupInfo() != before || engines() != enginesBefore)
        emit setupChanged();
}

void Narrator::clearError() { setError({}); }

void Narrator::setState(State state) {
    if (state == m_state)
        return;
    m_state = state;
    emit stateChanged();
    // The highlight ranges depend on being active.
    emit positionChanged();
    emit wordChanged();
}

void Narrator::showForTests(State state, int sentence, int word, double wordSeconds) {
    setPosition(sentence, word, wordSeconds);
    setState(state);
}

void Narrator::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}

void Narrator::setPosition(int sentence, int word, double wordSeconds) {
    const bool sentenceChanged = sentence != m_sentence;
    const bool wordChangedNow = sentenceChanged || word != m_word || wordSeconds != m_wordSeconds;
    m_sentence = sentence;
    m_word = word;
    m_wordSeconds = wordSeconds;
    if (sentenceChanged)
        emit positionChanged();
    if (wordChangedNow)
        emit wordChanged();
}

bool Narrator::skippable(int sentence) const {
    return !m_book->sentences.at(sentence).speakable() || m_failed.contains(sentence);
}

void Narrator::halt() {
    m_ticker.stop();
    m_restartTimer.stop();
    if (m_synth)
        m_synth->stop();
    m_player.close();
    m_ready.clear();
    m_failed.clear();
    m_timeline.clear();
    m_entry = 0;
    setState(Stopped);
}

void Narrator::restart() {
    if (!active() || !m_book || m_sentence < 0)
        return;
    const bool wasPaused = m_state == Paused;
    play(m_sentence);
    if (wasPaused)
        pause();
}

void Narrator::ensureSynthesizer() {
    if (m_synth && m_synth->engine() == m_settings.engine)
        return;
    m_synth = Synthesizer::create(m_settings.engine);
    connect(m_synth.get(), &Synthesizer::sentenceReady, this, &Narrator::onSentenceReady);
    connect(m_synth.get(), &Synthesizer::sentenceFailed, this, [this](int id) { onSentenceFailed(id); });
    connect(m_synth.get(), &Synthesizer::finished, this, &Narrator::onRunFinished);
}

void Narrator::play(int sentence) {
    if (!m_book || m_book->sentenceCount() == 0)
        return;
    sentence = qBound(0, sentence, m_book->sentenceCount() - 1);
    stopPreview();
    halt();
    setError({});
    refreshSetup();
    if (!available()) {
        setError(problem());
        return;
    }
    if (!m_player.open(Synthesizer::SampleRate))
        return; // errorOccurred already reported why

    while (sentence < m_book->sentenceCount() - 1 && !m_book->sentences.at(sentence).speakable())
        ++sentence;
    m_nextToAppend = sentence;
    setState(Preparing);
    setPosition(sentence, -1);
    m_ticker.start();
    ensureSynthesizer();
    startRun(sentence);
}

void Narrator::startRun(int from) {
    const int count = m_book->sentenceCount();
    m_runEnd = qMin(count, from + RunLength);
    QList<Synthesizer::Line> lines;
    for (int i = from; i < m_runEnd; ++i)
        if (m_book->sentences.at(i).speakable())
            lines.append({i, m_book->sentenceText(i)});
    m_synth->start(synthesisOptions(), lines);
}

void Narrator::onSentenceReady(int id, const SpokenSentence &sentence) {
    if (!m_book || id < 0 || id >= m_book->sentenceCount() || m_state == Stopped) {
        onSentenceFailed(id);
        return;
    }
    Ready ready;
    ready.audio = sentence.audio;
    ready.timings = Speech::align(m_book->sentenceWords(id), sentence.words, ready.audio.seconds());
    m_ready.insert(id, ready);
    appendReady();
}

void Narrator::onSentenceFailed(int id) {
    m_failed.insert(id);
    appendReady();
}

double Narrator::pauseAfter(int sentence) const {
    return m_book ? Synthesis::pauseAfter(*m_book, sentence, m_settings) : 0;
}

void Narrator::appendReady() {
    if (!m_book || m_state == Stopped)
        return;
    const int count = m_book->sentenceCount();
    while (m_nextToAppend < count) {
        if (skippable(m_nextToAppend)) {
            ++m_nextToAppend;
            continue;
        }
        auto it = m_ready.find(m_nextToAppend);
        if (it == m_ready.end())
            break;
        const qint64 start = m_player.queuedFrames();
        m_timeline.append({m_nextToAppend, start, it->audio.frames(), it->timings});
        m_player.append(it->audio.samples);
        // The pause belongs to the stream, not to the sentence: the word
        // highlight stays on the last word through it.
        const qint64 silence = qint64(std::llround(pauseAfter(m_nextToAppend) * Synthesizer::SampleRate));
        if (silence > 0)
            m_player.append(QByteArray(silence * qint64(sizeof(float)), '\0'));
        m_ready.erase(it);
        ++m_nextToAppend;
    }
    if (m_state == Preparing && !m_timeline.isEmpty())
        setState(Playing);
    updateHold();
}

void Narrator::onRunFinished(bool ok, const QString &error) {
    if (!m_book || m_state == Stopped)
        return;
    if (!ok) {
        setError(error);
        if (m_timeline.isEmpty())
            halt();
        return;
    }
    // Anything in the run the engine never reported on is skipped rather
    // than waited for forever.
    for (int i = m_nextToAppend; i < m_runEnd; ++i)
        if (!m_ready.contains(i))
            m_failed.insert(i);
    appendReady();
    if (m_runEnd < m_book->sentenceCount())
        startRun(m_runEnd);
}

void Narrator::updateHold() {
    if (!m_synth)
        return;
    const int ahead = int(m_timeline.size()) - m_entry - 1 + int(m_ready.size());
    if (!m_synth->isHeld() && ahead > HoldAhead)
        m_synth->setHeld(true);
    else if (m_synth->isHeld() && ahead < ReleaseAhead)
        m_synth->setHeld(false);
}

void Narrator::tick() {
    if (m_state != Playing || m_timeline.isEmpty())
        return;
    const qint64 frame = m_player.playedFrames();
    while (m_entry + 1 < m_timeline.size() && frame >= m_timeline.at(m_entry + 1).startFrame)
        ++m_entry;
    const Entry &entry = m_timeline.at(m_entry);

    if (frame >= m_player.queuedFrames() && m_entry + 1 >= m_timeline.size()) {
        // Heard everything queued: either the book is over or the engine is behind.
        if (m_nextToAppend >= m_book->sentenceCount() && !(m_synth && m_synth->isRunning())) {
            halt();
            emit finishedBook();
        }
        return;
    }

    const double seconds = double(frame - entry.startFrame) / Synthesizer::SampleRate;
    int word = -1;
    for (int i = 0; i < entry.timings.size(); ++i) {
        if (entry.timings.at(i).start <= seconds)
            word = i;
        else
            break;
    }
    // Until the next word starts, or the next sentence for the last word,
    // pause included.
    double wordSeconds = 0;
    if (word >= 0) {
        const double next = word + 1 < entry.timings.size() ? entry.timings.at(word + 1).start
                                                             : double(entry.frames) / Synthesizer::SampleRate;
        wordSeconds = qMax(0.0, next - entry.timings.at(word).start);
    }
    setPosition(entry.sentence, word, wordSeconds);
    updateHold();
}

void Narrator::pause() {
    if (m_state != Playing && m_state != Preparing)
        return;
    m_player.pause();
    setState(Paused);
}

void Narrator::resume() {
    if (m_state != Paused)
        return;
    stopPreview();
    m_player.resume();
    setState(m_timeline.isEmpty() ? Preparing : Playing);
}

void Narrator::stop() {
    stopPreview();
    halt();
}

void Narrator::toggle(int fromSentence) {
    switch (m_state) {
    case Stopped: play(fromSentence); break;
    case Paused: resume(); break;
    default: pause(); break;
    }
}

void Narrator::skip(int sentences) {
    if (!m_book || m_sentence < 0)
        return;
    int target = m_sentence;
    const int step = sentences < 0 ? -1 : 1;
    for (int moved = 0; moved < std::abs(sentences);) {
        target += step;
        if (target < 0 || target >= m_book->sentenceCount())
            return;
        if (m_book->sentences.at(target).speakable())
            ++moved;
    }
    const bool wasPaused = m_state == Paused;
    play(target);
    if (wasPaused)
        pause();
}

void Narrator::setPreviewing(bool previewing) {
    if (previewing == m_previewing)
        return;
    m_previewing = previewing;
    emit previewChanged();
}

void Narrator::preview() {
    stopPreview();
    setError({});
    refreshSetup();
    if (!available()) {
        setError(problem());
        return;
    }
    if (m_state == Playing || m_state == Preparing)
        pause();
    m_previewSynth = Synthesizer::create(m_settings.engine);
    connect(m_previewSynth.get(), &Synthesizer::sentenceReady, this, [this](int, const SpokenSentence &sentence) {
        if (!m_previewPlayer.isOpen() && !m_previewPlayer.open(Synthesizer::SampleRate))
            return; // errorOccurred already reported why
        m_previewPlayer.append(sentence.audio.samples);
        m_previewTicker.start();
    });
    connect(m_previewSynth.get(), &Synthesizer::sentenceFailed, this, [this](int, const QString &message) {
        setError(tr("The preview could not be spoken: %1").arg(message));
    });
    connect(m_previewSynth.get(), &Synthesizer::finished, this, [this](bool ok, const QString &error) {
        if (!ok)
            setError(error);
        if (!ok || m_previewPlayer.queuedFrames() == 0)
            stopPreview();
    });
    SynthesisOptions options = synthesisOptions();
    options.preview = true;
    const QString text = EngineCatalog::previewText(
        EngineCatalog::previewLanguage(m_settings.engine, m_settings.voice, m_settings.language));
    setPreviewing(true);
    m_previewSynth->start(options, {{0, text}});
}

void Narrator::stopPreview() {
    m_previewTicker.stop();
    if (m_previewSynth) {
        m_previewSynth->stop();
        // It may be the one calling: let it finish its signal first.
        m_previewSynth.release()->deleteLater();
    }
    m_previewPlayer.close();
    setPreviewing(false);
}

// Compiled as part of this file, not on its own; see omareader.pro.
#include "moc_narrator.cpp"
