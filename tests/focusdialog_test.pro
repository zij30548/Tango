QT += core gui widgets sql testlib
CONFIG += c++17 testcase
CONFIG -= app_bundle
TARGET = focusdialog_test
INCLUDEPATH += ..
SOURCES += focusdialog_test.cpp ../focusdialog.cpp
HEADERS += ../focusdialog.h ../focuscore.h ../dialogstyle.h
