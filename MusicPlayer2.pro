QT       += core gui
QT       += multimedia
QT       += network
QT       += sql

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17
TARGET = Tango
include(focus.pri)
isEmpty(APP_BACKEND_URL): APP_BACKEND_URL = http://127.0.0.1:16382
DEFINES += QUIET_BACKEND_URL=\\\"$$APP_BACKEND_URL\\\"

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    aboutdialog.cpp \
    historysong.cpp \
    likesong.cpp \
    main.cpp \
    mainwindow.cpp \
    playerwindowview.cpp \
    lyricswidget.cpp \
    neteaseclient.cpp \
    neteasedialogs.cpp \
    ranklist.cpp \
    signin.cpp \
    signup.cpp

HEADERS += \
    aboutdialog.h \
    historysong.h \
    likesong.h \
    mainwindow.h \
    playerwindowview.h \
    lyricswidget.h \
    neteaseclient.h \
    neteasedialogs.h \
    neteasetypes.h \
    ranklist.h \
    signin.h \
    signup.h \
    passwordvisibility.h

FORMS += \
    aboutdialog.ui \
    historysong.ui \
    likesong.ui \
    ranklist.ui \
    signin.ui \
    signup.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

HEADERS += redisconnection.h dialogstyle.h

macx {
    QMAKE_INFO_PLIST = packaging/macos/Info.plist
    isEmpty(HIREDIS_PREFIX): HIREDIS_PREFIX = /opt/homebrew/opt/hiredis
    !exists($$HIREDIS_PREFIX/include/hiredis/hiredis.h): HIREDIS_PREFIX = /usr/local/opt/hiredis
    INCLUDEPATH += $$HIREDIS_PREFIX/include
    LIBS += -L$$HIREDIS_PREFIX/lib -lhiredis
    QMAKE_LFLAGS += -Wl,-rpath,$$HIREDIS_PREFIX/lib
} else:unix {
    CONFIG += link_pkgconfig
    PKGCONFIG += hiredis
} else:win32 {
    HIREDIS_ROOT = $$(HIREDIS_ROOT)
    isEmpty(HIREDIS_ROOT): error(Set HIREDIS_ROOT to your hiredis installation)
    INCLUDEPATH += $$HIREDIS_ROOT/include
    LIBS += -L$$HIREDIS_ROOT/lib -lhiredis
}
