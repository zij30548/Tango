QT += core network testlib
QT -= gui
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = conversationclient_test
INCLUDEPATH += ..
SOURCES += conversationclient_test.cpp ../conversationclient.cpp ../appbackendclient.cpp ../neteaseclient.cpp ../musictaskclient.cpp
HEADERS += ../conversationclient.h ../appbackendclient.h ../neteaseclient.h ../musictaskclient.h
