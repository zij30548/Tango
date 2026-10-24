QT += core network sql testlib
QT -= gui
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = focusmodel_test
INCLUDEPATH += ..
SOURCES += focusmodel_test.cpp ../focusmodelclient.cpp ../appbackendclient.cpp
HEADERS += ../focusmodelclient.h ../appbackendclient.h ../focuscore.h
