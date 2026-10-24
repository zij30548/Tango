QT += core gui widgets testlib
CONFIG += c++17 testcase
CONFIG -= app_bundle
TARGET = lyrics_test

INCLUDEPATH += ..
SOURCES += lyrics_test.cpp ../lyricswidget.cpp
HEADERS += ../lyricswidget.h
