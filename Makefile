PREFIX ?= /usr
CFLAGS ?= -g -Wall -Wextra -Wno-unused-parameter

all: boxer

boxer: boxer.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ $<

install: boxer
	install -d "${DESTDIR}${PREFIX}/bin"
	install -t "${DESTDIR}${PREFIX}/bin" -o root -g root -m 4755 $<

clean:
	rm -rf boxer
