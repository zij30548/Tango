QT += core gui widgets network concurrent testlib
CONFIG += c++17 testcase
CONFIG -= app_bundle
TARGET = music_task_dialog_test
INCLUDEPATH += ..
SOURCES += music_task_dialog_test.cpp ../radiodialog.cpp ../neteaseclient.cpp ../playerwindowview.cpp ../lyricswidget.cpp
HEADERS += ../radiodialog.h ../radiotypes.h ../assistanttypes.h ../neteasetypes.h ../dialogstyle.h ../neteaseclient.h ../playerwindowview.h ../lyricswidget.h
include(../artwork.pri)
