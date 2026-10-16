CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
PREFIX  ?= /usr/local

all: bcm4752-init

bcm4752-init: src/bcm4752_init.c
	$(CC) $(CFLAGS) -o $@ $<

# Hardware-free check: the daemon's planned frames must match a captured
# vendor-daemon UART trace byte for byte.
test: bcm4752-init
	@test -n "$(TRACE)" || { echo "usage: make test TRACE=/path/to/capture.json [PATCH=...]"; exit 2; }
	python3 tests/verify_frames.py --daemon ./bcm4752-init --trace "$(TRACE)" $(if $(PATCH),--patch "$(PATCH)",)

install: bcm4752-init
	install -D -m 0755 bcm4752-init $(DESTDIR)$(PREFIX)/sbin/bcm4752-init

clean:
	rm -f bcm4752-init

.PHONY: all test install clean
