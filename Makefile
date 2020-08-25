CFLAGS += -O2 -Wall -Wextra -pedantic -std=c99 -lm -lrt
CC ?= gcc
INSTALL ?= install
DESTDIR ?=
prefix ?= /usr
exec_prefix ?= $(prefix)
bindir = $(exec_prefix)/bin

fling: fling.c
	$(CC) -o fling $(CFLAGS) fling.c

clean:
	rm -f fling

check: fling
	./smoke-test

install:
	$(INSTALL) -m 0755 -t "$(DESTDIR)/$(bindir)" -D fling
