QT += core gui widgets network sql testlib
include(../artwork.pri)
SOURCES += ../neteaseclient.cpp
HEADERS += ../neteaseclient.h
CONFIG += c++17 testcase
CONFIG -= app_bundle
TARGET = dialogs_test

INCLUDEPATH += ..
SOURCES += dialogs_test.cpp \
    ../aboutdialog.cpp \
    ../historysong.cpp \
    ../likesong.cpp \
    ../ranklist.cpp \
    ../signin.cpp \
    ../signup.cpp
SOURCES += ../appbackendclient.cpp
HEADERS += ../appbackendclient.h
HEADERS += ../aboutdialog.h \
    ../historysong.h \
    ../likesong.h \
    ../ranklist.h \
    ../signin.h \
    ../signup.h \
    ../dialogstyle.h \
    ../redisconnection.h
FORMS += ../aboutdialog.ui \
    ../historysong.ui \
    ../likesong.ui \
    ../ranklist.ui \
    ../signin.ui \
    ../signup.ui

macx {
    isEmpty(HIREDIS_PREFIX): HIREDIS_PREFIX = /opt/homebrew/opt/hiredis
    !exists($$HIREDIS_PREFIX/include/hiredis/hiredis.h): HIREDIS_PREFIX = /usr/local/opt/hiredis
    INCLUDEPATH += $$HIREDIS_PREFIX/include
    LIBS += -L$$HIREDIS_PREFIX/lib -lhiredis
    QMAKE_RPATHDIR += $$HIREDIS_PREFIX/lib
} else:unix {
    CONFIG += link_pkgconfig
    PKGCONFIG += hiredis
} else:win32 {
    HIREDIS_ROOT = $$(HIREDIS_ROOT)
    isEmpty(HIREDIS_ROOT): error(Set HIREDIS_ROOT to your hiredis installation)
    INCLUDEPATH += $$HIREDIS_ROOT/include
    LIBS += -L$$HIREDIS_ROOT/lib -lhiredis
}
