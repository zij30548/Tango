QT += core gui widgets network concurrent testlib
CONFIG += c++17 testcase
CONFIG -= app_bundle
TARGET = radiodialog_test
INCLUDEPATH += ..
SOURCES += radiodialog_test.cpp ../radiodialog.cpp ../neteaseclient.cpp
HEADERS += ../radiodialog.h ../radiotypes.h ../assistanttypes.h ../neteasetypes.h ../dialogstyle.h ../neteaseclient.h
include(../artwork.pri)
