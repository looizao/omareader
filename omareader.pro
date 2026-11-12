QT += core gui gui-private qml quick quickcontrols2 dbus multimedia concurrent network

CONFIG += c++17 release

# makepkg's hardening flags (full RELRO, fortify, stack protection) when set.
QMAKE_CXXFLAGS += $$(CXXFLAGS)
QMAKE_LFLAGS += $$(LDFLAGS)
TARGET = omareader
TEMPLATE = app

# VERSION is the one place the version lives; bin/release bumps it. It goes
# into a generated header only main.cpp includes rather than onto every
# compile line, so a release rebuilds one file and the compiler cache in CI
# keeps the rest.
APP_VERSION = $$cat($$PWD/VERSION)
VERSION_HEADER = "$${LITERAL_HASH}pragma once"
VERSION_HEADER += "$${LITERAL_HASH}define OMAREADER_VERSION \"$$APP_VERSION\""
write_file($$OUT_PWD/version.h, VERSION_HEADER)
INCLUDEPATH += $$OUT_PWD

HEADERS += \
    src/article.h \
    src/audioplayer.h \
    src/book.h \
    src/bookloader.h \
    src/cli.h \
    src/exporter.h \
    src/filepicker.h \
    src/highlight.h \
    src/kokoro.h \
    src/library.h \
    src/narrator.h \
    src/portalfilepicker.h \
    src/qwen.h \
    src/reader.h \
    src/speech.h \
    src/synthesizer.h \
    src/theme.h \
    src/titleagent.h \
    src/translator.h \
    src/voicesettings.h \
    src/webimport.h

SOURCES += \
    src/main.cpp \
    src/article.cpp \
    src/audioplayer.cpp \
    src/book.cpp \
    src/bookloader.cpp \
    src/cli.cpp \
    src/exporter.cpp \
    src/highlight.cpp \
    src/kokoro.cpp \
    src/library.cpp \
    src/narrator.cpp \
    src/portalfilepicker.cpp \
    src/qwen.cpp \
    src/reader.cpp \
    src/speech.cpp \
    src/synthesizer.cpp \
    src/theme.cpp \
    src/titleagent.cpp \
    src/translator.cpp \
    src/voicesettings.cpp \
    src/webimport.cpp

# Each source file ends by including its header's moc output
# (#include "moc_reader.cpp"), which qmake then leaves out of the build as a
# file of its own. Compiled alone, every one of them would read the same Qt
# headers again for a few lines of code.

RESOURCES += src/resources.qrc
