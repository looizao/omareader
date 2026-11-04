QT += core gui gui-private qml quick quickcontrols2 dbus multimedia concurrent

CONFIG += c++17 release

# makepkg's hardening flags (full RELRO, fortify, stack protection) when set.
QMAKE_CXXFLAGS += $$(CXXFLAGS)
QMAKE_LFLAGS += $$(LDFLAGS)
TARGET = omareader
TEMPLATE = app

# VERSION is the one place the version lives; bin/release bumps it.
APP_VERSION = $$cat($$PWD/VERSION)
DEFINES += OMAREADER_VERSION=\\\"$$APP_VERSION\\\"

HEADERS += \
    src/audioplayer.h \
    src/book.h \
    src/bookloader.h \
    src/cli.h \
    src/filepicker.h \
    src/highlight.h \
    src/kokoro.h \
    src/library.h \
    src/narrator.h \
    src/portalfilepicker.h \
    src/reader.h \
    src/speech.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/audioplayer.cpp \
    src/book.cpp \
    src/bookloader.cpp \
    src/cli.cpp \
    src/highlight.cpp \
    src/kokoro.cpp \
    src/library.cpp \
    src/narrator.cpp \
    src/portalfilepicker.cpp \
    src/reader.cpp \
    src/speech.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
