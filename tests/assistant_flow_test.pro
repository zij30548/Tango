# Reuse the complete desktop integration target's sources and build settings.
include(hosted_flow_test.pro)
TARGET = assistant_flow_test
SOURCES -= hosted_flow_test.cpp
SOURCES += assistant_flow_test.cpp
