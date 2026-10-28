# silverfast-linux build
#
#   make                 build helper (Linux) and sti.dll (Windows, via MinGW)
#   make test            unit tests + sti.dll self-test under Wine
#   make container       same as "make" but inside a podman/docker build image
#   make install         install into $(PREFIX) (default ~/.local)
#   make dist            release tarball from the current build outputs
#   make release         build the release tarball in an Ubuntu 22.04 container
#
# See docs/development.md for details.

VERSION    := $(shell sed -n 's/^VERSION=//p' scripts/silverfast-linux)
PREFIX     ?= $(HOME)/.local
BUILD      ?= build
CC         ?= gcc
MINGW_CC   ?= x86_64-w64-mingw32-gcc
CFLAGS     ?= -O2 -g
WARN       := -Wall -Wextra -Wno-unused-parameter
CONTAINER  ?= $(shell command -v podman 2>/dev/null || command -v docker 2>/dev/null)

LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS   := $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo -lusb-1.0)

HELPER_SRC := helper/src/usbd.c helper/src/firmware.c helper/src/rxbuf.c
STI_SRC    := sti/src/sti.c sti/src/transport.c sti/src/log.c

.PHONY: all helper sti test test-helper test-report test-sti container install uninstall dist release clean

all: helper sti

helper: $(BUILD)/silverfast-usbd
sti: $(BUILD)/sti.dll

$(BUILD):
	mkdir -p $@

MISSING_HINT := install the build dependencies (see docs/development.md) or run "make container" to build inside podman/docker

$(BUILD)/silverfast-usbd: $(HELPER_SRC) helper/src/*.h | $(BUILD)
	@pkg-config --exists libusb-1.0 2>/dev/null || { echo "error: libusb-1.0 development headers not found; $(MISSING_HINT)"; exit 1; }
	$(CC) $(CFLAGS) $(WARN) $(LIBUSB_CFLAGS) -o $@ $(HELPER_SRC) $(LIBUSB_LIBS)

$(BUILD)/sti.dll: $(STI_SRC) sti/src/*.h sti/src/sti.def helper/src/protocol.h | $(BUILD)
	@command -v $(MINGW_CC) >/dev/null || { echo "error: $(MINGW_CC) (MinGW-w64) not found; $(MISSING_HINT)"; exit 1; }
	$(MINGW_CC) $(CFLAGS) $(WARN) -D_WIN32_WINNT=0x0601 -shared -static-libgcc -o $@ $(STI_SRC) sti/src/sti.def -lole32 -luuid -lws2_32

$(BUILD)/sti_selftest.exe: sti/tests/sti_selftest.c | $(BUILD)
	$(MINGW_CC) $(CFLAGS) $(WARN) -D_WIN32_WINNT=0x0601 -static-libgcc -o $@ $< -ladvapi32

$(BUILD)/test_%: helper/tests/test_%.c helper/src/%.c helper/src/%.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -o $@ $< helper/src/$*.c

test: test-helper test-report test-sti

test-report:
	python3 tests/test_sfl_report.py
	python3 tests/test_sfl_models.py
	python3 scripts/sfl-models.py validate

test-helper: $(BUILD)/test_firmware $(BUILD)/test_rxbuf
	$(BUILD)/test_firmware
	$(BUILD)/test_rxbuf

# Runs in a throwaway Wine prefix so the user's prefixes are never touched.
test-sti: $(BUILD)/sti.dll $(BUILD)/sti_selftest.exe
	scripts/run-sti-selftest.sh $(BUILD)

container:
	@test -n "$(CONTAINER)" || { echo "podman or docker is required"; exit 1; }
	$(CONTAINER) build -t silverfast-linux-build -f build-env/Containerfile build-env
	$(CONTAINER) run --rm -v "$(CURDIR)":/src:Z -w /src silverfast-linux-build \
		make BUILD=$(BUILD) all $(BUILD)/test_firmware $(BUILD)/test_rxbuf $(BUILD)/sti_selftest.exe

install: all
	install -Dm755 scripts/silverfast-linux $(DESTDIR)$(PREFIX)/bin/silverfast-linux
	install -Dm755 $(BUILD)/silverfast-usbd $(DESTDIR)$(PREFIX)/lib/silverfast-linux/silverfast-usbd
	install -Dm644 $(BUILD)/sti.dll $(DESTDIR)$(PREFIX)/lib/silverfast-linux/sti.dll
	install -Dm644 scripts/components.txt $(DESTDIR)$(PREFIX)/lib/silverfast-linux/components.txt
	install -Dm755 scripts/sfl-report.py $(DESTDIR)$(PREFIX)/lib/silverfast-linux/sfl-report.py
	install -Dm755 scripts/sfl-models.py $(DESTDIR)$(PREFIX)/lib/silverfast-linux/sfl-models.py
	install -Dm644 models/*.json -t $(DESTDIR)$(PREFIX)/lib/silverfast-linux/models

# Same layout as "make install", packed so it can be extracted into ~/.local.
DIST_NAME := silverfast-linux-$(VERSION)-x86_64
dist: all
	rm -rf $(BUILD)/dist && mkdir -p $(BUILD)/dist/$(DIST_NAME)
	$(MAKE) --no-print-directory install DESTDIR=$(CURDIR)/$(BUILD)/dist/$(DIST_NAME) PREFIX=
	install -Dm644 README.md LICENSE -t $(BUILD)/dist/$(DIST_NAME)/share/doc/silverfast-linux
	tar -C $(BUILD)/dist --owner=0 --group=0 -czf $(BUILD)/$(DIST_NAME).tar.gz $(DIST_NAME)
	cp $(BUILD)/sti.dll $(BUILD)/silverfast-usbd $(BUILD)/dist/
	cd $(BUILD) && sha256sum $(DIST_NAME).tar.gz > SHA256SUMS
	@echo "release artifacts: $(BUILD)/$(DIST_NAME).tar.gz $(BUILD)/SHA256SUMS"

# Built on Ubuntu 22.04 (glibc 2.35) so the helper runs on older distributions too.
release:
	@test -n "$(CONTAINER)" || { echo "podman or docker is required"; exit 1; }
	$(CONTAINER) run --rm -v "$(CURDIR)":/src:Z -w /src docker.io/library/ubuntu:22.04 sh -c '\
		apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
			gcc make pkg-config libc6-dev libusb-1.0-0-dev gcc-mingw-w64-x86-64 >/dev/null && \
		make BUILD=build-release CFLAGS=-O2 clean dist build-release/test_firmware build-release/test_rxbuf build-release/sti_selftest.exe && \
		build-release/test_firmware && build-release/test_rxbuf'

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/silverfast-linux
	rm -rf $(DESTDIR)$(PREFIX)/lib/silverfast-linux

clean:
	rm -rf $(BUILD)
