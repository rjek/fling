CFLAGS=-O2 -Wall -Wextra -pedantic -std=c99
CC=gcc

fling: fling.c
	$(CC) -o fling $(CFLAGS) fling.c

clean:
	rm -f fling