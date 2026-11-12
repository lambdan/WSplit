# WSplit native port
#   make            -> builds for the current platform (Linux or macOS)
#   make test       -> core unit tests
#   make install    -> Linux: installs to $(PREFIX) (default /usr/local)
#   make app        -> macOS: builds WSplit.app

UNAME := $(shell uname -s)
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
CORE := core/run.c core/runfile.c core/settings.c core/app.c core/render.c core/menu.c
PREFIX ?= /usr/local

ifeq ($(UNAME),Darwin)
all: app
else
all: wsplit
endif

# ---------------------------------------------------------------- Linux
LINUX_PKGS := x11 cairo freetype2
wsplit: $(CORE) linux/main.c linux/ui.c linux/x11.h core/wsplit.h
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(LINUX_PKGS)) \
		$(CORE) linux/main.c linux/ui.c -o $@ \
		$(shell pkg-config --libs $(LINUX_PKGS)) -lm

install: wsplit
	install -Dm755 wsplit $(DESTDIR)$(PREFIX)/bin/wsplit
	install -Dm644 assets/digital-7-mono.ttf $(DESTDIR)$(PREFIX)/share/wsplit/digital-7-mono.ttf
	for s in 016 032 048 128 256 512; do \
		install -Dm644 assets/icons/icon_$$s.png $(DESTDIR)$(PREFIX)/share/wsplit/icons/icon_$$s.png; \
	done
	install -Dm644 assets/icons/icon_256.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/wsplit.png
	install -Dm644 linux/wsplit.desktop $(DESTDIR)$(PREFIX)/share/applications/wsplit.desktop

# ---------------------------------------------------------------- macOS
MAC_CFLAGS := -O2 -std=gnu11 -Wall -Wno-unused-parameter -Wno-four-char-constants -Wno-deprecated-declarations
MAC_FLAGS := -fobjc-arc -mmacosx-version-min=10.13
MAC_ARCHS ?= -arch arm64 -arch x86_64
MAC_FRAMEWORKS := -framework Cocoa -framework Carbon -framework QuartzCore -framework CoreText

WSplit.app/Contents/MacOS/WSplit: $(CORE) macos/*.m macos/*.h core/wsplit.h
	mkdir -p WSplit.app/Contents/MacOS WSplit.app/Contents/Resources
	clang $(MAC_CFLAGS) $(MAC_FLAGS) $(MAC_ARCHS) $(CORE) macos/*.m -o $@ $(MAC_FRAMEWORKS)

app: WSplit.app/Contents/MacOS/WSplit
	cp macos/Info.plist WSplit.app/Contents/Info.plist
	cp assets/digital-7-mono.ttf WSplit.app/Contents/Resources/
	./macos/make_icns.sh assets/icons WSplit.app/Contents/Resources/WSplit.icns
	codesign --force --deep -s - WSplit.app 2>/dev/null || true
	@echo "Built WSplit.app"

# ---------------------------------------------------------------- tests
test: tests/test_core
	./tests/test_core

tests/test_core: tests/test_core.c core/run.c core/runfile.c core/settings.c core/wsplit.h
	$(CC) $(CFLAGS) tests/test_core.c core/run.c core/runfile.c core/settings.c -lm -o $@

clean:
	rm -rf wsplit tests/test_core WSplit.app

.PHONY: all app install test clean
