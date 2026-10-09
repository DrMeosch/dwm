# dwm - dynamic window manager
# See the project repository for license information.

include src/config.mk

SRC = src/drw.c src/dwm.c src/util.c
OBJ = ${SRC:.c=.o}

all: dwm

.c.o:
	${CC} -c ${CFLAGS} $< -o $@

src/dwm.o: src/config.h src/config.mk src/drw.h src/util.h
src/drw.o: src/config.mk src/drw.h src/util.h
src/util.o: src/config.mk src/util.h

dwm: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	rm -f dwm ${OBJ} dwm-${VERSION}.tar.gz *.orig *.rej
	rm -f src/tests/righttile

test:
	@$(MAKE) -C src test

dist: clean
	mkdir -p dwm-${VERSION}
	cp -R src/Makefile src/README.md src/config.mk src/config.h\
		src/dwm.1 src/drw.h src/util.h src/dwm.c src/drw.c src/util.c\
		src/transient.c src/tests dwm-${VERSION}
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

.PHONY: all clean dist install test uninstall
