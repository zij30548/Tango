include(routing_client_test.pro)
TARGET = lookup_context_client_test
SOURCES -= routing_client_test.cpp
SOURCES += lookup_context_client_test.cpp
HEADERS += ../lookupcontext.h
