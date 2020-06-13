HEADERS = Qarma.h
SOURCES = Qarma.cpp
QT      += gui widgets
lessThan(QT_MAJOR_VERSION, 6){
  unix:!macx:QT += x11extras
}
TARGET  = qarma

DISABLE_DBUS {
	DEFINES += QARMA_NO_DBUS
} else {
	QT += dbus
}

unix:!macx:LIBS    += -lX11
unix:!macx:DEFINES += WS_X11

# override: qmake PREFIX=/some/where/else
isEmpty(PREFIX) {
  PREFIX = /usr
}

target.path = $$PREFIX/bin

INSTALLS += target
