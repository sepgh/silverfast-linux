# silverfast-linux build
#
#   make                 build helper (Linux) and sti.dll (Windows, via MinGW)
#   make test            unit tests + sti.dll self-test under Wine
#   make container       same as "make" but inside a podman/docker build image
#   make install         install into $(PREFIX) (default ~/.local)
#
# See docs/development.md for details.

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

.PHONY: all helper sti test test-helper test-sti container install uninstall clean

all: helper sti

helper: $(BUILD)/silverfast-usbd
sti: $(BUILD)/sti.dll

$(BUILD):
	mkdir -p $@

$(BUILD)/silverfast-usbd: $(HELPER_SRC) helper/src/*.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(LIBUSB_CFLAGS) -o $@ $(HELPER_SRC) $(LIBUSB_LIBS)

$(BUILD)/sti.dll: $(STI_SRC) sti/src/*.h sti/src/sti.def helper/src/protocol.h | $(BUILD)
	$(MINGW_CC) $(CFLAGS) $(WARN) -shared -static-libgcc -o $@ $(STI_SRC) sti/src/sti.def -lole32 -luuid -lws2_32

$(BUILD)/sti_selftest.exe: sti/tests/sti_selftest.c | $(BUILD)
	$(MINGW_CC) $(CFLAGS) $(WARN) -static-libgcc -o $@ $< -ladvapi32

$(BUILD)/test_%: helper/tests/test_%.c helper/src/%.c helper/src/%.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -o $@ $< helper/src/$*.c

test: test-helper test-sti

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
	install -Dm644 udev/70-silverfast-linux.rules $(DESTDIR)$(PREFIX)/lib/silverfast-linux/70-silverfast-linux.rules
	install -Dm644 scripts/components.txt $(DESTDIR)$(PREFIX)/lib/silverfast-linux/components.txt

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/silverfast-linux
	rm -rf $(DESTDIR)$(PREFIX)/lib/silverfast-linux

clean:
	rm -rf $(BUILD)
