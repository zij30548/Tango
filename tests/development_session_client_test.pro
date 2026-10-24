QT += core network testlib
CONFIG += c++17 testcase
CONFIG -= app_bundle
TARGET = development_session_client_test
INCLUDEPATH += ..
SOURCES += development_session_client_test.cpp ../appbackendclient.cpp
HEADERS += ../appbackendclient.h development_session_fixtures.h
