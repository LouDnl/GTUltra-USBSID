CC?=gcc
#CFLAGS+=`sdl2-config --cflags`
CFLAGS+=-I$(PWD)/SDL
LIBS+=`sdl2-config --libs`

all: clean \
	dat2inc \
	datafile

datafile:
	$(CC) $(CFLAGS) $(LIBS) -o datafile datafile.c bme_end.c
	strip datafile

dat2inc:
	$(CC) $(CFLAGS) $(LIBS) -o dat2inc dat2inc.c
	strip dat2inc

clean:
	$(RM) *.o
	$(RM) datafile
	$(RM) dat2inc
