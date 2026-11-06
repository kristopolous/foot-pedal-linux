CC ?= cc
CFLAGS ?= -Wall -O2 -Ivendor -I/usr/include/hidapi
LDFLAGS ?= -lhidapi-hidraw

all: fpedal

fpedal: fpedal.c vendor/common.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

install: fpedal
	install -Dm755 fpedal /usr/local/bin/fpedal
	sudo cp 50-fpedal.rules /etc/udev/rules.d/
	sudo udevadm control --reload-rules
	sudo udevadm trigger

clean:
	rm -f fpedal

.PHONY: all install clean
