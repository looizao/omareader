#pragma once

#include <QByteArray>
#include <QString>

namespace Fixtures {
void writeEpub(const QString &path, const QByteArray &coverPng);
void writeFb2(const QString &path);
void writeDocx(const QString &path);
void writeOdt(const QString &path);
}
