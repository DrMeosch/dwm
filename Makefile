# dwm - dynamic window manager
# See the project repository for license information.

include src/config.mk

SRC = src/drw.c src/dwm.c src/util.c
OBJ = ${SRC:.c=.o}

# Modify these according to your needs
TAG        = ${REGISTRY}:${VER}
DOCKERFILE = .devcontainer/Dockerfile
DOCKERDIR  = .

ARM64_IMAGE = ${TAG}-arm64
AMD64_IMAGE = ${TAG}-amd64

ARM64_PATCHED_IMAGE = ${TAG}-patched-arm64
AMD64_PATCHED_IMAGE = ${TAG}-patched-amd64

ARM64_TRIVY_REPORT = trivy-arm64.json
AMD64_TRIVY_REPORT = trivy-amd64.json

ARM64_COPA_CONTEXT = copa-arm64.tar
AMD64_COPA_CONTEXT = copa-amd64.tar

.PHONY: clean dist install test uninstall
.PHONY: qemu docker-multiarch-builder build-multiarch
.PHONY: arm64 amd64 all
.PHONY: security manifests
.PHONY: run test validate install-codex ensure-codex

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

src/dwm.o: src/config.h src/config.mk src/drw.h src/util.h
src/drw.o: src/config.mk src/drw.h src/util.h
src/util.o: src/config.mk src/util.h

dwm: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}


test:
	cd src
	@testbin=tests/righttile; \
	trap 'rm -f "$$testbin"' 0 1 2 3 15; \
	${CC} -std=c99 -Wall -Wno-unused-function tests/righttile.c -lm -o "$$testbin" && "$$testbin"

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


# ----------------------------------------------------------------------
# Build
# ----------------------------------------------------------------------

build-multiarch: qemu docker-multiarch-builder amd64 arm64

arm64: qemu docker-multiarch-builder
	docker buildx build \
		--cache-to type=local,dest=/tmp/.buildx-cache-arm64-new,mode=max \
		--cache-from type=local,src=/tmp/.buildx-cache-arm64 \
		--load \
		--platform linux/arm64 \
		-t ${ARM64_IMAGE} \
		--build-arg REGISTRY=${REGISTRY} \
		-f ${DOCKERFILE} ${DOCKERDIR}

amd64: qemu docker-multiarch-builder
	docker buildx build \
		--cache-to type=local,dest=/tmp/.buildx-cache-amd64-new,mode=max \
		--cache-from type=local,src=/tmp/.buildx-cache-amd64 \
		--load \
		--platform linux/amd64 \
		-t ${AMD64_IMAGE} \
		--build-arg REGISTRY=${REGISTRY} \
		-f ${DOCKERFILE} ${DOCKERDIR}


# ----------------------------------------------------------------------
# Build prerequisites
# ----------------------------------------------------------------------

qemu: /proc/sys/fs/binfmt_misc/qemu-m68k

/proc/sys/fs/binfmt_misc/qemu-m68k:
	docker run --rm --privileged multiarch/qemu-user-static --reset -p yes

docker-multiarch-builder: qemu
	@if ! docker buildx inspect docker-multiarch >/dev/null 2>&1; then \
		echo "Creating docker-multiarch builder..."; \
		docker buildx create \
			--name docker-multiarch \
			--driver docker-container \
			--use; \
	fi
	@docker buildx inspect docker-multiarch --bootstrap >/dev/null
	@docker buildx use docker-multiarch

# ----------------------------------------------------------------------
# Security
#
# Each architecture is handled independently:
#
#   original image
#       ↓
#   Trivy
#       ↓
#   application vulnerabilities → fail
#       ↓
#   unfixable OS vulnerabilities → fail
#       ↓
#   fixable OS vulnerabilities?
#       ├── no  → keep original image
#       └── yes → Copa patch → Trivy rescan
#
# A patched image is only created for the architecture that needs it.
# ----------------------------------------------------------------------

