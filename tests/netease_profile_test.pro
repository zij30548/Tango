QT += core network testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = netease_profile_test
INCLUDEPATH += ..
SOURCES += netease_profile_test.cpp ../neteaseclient.cpp
HEADERS += ../neteaseclient.h
