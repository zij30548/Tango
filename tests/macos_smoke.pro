QT += core gui widgets multimedia network sql testlib
CONFIG += c++17 testcase
include(../focus.pri)
CONFIG -= app_bundle
TARGET = macos_smoke

INCLUDEPATH += ..
SOURCES += macos_smoke.cpp \
    ../aboutdialog.cpp \
    ../historysong.cpp \
    ../likesong.cpp \
    ../mainwindow.cpp \
    ../playerwindowview.cpp \
    ../lyricswidget.cpp \
    ../neteaseclient.cpp \
    ../neteasedialogs.cpp \
    ../ranklist.cpp \
    ../signin.cpp \
    ../signup.cpp

HEADERS += ../aboutdialog.h \
    ../historysong.h \
    ../likesong.h \
    ../mainwindow.h \
    ../playerwindowview.h \
    ../lyricswidget.h \
    ../neteaseclient.h \
    ../neteasedialogs.h \
    ../neteasetypes.h \
    ../ranklist.h \
    ../signin.h \
    ../signup.h \
    ../redisconnection.h

FORMS += ../aboutdialog.ui \
    ../historysong.ui \
    ../likesong.ui \
    ../ranklist.ui \
    ../signin.ui \
    ../signup.ui

macx {
    HIREDIS_PREFIX = /opt/homebrew/opt/hiredis
    INCLUDEPATH += $$HIREDIS_PREFIX/include
    LIBS += -L$$HIREDIS_PREFIX/lib -lhiredis
    QMAKE_RPATHDIR += $$HIREDIS_PREFIX/lib
}
