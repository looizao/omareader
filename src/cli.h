#pragma once

#include <QStringList>

// Commands that need no display, so agents and scripts can use omareader:
// info, text, doctor, setup, skill. Each takes --json where it returns data
// and exits 0 or 1 with errors on stderr.
namespace Cli {
bool isCommand(const QString &word);
int run(const QStringList &arguments);
QString usage();
}
