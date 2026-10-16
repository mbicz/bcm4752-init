# bcm4752-init

Userspace initialisation daemon for the **Broadcom BCM4752** GPS receiver as wired up in the
Lenovo Yoga Book YB1-X90F (Cherry Trail / Intel Atom x5-Z8550, Android edition).

It replaces the vendor daemon `glgps_intelx86android` for the *initialisation* phase only:

1. power-cycle the receiver (rfkill block/unblock + runtime PM),
2. bring up the UART at 921600 8N1,
3. run the SEC/BRI boot handshake,
4. upload the receiver RAM patch from a file **you supply**,
5. switch the receiver from boot mode to GLME mode.

GNSS measurement, satellite tracking and NMEA generation are **out of scope**: after the mode
switch the receiver streams GLME frames, and a separate program has to speak that protocol.

## Related work

| what | where |
| --- | --- |
| the device-side port this daemon was written for (kernel, firmware, notes) | a private tree on the author's machine, not published |
| hardware check suite for the same tablet | https://github.com/mbicz/yogabook-hardware-check |
| the stock vendor daemon this replaces | `glgps_intelx86android`, extracted from your own ROM — see below |

This daemon was developed against the Lenovo Yoga Book YB1-X90F (Cherry Trail, `BCM4752:00` on
`/dev/ttyS2`). Nothing in it is tablet-specific except the default port path and the rfkill type, both of
which are options.

## Provenance and legal position

This is an **independent reimplementation of the wire protocol**, written from black-box analysis of
the UART traffic the vendor daemon produces on the author's own device. The repository contains:

- original C and Python code, written for this project;
- `SPEC.md`, a description of the *observed* protocol behaviour (frame layout, ordering,
  timing, and the RAM-patch transfer algorithm).

The repository contains **no Broadcom source code, no decompiled or disassembled listing, no
disassembly-derived addressing of the vendor daemon, and no vendor firmware bytes**. In particular it does
**not** contain the BCM4752 RAM patch.

### Ownership of the RAM patch

The RAM patch is Broadcom firmware: Broadcom, or its licensors, own it. It reaches a device through the
OEM image, under the OEM's terms for the licensed product, and no licence permitting redistribution of the
patch accompanies it. Labelling a copy with Broadcom's copyright would not create that licence: attribution is
not a grant, and shipping the bytes would redistribute firmware that this project has no right to redistribute.
`tools/extract_patch.py` therefore extracts the patch from **your own copy** of the vendor daemon into a file
that stays on your machine. Do not commit that file, and do not publish it. Everything else in this repository
is original work under the MIT licence (see `LICENSE`).

### Vendor-derived information this repository does contain

Two items, both needed to interoperate, neither a copy of vendor code:

1. The frame bytes, opcodes and ordering in `SPEC.md`. These are constants of the wire protocol observed
   in UART traffic; they are the interoperability information itself.
2. Two record-table file offsets in `tools/extract_patch.py` (`0x607660`, `0x6076b0`) with the image
   `sha256` they apply to. These are locating constants for the user's own image, analogous to the patch
   addresses in `brcm_patchram_plus`. The tool refuses to proceed if the hash or the table layout does not
   match, and `--table-offset` overrides them.

Nothing else is derived from the vendor daemon: no source, no decompiled or disassembled listing, no
pseudocode, no control-flow or data-structure transcription, no symbol or log-string quotation.

Read this honestly:

- The protocol description is *interoperability information* obtained by observing traffic between the
  vendor daemon and the receiver on a device the author owns. Facts about a wire protocol and short
  protocol constants needed to interoperate are not themselves the vendor's code.
- The implementation is not a formally isolated clean-room rewrite (two teams, one never seeing the
  vendor binary): one author did both the traffic analysis and the implementation. The implementation
  was nonetheless written against the behaviour description in `SPEC.md` and contains no vendor code.
- Copyright is not the only risk. Reimplementing an interface may still touch patents or other rights.
  Nothing here is legal advice; if you intend to ship or commercially use this, get your own review.
- "Broadcom", "BCM4752" and "Lenovo" are trademarks of their owners, used here only to describe
  what the software interoperates with. This project is not affiliated with, endorsed by or supported by
  Broadcom or Lenovo.
- Practitioner precedent for the "no firmware in the repo, extractor at runtime" split is
  `acidanthera/BrcmPatchRAM`, `brcm_patchram_plus`, and the many Broadcom Bluetooth firmware
  packaging projects: those redistribute firmware only where Broadcom grants redistribution (see
  `linux-firmware/WHENCE`; there is no such grant for the GPS patch).

## Requirements

- Linux with the BCM4752 exposed as a serial port (`/dev/ttyS2` on the Yoga Book) with a GPS
  rfkill switch named `BCM4752:00`.
- root (rfkill and UART access).
- The RAM patch file extracted from your own device (see below).

## Build

```sh
make
```

## Getting the vendor daemon, then extracting the RAM patch

Two steps: obtain the vendor daemon **yourself**, then run the extractor on it. Neither step is scripted
against a download this project controls; nothing here redistributes vendor bytes.

