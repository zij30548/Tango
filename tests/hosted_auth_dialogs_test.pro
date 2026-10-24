QT += core gui widgets network sql testlib
CONFIG += c++17 testcase
CONFIG -= app_bundle
TARGET = hosted_auth_dialogs_test
INCLUDEPATH += ..
SOURCES += hosted_auth_dialogs_test.cpp \
    ../signin.cpp \
    ../signup.cpp \
    ../appbackendclient.cpp
HEADERS += ../signin.h \
    ../signup.h \
    ../appbackendclient.h \
    ../dialogstyle.h \
    ../redisconnection.h
FORMS += ../signin.ui ../signup.ui
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
