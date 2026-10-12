# dwm - dynamic window manager
# See the project repository for license information.

include src/config.mk

SRC = src/borderwidth.c src/drw.c src/dwm.c src/selborder.c src/sigusr2.c src/util.c
OBJ = ${SRC:.c=.o}

.PHONY: clean dist install test uninstall
.PHONY: run test install-codex ensure-codex

SHELL := /bin/bash

DEVCONTAINER_LOCAL_FOLDER := $(CURDIR)
DEVCONTAINER_FILTER := label=devcontainer.local_folder=$(DEVCONTAINER_LOCAL_FOLDER)
NPM_PREFIX ?= /home/node/.local/npm
CODEX_BIN ?= $(NPM_PREFIX)/bin/codex
CODEX_VERSION ?= 0.160.0
TREEHOUSE_VERSION ?= v3.1.2
NO_MISTAKES_VERSION ?= v1.84.0

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
	cp Makefile dwm-${VERSION}
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


# ----------------------------------------------------------------------
# Other targets
# ----------------------------------------------------------------------

run:
	(devcontainer up --workspace-folder .)
	$(MAKE) shell

stop:
	(docker ps -q --filter '$(DEVCONTAINER_FILTER)' | xargs -r docker stop)

clear:
	(docker ps -a -q --filter '$(DEVCONTAINER_FILTER)' | xargs -r docker rm -f)

clean:
	rm -f dwm ${OBJ} dwm-${VERSION}.tar.gz *.orig *.rej
	rm -f src/tests/righttile

shell:
	devcontainer exec --workspace-folder . bash

codex: ensure-codex
	"$(CODEX_BIN)" resume

ensure-codex:
	@if [ ! -x "$(CODEX_BIN)" ] || [ "$$($(CODEX_BIN) --version | awk '{print $$NF}')" != "$(CODEX_VERSION)" ]; then \
		$(MAKE) install-codex; \
	fi

install-codex:
	# npm install --global --prefix "$(NPM_PREFIX)" "@openai/codex@$(CODEX_VERSION)"
	npm install --global "@openai/codex@$(CODEX_VERSION)"
	@test "$$($(CODEX_BIN) --version | awk '{print $$NF}')" = "$(CODEX_VERSION)"

resume: codex

install-firstmate:
	# Firstmate's command-line tools. Keep npm package versions pinned so image
	# rebuilds use a known compatible set with the Node 22 base image.
	npm install --global \
        gh-axi@0.1.35 \
        chrome-devtools-axi@0.1.39 \
        tasks-axi@0.2.6 \
        quota-axi@0.1.59 \
        lavish-axi@0.1.83 \
    && npm cache clean --force

	# Register the installed agent integrations in the image's default Codex home.
	mkdir -p /home/node/.codex /home/node/.local/bin \
    && HOME=/home/node CODEX_HOME=/home/node/.codex gh-axi setup hooks \
    && HOME=/home/node CODEX_HOME=/home/node/.codex chrome-devtools-axi setup hooks \
    && HOME=/home/node CODEX_HOME=/home/node/.codex lavish-axi setup hooks \
    && chown -R node:node /home/node/.codex /home/node/.local/bin

	# Install pinned Treehouse and no-mistakes releases. Download no-mistakes
	# directly so image construction does not try to start its background daemon.
	set -eu; \
    curl -fsSL "https://github.com/kunchenguid/treehouse/releases/download/${TREEHOUSE_VERSION}/treehouse-${TREEHOUSE_VERSION}-linux-amd64.tar.gz" -o /tmp/treehouse.tar.gz; \
    tar -xzf /tmp/treehouse.tar.gz -C /home/node/.local/bin treehouse; \
    chmod 0755 /home/node/.local/bin/treehouse; \
    rm /tmp/treehouse.tar.gz

	set -eu; \
    curl -fsSL "https://github.com/kunchenguid/no-mistakes/releases/download/${NO_MISTAKES_VERSION}/no-mistakes-${NO_MISTAKES_VERSION}-linux-amd64.tar.gz" -o /tmp/no-mistakes.tar.gz; \
    tar -xzf /tmp/no-mistakes.tar.gz -C /home/node/.local/bin no-mistakes; \
    chmod 0755 /home/node/.local/bin/no-mistakes; \
    rm /tmp/no-mistakes.tar.gz
