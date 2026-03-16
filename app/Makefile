CC     = gcc
CFLAGS = -Wall -O2 -I../include

.PHONY: all clean

all: ltfm

ltfm: ltfm.c ../include/ltcrypt.h
	$(CC) $(CFLAGS) -o ltfm ltfm.c

clean:
	rm -f ltfm
