QT += core sql testlib
QT -= gui
CONFIG += c++17 testcase
CONFIG -= app_bundle
TARGET = focuscore_test
INCLUDEPATH += ..
SOURCES += focuscore_test.cpp ../focuscore.cpp
HEADERS += ../focuscore.h
