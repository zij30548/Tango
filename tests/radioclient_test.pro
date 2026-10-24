QT += core network sql testlib
QT -= gui
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = radioclient_test
INCLUDEPATH += ..
SOURCES += radioclient_test.cpp ../radioclient.cpp ../radiostore.cpp ../appbackendclient.cpp ../neteaseclient.cpp
HEADERS += ../radioclient.h ../radiostore.h ../radiotypes.h ../assistanttypes.h ../neteasetypes.h ../appbackendclient.h ../neteaseclient.h
