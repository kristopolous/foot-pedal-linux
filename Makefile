CC ?= cc
CFLAGS ?= -Wall -O2 -Ivendor -I/usr/include/hidapi
LDFLAGS ?= -lhidapi-hidraw

CXX ?= g++
QT_CFLAGS := $(shell pkg-config --cflags Qt5Widgets) -fPIC
QT_LIBS := $(shell pkg-config --libs Qt5Widgets) -lX11 -lXtst

all: fpedal

fpedal: fpedal.c vendor/common.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# native X11 GUI (Qt5)
gui: fpedal-ui

gui/keymap_names.h: vendor/common.c scripts/gen_keymap.py
	@mkdir -p gui
	python3 scripts/gen_keymap.py vendor/common.c > gui/keymap_names.h.tmp && \
	  mv gui/keymap_names.h.tmp gui/keymap_names.h

fpedal-ui: gui/fpedal_ui.cpp gui/keymap_names.h
	$(CXX) -O2 -Wall -o $@ gui/fpedal_ui.cpp $(QT_CFLAGS) $(QT_LIBS)

# non-interactive smoke tests of the GUI: capture path (keyboard grab +
# XTEST) and the Apply path (stub fpedal). Neither writes the pedal. Uses
# xvfb when available so a real desktop's keyboard is never grabbed.
selftest: gui
	@run() { if command -v xvfb-run >/dev/null; then xvfb-run -a "$$@"; else "$$@"; fi; }; \
	run ./fpedal-ui --selftest \
	 && run ./fpedal-ui --selftest-write

install: fpedal
	install -Dm755 fpedal /usr/local/bin/fpedal
	sudo cp 50-fpedal.rules /etc/udev/rules.d/
	sudo udevadm control --reload-rules
	sudo udevadm trigger

clean:
	rm -f fpedal fpedal-ui gui/keymap_names.h

.PHONY: all gui selftest install clean