security: ${ARM64_TRIVY_REPORT} ${AMD64_TRIVY_REPORT}
	@set -euo pipefail; \
	@docker image rm -f "${ARM64_PATCHED_IMAGE}" "${AMD64_PATCHED_IMAGE}" >/dev/null 2>&1 || true; \
	for arch in arm64 amd64; do \
		case "$$arch" in \
			arm64) \
				image="${ARM64_IMAGE}"; \
				trivy_report="${ARM64_TRIVY_REPORT}"; \
				patched_image="${ARM64_PATCHED_IMAGE}"; \
				context="${ARM64_COPA_CONTEXT}"; \
				;; \
			amd64) \
				image="${AMD64_IMAGE}"; \
				trivy_report="${AMD64_TRIVY_REPORT}"; \
				patched_image="${AMD64_PATCHED_IMAGE}"; \
				context="${AMD64_COPA_CONTEXT}"; \
				;; \
		esac; \
		echo ""; \
		echo "Security checks: $$arch"; \
		echo "============================================================"; \
		\
		app_vulns=$$(jq '[.Results[]? | select(.Class == "lang-pkgs") | .Vulnerabilities[]? | select(.Severity == "HIGH" or .Severity == "CRITICAL")] | length' "$$trivy_report"); \
		echo "$$arch: $$app_vulns HIGH/CRITICAL application vulnerabilities"; \
		if [ "$$app_vulns" -gt 0 ]; then \
			echo "$$arch: application vulnerabilities found"; \
			exit 1; \
		fi; \
		\
		unfixable=$$(jq '[.Results[]? | select(.Class == "os-pkgs") | .Vulnerabilities[]? | select((.Severity == "HIGH" or .Severity == "CRITICAL") and (.FixedVersion == null or .FixedVersion == ""))] | length' "$$trivy_report"); \
		echo "$$arch: $$unfixable unfixable HIGH/CRITICAL OS vulnerabilities"; \
		if [ "$$unfixable" -gt 0 ]; then \
			echo "$$arch: unfixable OS vulnerabilities found"; \
			exit 1; \
		fi; \
		\
		fixable=$$(jq '[.Results[]? | select(.Class == "os-pkgs") | .Vulnerabilities[]? | select((.Severity == "HIGH" or .Severity == "CRITICAL") and .FixedVersion != null and .FixedVersion != "")] | length' "$$trivy_report"); \
		echo "$$arch: $$fixable fixable HIGH/CRITICAL OS vulnerabilities"; \
		\
		if [ "$$fixable" -eq 0 ]; then \
			echo "$$arch: no patch required"; \
			continue; \
		fi; \
		\
		echo "$$arch: generating patch context"; \
		rm -rf "$$context"; \
		copacetic generate \
			--debug \
			--image "$$image" \
			--report "$$trivy_report" \
			--output-context "$$context"; \
		\
		echo "$$arch: building patched image"; \
		docker build \
			--platform "linux/$$arch" \
			-t "$$patched_image" \
			- < "$$context"; \
		echo "$$arch: rescanning patched image"; \
		trivy image \
			--severity CRITICAL,HIGH \
			--pkg-types os,library \
			--ignore-unfixed=false \
			--exit-code 1 \
			"$$patched_image"; \
		\
		echo "$$arch: patch successful"; \
	done

${ARM64_TRIVY_REPORT}: arm64
	trivy image \
		--format json \
		--output ${ARM64_TRIVY_REPORT} \
		--pkg-types os,library \
		--ignore-unfixed=false \
		--severity CRITICAL,HIGH \
		--exit-code 0 \
		${ARM64_IMAGE}

${AMD64_TRIVY_REPORT}: amd64
	trivy image \
		--format json \
		--output ${AMD64_TRIVY_REPORT} \
		--pkg-types os,library \
		--ignore-unfixed=false \
		--severity CRITICAL,HIGH \
		--exit-code 0 \
		${AMD64_IMAGE}


# ----------------------------------------------------------------------
# Manifests
#
# Each architecture independently selects either:
#
#   <tag>-arm64
#   <tag>-patched-arm64
#
# and:
#
#   <tag>-amd64
#   <tag>-patched-amd64
#
# Therefore if only one architecture required patching, the final
# manifest correctly combines the patched and unpatched images.
# ----------------------------------------------------------------------

push: security
	@set -euo pipefail; \
	\
	arm64_image="${ARM64_IMAGE}"; \
	amd64_image="${AMD64_IMAGE}"; \
	\
	if docker image inspect "${ARM64_PATCHED_IMAGE}" >/dev/null 2>&1; then \
		echo "Using patched arm64 image: ${ARM64_PATCHED_IMAGE}"; \
		arm64_image="${ARM64_PATCHED_IMAGE}"; \
	else \
		echo "Using original arm64 image: ${ARM64_IMAGE}"; \
	fi; \
	\
	if docker image inspect "${AMD64_PATCHED_IMAGE}" >/dev/null 2>&1; then \
		echo "Using patched amd64 image: ${AMD64_PATCHED_IMAGE}"; \
		amd64_image="${AMD64_PATCHED_IMAGE}"; \
	else \
		echo "Using original amd64 image: ${AMD64_IMAGE}"; \
	fi; \
	\
	echo "Pushing arm64 image: $$arm64_image"; \
	docker push "$$arm64_image"; \
	\
	echo "Pushing amd64 image: $$amd64_image"; \
	docker push "$$amd64_image"; \
	\
	echo "Creating manifest: ${TAG}"; \
	docker buildx imagetools create \
		-t "${TAG}" \
		"$$amd64_image" \
		"$$arm64_image"; \
	\
	echo "Creating manifest: ${REGISTRY}:latest"; \
	docker buildx imagetools create \
		-t "${REGISTRY}:latest" \
		"$$amd64_image" \
		"$$arm64_image"; \


# ----------------------------------------------------------------------
# Other targets
# ----------------------------------------------------------------------

run:
	(devcontainer up --workspace-folder .)
	$(MAKE) shell

stop:
	(docker ps -q --filter '$(DEVCONTAINER_FILTER)' | xargs -r docker stop)

clean:
	(docker ps -a -q --filter '$(DEVCONTAINER_FILTER)' | xargs -r docker rm -f)
	rm -f copa-* trivy-*
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
