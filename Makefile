CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
PREFIX  ?= /usr/local

# Path to your own copy of the vendor daemon (see README section 1), and the RAM-patch
# container this project's extractor writes from it.
IMAGE   ?=
PATCH   ?= bcm4752-patch.bin

all: bcm4752-init

bcm4752-init: src/bcm4752_init.c
	$(CC) $(CFLAGS) -o $@ $<

# Hardware-free check: the daemon's planned frames must match a captured
# vendor-daemon UART trace byte for byte.
test: bcm4752-init
	@test -n "$(TRACE)" || { echo "usage: make test TRACE=/path/to/capture.json [PATCH=...]"; exit 2; }
	python3 tests/verify_frames.py --daemon ./bcm4752-init --trace "$(TRACE)" $(if $(PATCH),--patch "$(PATCH)",)

# Extract the RAM patch from your own copy of the vendor daemon.
patch: tools/extract_patch.py
	@test -n "$(IMAGE)" || { echo "usage: make patch IMAGE=/path/to/glgps_intelx86android [PATCH=$(PATCH)]"; exit 2; }
	python3 tools/extract_patch.py "$(IMAGE)" -o "$(PATCH)"

# Hardware-free: print the frames the daemon would send for that patch.
plan: bcm4752-init
	./bcm4752-init --dump-plan --patch "$(PATCH)"

install: bcm4752-init
	install -D -m 0755 bcm4752-init $(DESTDIR)$(PREFIX)/sbin/bcm4752-init

clean:
	rm -f bcm4752-init

.PHONY: all patch plan test install clean
