QT += core sql testlib
QT -= gui
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = preferenceitems_test
INCLUDEPATH += ..
SOURCES += preferenceitems_test.cpp ../radiostore.cpp
HEADERS += ../radiostore.h
