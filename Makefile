CC?=gcc
CFLAGS?=-I/usr/local/include

all:
	$(CC) $(CFLAGS) -std=c99 -shared -O2 -o pulse2.so -lpulse pulse.c -fPIC -Wall -march=native