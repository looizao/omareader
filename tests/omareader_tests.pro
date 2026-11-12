QT += core gui gui-private qml quick quickcontrols2 dbus multimedia concurrent network testlib
CONFIG += c++17 testcase
TARGET = omareader_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += $$files(../src/*.h) fixtures.h
SOURCES += omareader_tests.cpp fixtures.cpp $$files(../src/*.cpp)
SOURCES -= ../src/main.cpp
RESOURCES += ../src/resources.qrc

# Some tests read files from the source tree, such as the translations.
DEFINES += OMAREADER_SOURCE=\\\"$$PWD/..\\\"
