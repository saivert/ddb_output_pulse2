CC?=gcc
CFLAGS?=-I/usr/local/include

all:
	$(CC) $(CFLAGS) -std=c99 -shared -O2 -o pulse2.so pulse.c $(LDFLAGS) -lpulse -fPIC -Wall -march=native
debug: CFLAGS += -DDBPULSE_DEBUG -g
debug: all

install: all
	cp pulse2.so ~/.local/lib64/deadbeef/
installdebug: debug install
