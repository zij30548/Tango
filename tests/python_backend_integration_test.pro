QT += core network testlib
QT -= gui
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = python_backend_integration_test
INCLUDEPATH += ..
SOURCES += python_backend_integration_test.cpp ../appbackendclient.cpp ../conversationclient.cpp ../neteaseclient.cpp
HEADERS += ../appbackendclient.h ../conversationclient.h ../neteaseclient.h
