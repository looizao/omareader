#pragma once

#include <QByteArray>
#include <QString>

namespace Fixtures {
void writeEpub(const QString &path, const QByteArray &coverPng);
void writeFb2(const QString &path);
void writeDocx(const QString &path);
void writeOdt(const QString &path);
// A stand-in for qwen-worker.py that speaks the same JSON-lines protocol
// without any model: each sentence becomes half a second of 12 kHz audio per
// word, every word timed. It logs its arguments and requests to
// FAKE_QWEN_LOG, waits FAKE_QWEN_DELAY seconds per request, fails sentences
// reading FAIL, and stops with a fatal error when the model path contains
// "broken".
QByteArray fakeQwenWorker();
}
