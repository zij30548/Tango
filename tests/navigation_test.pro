# Complete desktop target with isolated HTTP/database fixtures.
include(hosted_flow_test.pro)
TARGET = navigation_test
SOURCES -= hosted_flow_test.cpp
SOURCES += navigation_test.cpp
