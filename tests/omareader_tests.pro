QT += core gui gui-private qml quick quickcontrols2 dbus multimedia concurrent testlib
CONFIG += c++17 testcase
TARGET = omareader_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += $$files(../src/*.h) fixtures.h
SOURCES += omareader_tests.cpp fixtures.cpp $$files(../src/*.cpp)
SOURCES -= ../src/main.cpp
