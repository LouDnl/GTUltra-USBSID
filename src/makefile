#
# Linux makefile
#

CC?=gcc
CXX?=g++
#CFLAGS+=`sdl2-config --cflags` `pkg-config --cflags alsa` -D__LINUX_ALSA__ `pkg-config --cflags libusb-1.0` -I/usr/local/include/libusb-1.0 -L/usr/local/lib
CFLAGS+=-ISDL `pkg-config --cflags alsa` -D__LINUX_ALSA__ `pkg-config --cflags libusb-1.0` -I/usr/local/include/libusb-1.0 -L/usr/local/lib
PREFIX=../linux/
SUFFIX=
LIBS+=`sdl2-config --libs` `pkg-config --libs alsa` -lpthread -L/usr/local/lib `pkg-config --libs libusb-1.0`

CFLAGS+=-Wno-unused-result -Wno-unused-variable -Wno-unused-function -Wno-unused-value -Wno-uninitialized -Wno-maybe-uninitialized -Wno-format-overflow -Wno-parentheses

include makefile.common
