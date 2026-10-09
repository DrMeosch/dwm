# dwm - dynamic window manager
# See LICENSE file for copyright and license details.

include config.mk

SRC = drw.c dwm.c util.c
OBJ = ${SRC:.c=.o}

all: dwm

.c.o:
	${CC} -c ${CFLAGS} $<

dwm.o: config.h config.mk drw.h util.h
drw.o: config.mk drw.h util.h
util.o: config.mk util.h

dwm: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	rm -f dwm ${OBJ} dwm-${VERSION}.tar.gz *.orig *.rej
	rm -f tests/righttile

test:
	@testbin=tests/righttile; \
	trap 'rm -f "$$testbin"' 0 1 2 3 15; \
	${CC} -std=c99 -Wall -Wno-unused-function tests/righttile.c -lm -o "$$testbin" && "$$testbin"

dist: clean
	mkdir -p dwm-${VERSION}
	cp -R LICENSE Makefile README.md config.mk\
		dwm.1 drw.h util.h ${SRC} transient.c tests dwm-${VERSION}
	tar -cf dwm-${VERSION}.tar dwm-${VERSION}
	gzip dwm-${VERSION}.tar
	rm -rf dwm-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	install -Dm755 ./dwm ${DESTDIR}${PREFIX}/bin
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	sed "s/VERSION/${VERSION}/g" < dwm.1 > ${DESTDIR}${MANPREFIX}/man1/dwm.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/dwm.1
	mkdir -p ${DESTDIR}${PREFIX}/share/dwm
	install -Dm644 ./larbs.mom ${DESTDIR}${PREFIX}/share/dwm

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/dwm\
		${DESTDIR}${PREFIX}/share/dwm/larbs.mom\
		${DESTDIR}${MANPREFIX}/man1/dwm.1

.PHONY: all clean dist install test uninstall
