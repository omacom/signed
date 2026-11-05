QT += core gui qml quick quickcontrols2 testlib
CONFIG += c++17 testcase
CONFIG -= app_bundle
TARGET = scroll_tests
TEMPLATE = app
INCLUDEPATH += ../src
HEADERS += ../src/textitem.h
SOURCES += scroll_tests.cpp ../src/textitem.cpp ../src/textlayout.cpp
