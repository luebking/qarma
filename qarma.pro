HEADERS = Qarma.h
SOURCES = Qarma.cpp
QT      += dbus gui widgets x11extras
TARGET  = qarma

LIBS    += -lX11
DEFINES += WS_X11

target.path += /usr/bin
INSTALLS += target