#include "narrator.h"

#include <QFile>
#include <QSettings>
#include <cmath>

Narrator::Narrator(QObject *parent) : QObject(parent) {
    QSettings settings;
    m_voice = settings.value("voice/name", "af_heart").toString();
    m_speed = qBound(0.5, settings.value("voice/speed", 1.0).toDouble(), 2.0);
    m_binaryOverride = settings.value("voice/koko").toString();
    m_modelOverride = settings.value("voice/models").toString();
    m_setup = KokoroSetup::find(m_binaryOverride, m_modelOverride);

    m_ticker.setInterval(25);
    connect(&m_ticker, &QTimer::timeout, this, &Narrator::tick);
    connect(&m_runner, &KokoroRunner::sentenceReady, this, &Narrator::onSentenceReady);
    connect(&m_runner, &KokoroRunner::sentenceFailed, this, [this](int id) { onSentenceFailed(id); });
    connect(&m_runner, &KokoroRunner::finished, this, &Narrator::onRunFinished);
    connect(&m_player, &AudioPlayer::errorOccurred, this, [this](const QString &message) {
        setError(message);
        halt();
    });
}

Narrator::~Narrator() { m_runner.stop(); }

void Narrator::setBook(BookPtr book) {
    halt();
    m_book = std::move(book);
    m_sentence = -1;
    m_word = -1;
    emit positionChanged();
    emit wordChanged();
}

QVariantMap Narrator::setupInfo() const { return m_setup.toJson().toVariantMap(); }

QVariantList Narrator::voices() const {
    QVariantList list;
    for (const Voice &v : Voices::all())
        list.append(QVariantMap{{"id", v.id}, {"name", v.name}, {"language", v.language}});
    return list;
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

void Narrator::setVoice(const QString &voice) {
    if (voice == m_voice || voice.isEmpty())
        return;
    m_voice = voice;
    QSettings().setValue("voice/name", voice);
    emit voiceChanged();
    if (active())
        play(m_sentence);
}

void Narrator::setSpeed(double speed) {
    speed = qBound(0.5, std::round(speed * 20) / 20, 2.0);
    if (qFuzzyCompare(speed, m_speed))
        return;
    m_speed = speed;
    QSettings().setValue("voice/speed", speed);
    emit speedChanged();
    if (active())
        play(m_sentence);
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
    m_setup = KokoroSetup::find(m_binaryOverride, m_modelOverride);
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

void Narrator::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}

void Narrator::setPosition(int sentence, int word) {
    const bool sentenceChanged = sentence != m_sentence;
    const bool wordChangedNow = sentenceChanged || word != m_word;
    m_sentence = sentence;
    m_word = word;
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
    m_runner.stop();
    m_player.close();
    m_ready.clear();
    m_failed.clear();
    m_timeline.clear();
    m_entry = 0;
    setState(Stopped);
}

void Narrator::play(int sentence) {
    if (!m_book || m_book->sentenceCount() == 0)
        return;
    sentence = qBound(0, sentence, m_book->sentenceCount() - 1);
    halt();
    setError({});
    refreshSetup();
    if (!m_setup.ready()) {
        setError(m_setup.problem());
        return;
    }
    if (!m_player.open(m_sampleRate))
        return; // errorOccurred already reported why

    while (sentence < m_book->sentenceCount() - 1 && !m_book->sentences.at(sentence).speakable())
        ++sentence;
    m_nextToAppend = sentence;
    setState(Preparing);
    setPosition(sentence, -1);
    m_ticker.start();
    startRun(sentence);
}

void Narrator::startRun(int from) {
    const int count = m_book->sentenceCount();
    m_runEnd = qMin(count, from + RunLength);
    QList<KokoroRunner::Line> lines;
    for (int i = from; i < m_runEnd; ++i)
        if (m_book->sentences.at(i).speakable())
            lines.append({i, m_book->sentenceText(i)});
    m_runner.start({m_setup, m_voice, m_speed}, lines);
}

void Narrator::onSentenceReady(int id, const QString &wav, const QString &tsv) {
    Ready ready;
    QString error;
    const bool ok = Speech::readWav(wav, &ready.audio, &error);
    QFile timestamps(tsv);
    const QByteArray tsvData = timestamps.open(QIODevice::ReadOnly) ? timestamps.readAll() : QByteArray();
    timestamps.close();
    QFile::remove(wav);
    QFile::remove(tsv);
    if (!ok || !m_book || id >= m_book->sentenceCount()) {
        onSentenceFailed(id);
        return;
    }
    if (ready.audio.sampleRate != m_sampleRate) {
        setError(tr("Kokoros produced %1 Hz audio; omareader expects %2 Hz.").arg(ready.audio.sampleRate).arg(m_sampleRate));
        onSentenceFailed(id);
        return;
    }
    ready.timings = Speech::align(m_book->sentenceWords(id), Speech::parseTimestamps(tsvData), ready.audio.seconds());
    m_ready.insert(id, ready);
    appendReady();
}

void Narrator::onSentenceFailed(int id) {
    m_failed.insert(id);
    appendReady();
}

void Narrator::appendReady() {
    if (!m_book)
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
    // Anything in the run Kokoros never reported on is skipped rather than
    // waited for forever.
    for (int i = m_nextToAppend; i < m_runEnd; ++i)
        if (!m_ready.contains(i))
            m_failed.insert(i);
    appendReady();
    if (m_runEnd < m_book->sentenceCount())
        startRun(m_runEnd);
}

void Narrator::updateHold() {
    const int ahead = int(m_timeline.size()) - m_entry - 1 + int(m_ready.size());
    if (!m_runner.isHeld() && ahead > HoldAhead)
        m_runner.setHeld(true);
    else if (m_runner.isHeld() && ahead < ReleaseAhead)
        m_runner.setHeld(false);
}

void Narrator::tick() {
    if (m_state != Playing || m_timeline.isEmpty())
        return;
    const qint64 frame = m_player.playedFrames();
    while (m_entry + 1 < m_timeline.size() && frame >= m_timeline.at(m_entry + 1).startFrame)
        ++m_entry;
    const Entry &entry = m_timeline.at(m_entry);

    if (frame >= entry.startFrame + entry.frames && m_entry + 1 >= m_timeline.size()) {
        // Heard everything queued: either the book is over or Kokoros is behind.
        if (m_nextToAppend >= m_book->sentenceCount() && !m_runner.isRunning()) {
            halt();
            emit finishedBook();
        }
        return;
    }

    const double seconds = double(frame - entry.startFrame) / m_sampleRate;
    int word = -1;
    for (int i = 0; i < entry.timings.size(); ++i) {
        if (entry.timings.at(i).start <= seconds)
            word = i;
        else
            break;
    }
    setPosition(entry.sentence, word);
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
    m_player.resume();
    setState(m_timeline.isEmpty() ? Preparing : Playing);
}

void Narrator::stop() { halt(); }

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
