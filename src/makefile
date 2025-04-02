#
# Linux makefile
#

CFLAGS+=`sdl2-config --cflags` `pkg-config --cflags alsa` -D__LINUX_ALSA__
PREFIX=../linux/
SUFFIX=
LIBS+=`sdl2-config --libs` `pkg-config --libs alsa` -lpthread

include makefile.common
