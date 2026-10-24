QT += core gui widgets network testlib
include(../artwork.pri)
SOURCES += ../neteaseclient.cpp
HEADERS += ../neteaseclient.h
CONFIG += c++17 testcase
CONFIG -= app_bundle
TARGET = netease_dialogs_test
INCLUDEPATH += ..
SOURCES += netease_dialogs_test.cpp ../neteasedialogs.cpp
HEADERS += ../neteasedialogs.h ../neteasetypes.h ../dialogstyle.h
