#!/usr/bin/env python3
"""Compare bcm4752-init's planned frames against a captured vendor-daemon run.

    ./tests/verify_frames.py --daemon ./bcm4752-init \\
        --patch bcm4752-patch.bin --trace capture.json

The trace is a capture of the vendor daemon talking to the receiver (see README):
either the JSON produced by a strace parser (a list of {"sys": "write"/"read",
"hex": ...} objects) or a raw ``strace -xx`` log. The daemon is run with
--dump-plan, so no hardware is touched.

Checks:
  1. the concatenated host->receiver bytes are identical;
  2. the daemon's frames tile the trace's write() calls exactly;
  3. every exact receiver reply the daemon expects appears, in order, in the
     trace's read stream;
  4. every acknowledgement the daemon expects has the captured frame shape.
"""

import argparse
import json
import re
import subprocess
import sys

FD_GPS = "4:66"          # <char 4:66> on /dev/ttyS2


def unescape(s):
    out = bytearray()
    i = 0
    while i < len(s):
        if s.startswith("\\x", i) and len(s) >= i + 4:
            out.append(int(s[i + 2:i + 4], 16))
            i += 4
        elif s[i] == "\\" and i + 1 < len(s):
            simple = {"n": 10, "r": 13, "t": 9, "\\": 92, '"': 34, "a": 7, "b": 8, "f": 12, "v": 11, "0": 0}
            out.append(simple.get(s[i + 1], ord(s[i + 1])))
            i += 2
        else:
            out.append(ord(s[i]))
            i += 1
    return bytes(out)


def is_gps_fd(annotation):
    """True if a strace fd annotation refers to the GPS UART.

    strace -yy normally appends the device, ``<char 4:66>``, but when the tracee
    runs in a chroot it only reports the path, so ``/dev/ttyS2`` is accepted too.
    """
    if FD_GPS in annotation:
        return True
    return unescape(annotation).endswith(b"/ttyS2")


def trace_io(path):
    """Return (writes, reads) as lists of byte strings."""
    if path.endswith(".json"):
        events = json.load(open(path))
        writes = [bytes.fromhex(e["hex"]) for e in events
                  if e.get("sys") == "write" and e.get("hex")]
        reads = [bytes.fromhex(e["hex"]) for e in events
                 if e.get("sys") == "read" and e.get("hex")]
        return writes, reads

    writes, reads = [], []
    call = re.compile(r'^\d+\s+[\d:.]+ (read|write)\((\d+)<([^>]*)>, "(.*?)", (\d+)\) = (\d+)\s*$')
    for line in open(path, errors="replace"):
        m = call.match(line.rstrip("\n"))
        if not m or not is_gps_fd(m.group(3)):
            continue
        data = unescape(m.group(4))
        if len(data) != int(m.group(6)):
            continue
        (writes if m.group(1) == "write" else reads).append(data)
    return writes, reads


def plan(daemon, patch):
    out = subprocess.run([daemon, "--dump-plan", "--patch", patch],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"{daemon} --dump-plan failed: {out.stderr.strip()}")
    tx, rx, ack = [], [], []
    for line in out.stdout.splitlines():
        if line.startswith("tx "):
            tx.append(bytes.fromhex(line[3:]))
        elif line.startswith("rx "):
            rx.append(bytes.fromhex(line[3:]))
        elif line.startswith("ack "):
            ack.append(bytes.fromhex(line[4:]))
    return tx, rx, ack


def report(label, ok, detail=""):
    print(f"{'ok  ' if ok else 'FAIL'} {label}{': ' + detail if detail else ''}")
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--daemon", required=True)
    parser.add_argument("--patch", required=True)
    parser.add_argument("--trace", required=True)
    args = parser.parse_args()

    writes, reads = trace_io(args.trace)
    trace_tx = b"".join(writes)
    trace_rx = b"".join(reads)

    # The vendor daemon keeps transmitting after the mode switch (GLME
    # configuration and then measurement), which is out of scope here: compare
    # only up to the end of the switch frame.
    switch = bytes.fromhex("fe00fd000055aa00fffc")
    end = trace_tx.rfind(switch)
    if end < 0:
        sys.exit("error: the captured writes contain no mode switch frame")
    tail = trace_tx[:end + len(switch)]
    if trace_tx[end + len(switch):]:
        print(f"note {len(trace_tx) - end - len(switch)} captured bytes after the "
              f"mode switch are out of scope and not compared")
    trace_tx = tail
    trace_writes = writes_up_to(writes, end + len(switch))

    tx, rx, ack = plan(args.daemon, args.patch)
    planned_tx = b"".join(tx)

    ok = True
    ok &= report("transmitted bytes identical",
                 planned_tx == trace_tx,
                 f"{len(planned_tx)} planned vs {len(trace_tx)} captured")
    if planned_tx != trace_tx:
        for i, (a, b) in enumerate(zip(planned_tx, trace_tx)):
            if a != b:
                print(f"     first difference at byte {i}: {a:#04x} planned, {b:#04x} captured")
                break

    ok &= report("frames tile the captured writes", *tile(tx, trace_writes))

    position = 0
    for frame in rx:
        index = trace_rx.find(frame, position)
        if index < 0:
            ok &= report("receiver reply", False, frame.hex())
            break
        position = index + len(frame)
    else:
        ok &= report("receiver replies match", True, f"{len(rx)} replies")

    captured_acks = expected_ack_count(trace_rx)
    ok &= report("acknowledgements", captured_acks >= len(ack),
                 f"{captured_acks} captured, {len(ack)} planned")

    return 0 if ok else 1


def writes_up_to(writes, limit):
    """The captured write() calls covering only the first `limit` bytes."""
    out = []
    left = limit
    for call in writes:
        if left <= 0:
            break
        out.append(call[:left])
        left -= len(call)
    return out


def tile(tx, writes):
    """The planned frames must partition the trace's write() calls."""
    index = 0
    for n, call in enumerate(writes):
        consumed = 0
        while consumed < len(call):
            if index >= len(tx):
                return False, f"planned bytes ran out inside write() {n}"
            frame = tx[index]
            if len(frame) > len(call) - consumed:
                return False, f"frame {index} crosses write() {n}"
            if call[consumed:consumed + len(frame)] != frame:
                return False, f"frame {index} differs inside write() {n}"
            consumed += len(frame)
            index += 1
    if index != len(tx):
        return False, f"{len(tx) - index} planned frames were not transmitted"
    return True, f"{len(tx)} frames"


def expected_ack_count(trace_rx):
    pattern = re.compile(b"\xfe\x00\xfd\x2f\x15[\x00-\xff]{4}\xfc")
    return len(pattern.findall(trace_rx))


if __name__ == "__main__":
    sys.exit(main())
