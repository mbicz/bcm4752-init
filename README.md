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

## Provenance and legal position

This is an **independent reimplementation of the wire protocol**, written from black-box analysis of
the UART traffic the vendor daemon produces on the author's own device. The repository contains:

- original C and Python code, written for this project;
- `SPEC.md`, a description of the *observed* protocol behaviour (frame layout, ordering,
  timing, and the RAM-patch transfer algorithm).

The repository contains **no Broadcom source code, no decompiled or disassembled listing, and no
vendor firmware bytes**. In particular it does **not** contain the BCM4752 RAM patch: that patch is
copyrighted Broadcom firmware, it is shipped inside Lenovo's stock ROM, and no licence to
redistribute it exists. `tools/extract_patch.py` extracts it from **your own copy** of the vendor
daemon into a file that stays on your machine. Do not commit that file.

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

## Extract the RAM patch from your own copy of the vendor daemon

```sh
./tools/extract_patch.py /path/to/glgps_intelx86android -o bcm4752-patch.bin
```

The extractor reads the patch segment table of the binary you point it at. It does not download
anything and it contains no vendor bytes. Keep `*.bin` out of version control (see `.gitignore`).

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
capture of the vendor daemon.

## Verification

Capture the vendor daemon (on your own device) and compare:

```sh
./tools/verify_frames.py --daemon ./bcm4752-init --patch bcm4752-patch.bin \
    --trace /path/to/uart-events.json
```

The script concatenates the bytes the vendor daemon wrote to the GPS UART (up to the mode switch)
with the bytes this daemon plans to write, and requires them to be identical. It also checks that the
planned frames tile the captured `write()` calls, that the captured receiver replies appear in order, and
that the number of CRC-acknowledgement frames matches.

### Status on the Yoga Book YB1-X90F (2026-10-16)

| check | result |
| --- | --- |
| planned frames == recorded vendor capture (`.artifacts/uart-events.json`) | yes, 11252 bytes, 119 frames |
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
