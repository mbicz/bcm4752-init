# BCM4752 GPS receiver host protocol — initialisation description

This document describes the wire behaviour **observed** between the vendor daemon
(`glgps_intelx86android` from Lenovo's stock Yoga Book YB1-X90F ROM) and the BCM4752
receiver, as captured on the UART of a device owned by the author. It is the specification
this project's `src/bcm4752_init.c` was written against.

The labels BRI, SEC and GLME are used descriptively for the boot-ROM, boot and measurement phases of
this initialisation. They are the conventional names for this chip family's interfaces; no vendor source,
listing or output is quoted in this document.

Evidence quality is marked throughout:

- **[OBSERVED]** — byte sequence seen in the capture.
- **[INFERENCE]** — a reading of observed behaviour that was not confirmed.

## 1. Transport and power

| item | value | source |
| --- | --- | --- |
| port | `/dev/ttyS2` (vendor config name `/dev/gps/ttyGPS`) | [OBSERVED] |
| line settings | 921600 baud, 8 data bits, no parity, 1 stop bit, CRTSCTS | [OBSERVED] |
| initial settings before reconfiguration | 9600 baud 8N1 (`TCGETS`), then `TCSETS` to 921600, buffer flush before and after | [OBSERVED] |
| serial runtime PM | `/sys/class/tty/ttyS2/device/power/control` set to `on` | [OBSERVED] |
| rfkill switch | one entry of type `gps`, name `BCM4752:00`, soft-blocked then unblocked through `/dev/rfkill` | [OBSERVED] |
| delay after power-on | 1000 ms in the stock launcher; vendor configuration states 130 ms | [OBSERVED] |

### rfkill

`rfkill` is not cosmetic here: writing `soft=0` to `/sys/class/rfkill/rfkill0/state` leaves the
switch reporting `soft=1`, so the receiver stays powered down and answers nothing. The vendor daemon
instead opens `/dev/rfkill` and writes two 8-byte `struct rfkill_event` records with
`op = RFKILL_OP_CHANGE_ALL (3)`, `type = RFKILL_TYPE_GPS (6)`:

| offset | field | value |
| --- | --- | --- |
| 0 | `idx` | 0 (ignored for `CHANGE_ALL`) |
| 4 | `type` | 6, `RFKILL_TYPE_GPS` |
| 5 | `op` | 3, `RFKILL_OP_CHANGE_ALL` |
| 6 | `soft` | 1, then 0 (`hard` stays 0) |

The two writes are 54 ms apart in the capture; the unblock happens 140 ms before the UART is opened.
The block followed by the unblock power-cycles the receiver. On exit, the vendor daemon writes
`soft=1` again, which is the state this implementation restores unless `--no-rfkill-restore` is given.

`--no-rfkill-cycle` falls back to the single sysfs write and is **known not to work** on this device:
it leaves the receiver powered down, the autobaud echo never arrives and every later step times out.
The option exists only to demonstrate the cause; the default is the rfkill cycle. **[OBSERVED]**

`src/bcm4752_init.c` sets the line settings to exactly the captured `TCSETS` values:

`src/bcm4752_init.c` sets the line settings to exactly the captured `TCSETS` values:
`B921600|CS8|CREAD|CLOCAL|CRTSCTS` with `c_iflag`, `c_oflag` and `c_lflag` cleared. Verified against
a `strace` of the vendor daemon on the same device: the two `TCSETS` structs are identical. Clearing
`c_oflag` also means payload bytes are transmitted unchanged, which matters because the patch data contains
`0x0a` and `0x0d` bytes. **[OBSERVED]**

The **order and content of the host frames is not a constant of the protocol**: two captures of the same
vendor daemon on the same device differ from each other, and from the plan implemented here, in the middle of the
RAM-patch transfer. In one capture the vendor repeated a data write (60 bytes) that this implementation sends
once. The vendor daemon therefore adapts to the receiver while it runs. **[OBSERVED]**

## 2. Host→receiver frame layout

```
<soff> 00 fd <opcode> [payload…] fc
```

- `soff` is `0xfe` or `0xff`. Both appear in the capture. Which one is used is not established
  from the bytes alone, so this implementation keeps the captured sequence verbatim. **[OBSERVED]**
- the second byte is `0x00` in every captured host frame. **[OBSERVED]**
- `0xfd` separates the header from the opcode. **[OBSERVED]**
- `0xfc` terminates every captured frame. **[OBSERVED]**
- **escaping is not applied** in this mode: captured frames carry raw `0xfc`, `0xe0`, `0x00` and `0x01`
  bytes inside payloads (for example the RAM-patch payloads), and they are transmitted unchanged.
  The receiver's GLME protocol defines an `0xe0` escape scheme (see §9); SEC mode has none. **[OBSERVED]**
- no checksum or CRC was found on host→receiver frames. **[OBSERVED]**

## 3. Receiver→host frame layout

```
fe 00 fd <opcode> [payload…] fc
```

Every captured response begins `fe 00 fd` and ends with `0xfc`, and no response payload contains
`0xfc`, so a receiver→host frame can be delimited on `0xfc`. Sizes observed: 10 bytes almost always. **[OBSERVED]**

## 4. Initialisation sequence, in the order observed

| # | host frame(s) (`0x` bytes, no spaces) | receiver reply | note |
| --- | --- | --- | --- |
| 1 | `80`×16, then `fe00fd210001330e10fc` | `fe00fd210001330e10fc` (identical echo) | autobaud/line-rate training **[INFERENCE]** |
| 2 | `ff00fd0000fc` | `fe00fd000005000230fc` | query, 4-byte value `0x30020005` returned **[OBSERVED]** |
| 3 | `ff00fd1f95fc` | `fe00fd1f9500000000fc` | query of item `0x95`, value `0` **[OBSERVED]** |
| 4 | `fe00fd611600000000fc` | *(none)* | write of item `0x16`, value 0 **[OBSERVED]** |
| 5 | `ff00fd1f94fc` | `fe00fd1f9400000000fc` | query of item `0x94`, value `0` **[OBSERVED]** |
| 6 | RAM patch piece `0x7000`, 4060 B | `fe00fd2f15b69b07acfc` | see §5 |
| 7 | RAM patch piece `0x7fec`, 4 B | `fe00fd2f15750000ecfc` | see §5 |
| 8 | `fe00fd611601000000fc` | *(none)* | write of item `0x16`, value 1 **[OBSERVED]** |
| 9 | `ff00fd1f94fc` | `fe00fd1f9401000000fc` | query of item `0x94`, value `1` (matches the value written in step 8) **[OBSERVED]** |
| 10 | RAM patch piece `0x8000`, 4096 B | `fe00fd2f1530b90936fc` | see §5 |
| 11 | RAM patch piece `0x9000`, 2829 B | `fe00fd2f15444506dffc` | see §5 |
| 12 | `fe00fd000055aa00fffc` | `e0000000071002017e063002000550e001` | mode switch to GLME, see §9 |

Each step is written only after the previous reply arrived. The whole sequence took about 0.7 s
at 921600 baud. **[OBSERVED]**

Reading of the opcodes (not needed to interoperate, listed for clarity): `0x61` writes a 4-byte
item, `0x1f` reads an item, `0x00` reads a register, `0x21` is a header used by the autobaud and
CRC frames, `0x2f` acknowledges a received data piece. **[INFERENCE]**

The 4-byte value in the patch acknowledgement frames (for example `b69b07ac` for the `0x7000` piece) is
produced by the receiver. Its derivation was **not** established, and no checksum algorithm reproduced it;
this implementation logs the value and does not gate progress on it. **[OBSERVED, semantics unknown]**

An alternative frame layout exists in the vendor daemon for other ASIC revisions (`4752_B3`), with 4-byte
addresses and different header bytes. It was never exercised on this device and is not implemented here.
**[OBSERVED in the binary's tables, not in the capture]**

## 5. RAM patch transfer algorithm

The patch is a set of pieces, each with a start address and a length. The capture shows four pieces:

| piece | address | length | receiver acknowledgement |
| --- | --- | --- | --- |
| 1 | `0x7000` | 4060 | `b69b07ac` |
| 2 | `0x7fec` | 4 | `750000ec` |
| 3 | `0x8000` | 4096 | `30b90936` |
| 4 | `0x9000` | 2829 | `444506df` |

Because two pieces are adjacent (`0x8000`+4096 = `0x9000`) and the untransferred region `0x7ff0`…`0x8000`
is not covered, the pieces are best read as *whole transferred regions*, not as one region split by an
address rule. The underlying vendor patch table stores the last region as one 6925-byte record, which the vendor
daemon sent as 4096 + 2829: the observed split is "at most 4096 bytes per transfer". **[INFERENCE]**

### Per-piece framing

Given a piece at address `A` with length `L` and data `D`:

```
remaining = L; sent = 0
while remaining > 0:
    chunk   = min(remaining, 1020)
    aligned = chunk & ~3
    rem     = chunk & 3
    if aligned > 0:
        header: fe (aligned/4) fd 61 17 de c0 <lo(A+sent)> <hi(A+sent)>      # 9 bytes
        data D[sent .. sent+aligned) in pieces of at most 160 bytes
        fc
    if rem > 0:
        w = (rem + 3) >> 2                                                   # 1 for rem 1..3
        a = (A + sent + aligned + rem - 4) & 0xffff                           # wraps in 16 bits
        header: fe w fd 61 17 de c0 <lo(a)> <hi(a)>                           # 9 bytes
        data D[sent + aligned + rem - 4 .. +4)                                  # 4 bytes
        fc
    sent += chunk; remaining -= chunk
verify: fe 00 fd 21 18 <lo(A)> <hi(A)> <lo(L)> <hi(L)> fc                        # 10 bytes
```

Worked examples from the capture (all byte-exact):

- piece `(0x7000, 4060)`: chunks 1020, 1020, 1020, 1000
  (`fe ff fd 61 17 de c0 00 70`, `… 73fc`, `… 77f8`, `fe fa fd 61 17 de c0 f4 7b`),
  no remainder frame for the last chunk (1000 is a multiple of 4), verify `fe 00 fd 21 18 00 70 dc 0f fc`.
- piece `(0x7fec, 4)`: one chunk of 4 → `fe 01 fd 61 17 de c0 ec 7f` + 4 data bytes + `fc`,
  no remainder frame, verify `fe 00 fd 21 18 ec 7f 04 00 fc`.
- piece `(0x8000, 4096)`: chunks 1020×4 then 16 → header word count `04`, address `8ff0`;
  verify `fe 00 fd 21 18 00 80 00 10 fc`.
- piece `(0x9000, 2829)`: chunks 1020, 1020, 789; the 789-byte chunk yields
  `fe c5 fd 61 17 de c0 f8 97` + 788 bytes + `fc`, then a remainder frame
  `fe 01 fd 61 17 de c0 09 9b` + the last 4 bytes of the piece (`f8 a6 07 22`) + `fc`;
  verify `fe 00 fd 21 18 00 90 0d 0b fc` (length `0x0b0d` = 2829).

The remainder frame's 4 data bytes are the last 4 bytes of the piece. For a piece whose last word
is incomplete this is the last 3 already-transmitted bytes plus the final byte; the receiver appears to
need a complete trailing word. **[INFERENCE — the reason was not observed, only the bytes]**

The `N` in the 9-byte header is the number of 32-bit words in the aligned part (`1`…`255`). The 16-bit
address and length fields are little-endian. Chunk size 1020 and write-piece size 160 are both [OBSERVED].

## 6. Patch container file format

The vendor keeps its patch in a table inside the binary. This project does not redistribute it, so the
daemon reads a small container that `tools/extract_patch.py` produces from the user's own copy of the vendor
daemon. Format (little-endian):

```
offset  size  field
0       12    magic "BCM4752PATCH"
12      4     piece count
16      4     reserved, must be 0
20      ...   pieces:
              u32 address
              u32 length
              u32 reserved, must be 0
              length bytes of data
```

The extractor emits the blocks that the vendor daemon transmitted: `0x7000`/4060, `0x7fec`/4, `0x8000`/4096,
`0x9000`/2829 for the Yoga Book firmware (regions longer than 4096 bytes are split at 4096). The extractor
takes a second copy of the vendor daemon (ASIC table `4752_B3`) into account but refuses to convert it.

## 7. Verification behaviour

- After every request that has a reply, the reply must arrive before the next request is sent. **[OBSERVED]**
- The reply is matched on length and on the header bytes `fe 00 fd <opcode>` and the item/echo byte. **[OBSERVED]**
- The 4-byte value in the patch acknowledgement is recorded in the log. Its derivation was not established;
  this implementation does not treat a different value as a failure. **[OBSERVED + declared limitation]**
- What the vendor daemon does on a wrong or missing reply beyond the captured happy path (retry counts,
  abort conditions) was not observed and is not reproduced; this daemon fails after its per-step timeout.

## 8. Not established by this work

- meaning of `soff` (`0xfe` vs `0xff`) and of the constant `0x00` second byte;
- meaning of opcodes and of the item numbers (`0x16`, `0x94`, `0x95`, `0x00`, `0x61 0x17`, `0x21 0x18`);
- the checksum/CRC algorithm behind the 4-byte acknowledgement values;
- the general piece-splitting rule (only the four captured pieces are known);
- escaping behaviour, if the receiver ever enables it;
- everything after the mode switch: GLME payload semantics, configuration bursts, measurement
  output and NMEA generation.

## 9. GLME handoff

The switch frame `fe00fd000055aa00fffc` is answered with a frame delimited by `e0 00` … `e0 01`
(for the captured run: `e0000000071002017e063002000550e001`). Subsequent traffic is GLME
configuration, out of scope here. Frames in GLME mode are `e0 00 <payload> e0 01`. For one direction the
GLME protocol defines a `0xe0` escape scheme (`e0 02` = `0xe0`, `e0 03` = `0x00`, `e0 04` = `0x01`), but the
captured GLME frames carry raw `0x00`/`0x01`, so escaping was not in effect in the captured run. This document
only states what the captured frames show. **[OBSERVED]**
