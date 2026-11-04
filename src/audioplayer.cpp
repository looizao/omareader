#include "audioplayer.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QMediaDevices>
#include <cmath>

AudioPlayer::AudioPlayer(QObject *parent) : QObject(parent) {
    m_pump.setInterval(15);
    connect(&m_pump, &QTimer::timeout, this, &AudioPlayer::pump);
}

AudioPlayer::~AudioPlayer() { close(); }

bool AudioPlayer::open(int sampleRate) {
    if (m_sink && sampleRate == m_sampleRate)
        return true;
    close();
    m_error.clear();

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        m_error = tr("There is no audio output device to read aloud through.");
        emit errorOccurred(m_error);
        return false;
    }
    m_format = QAudioFormat();
    m_format.setSampleRate(sampleRate);
    m_format.setChannelCount(1);
    m_format.setSampleFormat(QAudioFormat::Float);
    if (!device.isFormatSupported(m_format))
        m_format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(m_format)) {
        m_error = tr("The audio output %1 can't play %2 Hz mono audio.").arg(device.description()).arg(sampleRate);
        emit errorOccurred(m_error);
        return false;
    }
    m_sampleRate = sampleRate;
    m_sink = std::make_unique<QAudioSink>(device, m_format);
    // A modest buffer keeps the gap between "written" and "heard" small, so
    // the highlight stays with the voice.
    m_sink->setBufferSize(m_format.bytesForDuration(150000));
    m_device = m_sink->start();
    if (!m_device) {
        m_error = tr("The audio output could not be started.");
        m_sink.reset();
        emit errorOccurred(m_error);
        return false;
    }
    m_paused = false;
    m_pump.start();
    return true;
}

void AudioPlayer::close() {
    m_pump.stop();
    if (m_sink) {
        m_sink->stop();
        m_sink.reset();
    }
    m_device = nullptr;
    m_pending.clear();
    m_appendedFrames = 0;
    m_writtenBytes = 0;
    m_sampleRate = 0;
}

void AudioPlayer::append(const QByteArray &samples) {
    if (!m_sink)
        return;
    const qsizetype frames = samples.size() / qsizetype(sizeof(float));
    m_appendedFrames += frames;
    if (m_format.sampleFormat() == QAudioFormat::Float) {
        m_pending += samples;
    } else {
        const float *in = reinterpret_cast<const float *>(samples.constData());
        QByteArray converted(frames * qsizetype(sizeof(qint16)), Qt::Uninitialized);
        auto *out = reinterpret_cast<qint16 *>(converted.data());
        for (qsizetype i = 0; i < frames; ++i)
            out[i] = qint16(std::lround(qBound(-1.0f, in[i], 1.0f) * 32767.0f));
        m_pending += converted;
    }
    pump();
}

void AudioPlayer::pump() {
    if (!m_sink || !m_device || m_paused || m_pending.isEmpty())
        return;
    const qsizetype free = m_sink->bytesFree();
    if (free <= 0)
        return;
    const int frameBytes = m_format.bytesPerFrame();
    const qsizetype chunk = qMin(free, m_pending.size()) / frameBytes * frameBytes;
    if (chunk <= 0)
        return;
    const qint64 written = m_device->write(m_pending.constData(), chunk);
    if (written > 0) {
        m_pending.remove(0, written);
        m_writtenBytes += written;
    }
}

qint64 AudioPlayer::playedFrames() const {
    if (!m_sink)
        return 0;
    // What the sink has consumed, never ahead of what was written to it.
    const qint64 processed = m_format.framesForDuration(m_sink->processedUSecs());
    const qint64 written = m_writtenBytes / qMax(1, m_format.bytesPerFrame());
    return qBound<qint64>(0, processed, written);
}

void AudioPlayer::pause() {
    if (!m_sink || m_paused)
        return;
    m_paused = true;
    m_sink->suspend();
}

void AudioPlayer::resume() {
    if (!m_sink || !m_paused)
        return;
    m_paused = false;
    m_sink->resume();
    pump();
}