### 1. Obtain the vendor daemon (6,365,320 bytes)

It is `/vendor/bin/gpsd` inside the stock Android 7.1.1 system image of this tablet. The image used here:

| item | value |
| --- | --- |
| archive | `YB1-X90F_USR_S100043_1709020000_WW08_BP_ROW-flashfiles.zip` |
| source | Lenovo Yoga Book YB1-X90F/L stock ROM, `archive.org` item `yb-1-x-90-f-usr-s-100043-1709020000-ww-08-bp-row-flashfiles` |
| URL | `https://archive.org/download/yb-1-x-90-f-usr-s-100043-1709020000-ww-08-bp-row-flashfiles/YB1-X90F_USR_S100043_1709020000_WW08_BP_ROW-flashfiles.zip` |
| archive size / md5 | 2,604,209,805 B / `acebae7b235d4b91e839dd806c1211d7` |
| `system.img` entry | stored deflated at zip offset 753,190,880, csize 1,851,016,071 |
| decoded image | ext4, 4,026,531,840 B |

Any equivalent ROM of the same model works if the daemon hash matches at the end of the steps below.

```sh
# a) get system.img out of the archive
unzip YB1-X90F_USR_S100043_1709020000_WW08_BP_ROW-flashfiles.zip system.img
# b) Android sparse image -> raw ext4 (Debian: android-sdk-libsparse-utils)
simg2img system.img system.ext4
# c) pull the daemon out of the ext4 (Debian: e2fsprogs)
debugfs -R 'dump /vendor/bin/gpsd /tmp/glgps_intelx86android' system.ext4
# d) verify it is the image this project was written against
sha256sum /tmp/glgps_intelx86android
#   expected: d135fc515dd3802dcd9fe807b942e1e112767f56b5a44463c64121317753c715
```

The 2.6 GB archive is only needed for that one entry; it can also be fetched entry-by-entry over
HTTP `Range` instead of downloading the whole file.

### 2. Extract the RAM patch

```sh
make patch IMAGE=/tmp/glgps_intelx86android
# or: ./tools/extract_patch.py /tmp/glgps_intelx86android -o bcm4752-patch.bin
```

The extractor reads the patch segment table of the binary you point it at, checks the image hash, and
refuses to guess if the layout does not match (`--list` prints the pieces, `--no-sha256-check` overrides the
hash check). It contains no vendor bytes. Keep `*.bin` out of version control (see `.gitignore`).

## Run

```sh
sudo ./bcm4752-init --patch bcm4752-patch.bin --verbose
```

Options:

| option | meaning |
| --- | --- |
| `--tty PATH` | serial port (default `/dev/ttyS2`) |
| `--rfkill PATH` | rfkill type `gps` state file (default: auto-detect), used only without the rfkill cycle |
| `--patch FILE` | RAM patch container produced by `tools/extract_patch.py` |
| `--dump-plan` | print the frames that would be sent, touch no hardware, exit |
| `--no-power` | do not touch rfkill / runtime PM (chip already powered) |
| `--no-rfkill-cycle` | write the sysfs `state` file only (known not to power the receiver here) |
| `--no-rfkill-restore` | leave the rfkill switch unblocked at exit |
| `--delay MS` | delay after power-on (default 1000) |
| `--timeout MS` | per-step response timeout (default 1000) |
| `--burst N` | bytes of `0x80` in the autobaud burst (default 16, matching the capture) |
| `--skip-autobaud` | omit the autobaud sync frame (one vendor capture omits it) |
| `--modem-lines` | assert RTS and DTR explicitly after configuring the port |
| `--hold MS` | after the mode switch, keep reading GLME frames for `MS` ms |
| `--verbose` | log every frame and response |

`--dump-plan` is the hardware-free verification path: its output is byte-comparable with a
capture of the vendor daemon, and `make plan PATCH=bcm4752-patch.bin` runs it for you.

## Install and run

```sh
sudo make install                      # /usr/local/sbin/bcm4752-init
sudo bcm4752-init --patch /etc/bcm4752-patch.bin
```

It is a one-shot program: it powers the receiver, runs the SEC/BRI handshake, uploads the RAM patch,
switches the receiver to GLME mode, and exits with 0 when every step was acknowledged and 1 otherwise.
It prints the patch pieces and a summary, or every frame with `--verbose`. It needs root because it writes `/dev/rfkill` and the serial port, and it holds nothing open
afterwards.

Two exit behaviours matter if a measurement client follows:

- by default the rfkill switch is left **soft-blocked** again, which is what the vendor daemon does on exit and
  which powers the receiver down;
- with `--no-rfkill-restore` the switch is left soft-unblocked, so the receiver stays powered for a client that
  speaks GLME afterwards. Verified on hardware: initialisation completes and the switch reads `soft=0` afterwards
  (`state` is not a reliable readback here — it can read 1 while the switch is unblocked).

Run the daemon before whatever consumes the receiver:
it must not race another process for `/dev/ttyS2`.

