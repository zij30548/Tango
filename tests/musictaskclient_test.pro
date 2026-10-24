QT += core network testlib
QT -= gui
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = musictaskclient_test
INCLUDEPATH += ..
SOURCES += musictaskclient_test.cpp ../musictaskclient.cpp ../appbackendclient.cpp ../neteaseclient.cpp
HEADERS += ../musictaskclient.h ../appbackendclient.h ../neteaseclient.h
