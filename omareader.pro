QT += core gui gui-private qml quick quickcontrols2 dbus multimedia concurrent network

CONFIG += c++17 release

# makepkg's hardening flags (full RELRO, fortify, stack protection) when set.
QMAKE_CXXFLAGS += $$(CXXFLAGS)
QMAKE_LFLAGS += $$(LDFLAGS)
TARGET = omareader
TEMPLATE = app

# VERSION is the one place the version lives; bin/release bumps it.
APP_VERSION = $$cat($$PWD/VERSION)
DEFINES += OMAREADER_VERSION=\\\"$$APP_VERSION\\\"

# The Dropbox app key for sync (docs/sync.md); public, as PKCE needs no
# secret. Empty builds without sync.
DROPBOX_KEY = $$cat($$PWD/DROPBOX_APP_KEY)
DEFINES += OMAREADER_DROPBOX_KEY=\\\"$$DROPBOX_KEY\\\"

# The Dropbox sign-in is kept in the Secret Service keyring.
CONFIG += link_pkgconfig
PKGCONFIG += libsecret-1

HEADERS += \
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
    src/dropbox.h \
    src/sync.h \
    src/syncengine.h \
    src/syncstate.h \
    src/tokenstore.h \
    src/synthesizer.h \
    src/theme.h \
    src/titleagent.h \
    src/translator.h \
    src/voicesettings.h \
    src/zipwriter.h

SOURCES += \
    src/main.cpp \
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
    src/dropbox.cpp \
    src/sync.cpp \
    src/syncengine.cpp \
    src/syncstate.cpp \
    src/tokenstore.cpp \
    src/synthesizer.cpp \
    src/theme.cpp \
    src/titleagent.cpp \
    src/translator.cpp \
    src/voicesettings.cpp \
    src/zipwriter.cpp

RESOURCES += src/resources.qrc
