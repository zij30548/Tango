QT += core gui widgets testlib network concurrent
CONFIG += c++17 testcase
CONFIG -= app_bundle
TARGET = coverart_test
INCLUDEPATH += ..
include(../artwork.pri)
SOURCES += coverart_test.cpp ../neteaseclient.cpp
HEADERS += ../neteaseclient.h