## Capture a vendor run for `make test`

The comparison test needs a capture of the *vendor* daemon talking to the receiver on your own device. The vendor
daemon is launched by the ROM's own service, so trace it where it runs — under a chroot from the stock image it is:

```sh
sudo strace -f -tt -yy -xx -s 65536 -e trace=read,write \
    -o capture.log python3 run_gps.py android-gps-final --seconds 20 --output gps.nmea
make test TRACE=capture.log PATCH=bcm4752-patch.bin
```

`-xx` is required: the trace must show the raw bytes. `-yy` annotates each file descriptor; the GPS port is
recognised either as the device (`<char 4:66>`) or, when the tracee runs inside a chroot where strace cannot
resolve the device, by its `/dev/ttyS2` path. A parsed JSON capture (`[{"sys": "write", "hex": ...}, ...]`) works
as well.

What the four checks mean, and what to expect from a fresh capture:

| check | expectation |
| --- | --- |
| 1. planned bytes == captured bytes | passes against the capture this implementation was verified with; a fresh run can differ, because the vendor daemon retransmits a data block when the receiver does not acknowledge it |
| 2. planned frames tile the captured `write()` calls | passes |
| 3. every receiver reply the daemon expects appears in order | passes |
| 4. acknowledgement frames | passes |

So checks 2–4 are the structural ones and must stay green; a red check 1 on a fresh capture is the vendor daemon's
retransmission, not a framing error.

## Verification

Capture the vendor daemon (on your own device) and compare:

```sh
make test TRACE=/path/to/capture.json PATCH=bcm4752-patch.bin
# or: ./tools/verify_frames.py --daemon ./bcm4752-init --patch bcm4752-patch.bin \
#     --trace /path/to/capture.json
```

The script concatenates the bytes the vendor daemon wrote to the GPS UART (up to the mode switch)
with the bytes this daemon plans to write, and requires them to be identical. It also checks that the
planned frames tile the captured `write()` calls, that the captured receiver replies appear in order, and
that the number of CRC-acknowledgement frames matches.

### Status on the Yoga Book YB1-X90F (2026-10-16)

| check | result |
| --- | --- |
| planned frames == recorded vendor capture (a UART capture from your own device) | yes, 11252 bytes, 119 frames |
| termios set by this daemon == termios set by the vendor daemon | identical (`B921600|CS8|CREAD|CLOCAL|CRTSCTS`, `iflag`/`oflag`/`lflag` cleared) |
| this daemon on real hardware | **initialisation complete**: autobaud echo, all four query replies, all four CRC acknowledgements and the GLME handshake received |
| same daemon with `--no-rfkill-cycle` | silent: autobaud echo never arrives, every later step times out |
| vendor daemon on the same device | works (324 NMEA sentences received) |

The cause of the earlier silence was the rfkill power-up path, not the frame plan. Writing `0` to
`/sys/class/rfkill/rfkill0/state` does not change the switch's soft-block state: after such a write the
sysfs attributes read `state=0` but `soft=1`, so the receiver stayed powered down and never answered —
not even the autobaud echo. The vendor daemon does not use sysfs at all here; it opens `/dev/rfkill` and
writes `RFKILL_OP_CHANGE_ALL` for `RFKILL_TYPE_GPS` with `soft=1` and then `soft=0` 54 ms later, which
power-cycles the receiver. This daemon does the same by default and restores `soft=1` on exit, matching the
vendor daemon. `--no-rfkill-cycle` keeps the ineffective sysfs write and reproduces the silence, which is how
the cause was isolated.

Earlier hardware variants that stayed silent all shared the ineffective rfkill path: the captured 16-byte burst with
the autobaud sync frame, `--burst 128`, `--skip-autobaud`, `--modem-lines`, and a forced rfkill `1` → `0`
sysfs power cycle. Their `strace` shows no `tcsetattr` difference from the vendor daemon, so the host framing and
line settings were right and the receiver was simply powered down.

The frame plan is verified against a recorded vendor run, and the receiver accepts it: every frame the receiver answers
in the recorded run also arrives on hardware with the same acknowledgements. The vendor daemon is still not a fixed plan —
it adapts to the receiver while it runs, and a second capture of the same vendor daemon produced 11326 SEC bytes against
this daemon's 11252, the extra bytes being a repeated data write (60 bytes in one capture, 14 in another) in the middle
of the patch transfer. This daemon sends each piece once, waits for the receiver's CRC acknowledgement, and proceeds after
its timeout when the acknowledgement does not arrive; on hardware all four acknowledgements arrive. **[OBSERVED]**

## Files

```
SPEC.md                  protocol description derived from observed behaviour
src/bcm4752_init.c       the daemon (single translation unit)
tools/extract_patch.py    RAM-patch extractor for your own vendor binary
tests/verify_frames.py    byte-compare against a captured vendor run
```

## License

MIT for the code and the protocol description (`SPEC.md`); see `LICENSE`. That licence covers
this project's own text and code only, not the vendor firmware it interoperates with.
