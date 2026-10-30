# dwm - dynamic window manager
# See the project repository for license information.

include src/config.mk

SRC = src/borderwidth.c src/drw.c src/dwm.c src/selborder.c src/sigusr2.c src/util.c
OBJ = ${SRC:.c=.o}

.PHONY: all clean dist install test uninstall

all: dwm

.c.o:
	${CC} -c ${CFLAGS} $< -o $@

src/borderwidth.o: src/borderwidth.h
src/dwm.o: src/config.h src/config.mk src/borderwidth.h src/drw.h src/selborder.h src/sigusr2.h src/util.h
src/drw.o: src/config.mk src/drw.h src/util.h
src/util.o: src/config.mk src/util.h

dwm: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

test:
	@TEST_CFLAGS='-std=c99 -pedantic -Wall -Wextra -Wno-unused-function -Wno-deprecated-declarations -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700L ${INCS}' \
		TEST_LIBS='${LDFLAGS}' sh src/tests/run.sh '${CURDIR}'

dist: clean
	mkdir -p dwm-${VERSION}/src
	cp Makefile LICENSE dwm-${VERSION}
	cp -R src/README.md src/config.mk src/config.h src/PKGBUILD\
		src/dwm.1 src/drw.h src/util.h src/dwm.c src/drw.c src/util.c\
		src/borderwidth.c src/borderwidth.h src/selborder.c src/selborder.h src/sigusr2.c src/sigusr2.h\
		src/shiftview.c src/vanitygaps.c src/transient.c src/tests dwm-${VERSION}/src
	tar -cf dwm-${VERSION}.tar dwm-${VERSION}
	gzip dwm-${VERSION}.tar
	rm -rf dwm-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	install -Dm755 ./dwm ${DESTDIR}${PREFIX}/bin
	mkdir -p ${DESTDIR}${MANPREFIX}/man
	sed "s/VERSION/${VERSION}/g" < src/dwm.1 > ${DESTDIR}${MANPREFIX}/man/dwm.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man/dwm.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/dwm\
		${DESTDIR}${MANPREFIX}/man/dwm.1

clean:
	rm -f dwm ${OBJ} dwm-${VERSION}.tar.gz *.orig *.rej
	rm -f src/tests/righttile
