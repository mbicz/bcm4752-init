#!/usr/bin/env python3
"""Extract the BCM4752 RAM patch from your own copy of the vendor daemon.

The RAM patch is copyrighted Broadcom firmware shipped inside Lenovo's stock ROM.
It is not part of this project and must not be redistributed. This tool only reads
a binary you already have, writes the patch to a plain file, and refuses to guess:
it validates the table it finds and aborts if the layout does not match.

    ./extract_patch.py /path/to/glgps_intelx86android -o bcm4752-patch.bin

The output stays on your machine. Never commit it (see ../.gitignore).

Table layout used by the vendor daemon, verified by byte-comparison against a UART
capture of a stock run (see SPEC.md section 6):

  - two tables of 24-byte records, one per ASIC revision, selected by the
    revision string "4752_B2" or "4752_B3"
  - record fields: byte flags, u32 address, u64 data pointer (a file offset into
    the same binary), u32 length

Records longer than 4096 bytes are emitted as several pieces of at most 4096 bytes,
which is what the vendor daemon transmitted (a 6925-byte record was sent as
4096 + 2829).
"""

import argparse
import hashlib
import struct
import sys

REVISION = "bcm4752-patch-extract 0.1.0"

# Default table locations for the Lenovo Yoga Book YB1-X90F stock daemon
# (sha256 d135fc515dd3802dcd9fe807b942e1e112767f56b5a44463c64121317753c715).
DEFAULT_TABLES = {
    "4752_B2": 0x607660,
    "4752_B3": 0x6076B0,
}
EXPECTED_SHA256 = "d135fc515dd3802dcd9fe807b942e1e112767f56b5a44463c64121317753c715"
RECORD_SIZE = 24
MAX_RECORDS = 16
PIECE_MAX = 4096
MAGIC = b"BCM4752PATCH"


def parse_table(image: bytes, offset: int):
    """Parse one revision table. Returns a list of (addr, length, data_offset).

    Records are 24 bytes: byte flags, u32 address, u64 data pointer (a file
    offset into the same image), u32 length, 4 bytes of padding. The table ends
    at an all-zero record or at the first record whose data does not lie inside
    the image.
    """
    records = []
    base = offset
    while base + RECORD_SIZE <= len(image) and len(records) < MAX_RECORDS:
        flags, addr, data_ptr, length = struct.unpack_from("<BxxxIQ I", image, base)
        if flags == 0 and addr == 0 and data_ptr == 0 and length == 0:
            break
        if flags > 1 or length == 0 or length > 0x100000:
            break                      # end of this revision's table
        if data_ptr == 0 or data_ptr + length > len(image):
            break                      # end of this revision's table
        records.append((addr, length, data_ptr))
        base += RECORD_SIZE
    if not records:
        raise ValueError(f"no records at {offset:#x}")
    return records


def split_pieces(records):
    """Emit the pieces the vendor daemon transmitted: at most 4096 bytes each."""
    pieces = []
    for addr, length, data_ptr in records:
        sent = 0
        while sent < length:
            n = min(length - sent, PIECE_MAX)
            pieces.append((addr + sent, n, data_ptr + sent))
            sent += n
    return pieces


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", help="your own copy of the vendor daemon")
    parser.add_argument("-o", "--output", required=True, help="container to write")
    parser.add_argument("--revision", default="4752_B2", choices=sorted(DEFAULT_TABLES),
                        help="ASIC revision table to use (default %(default)s)")
    parser.add_argument("--table-offset", type=lambda v: int(v, 0), default=None,
                        help="override the table file offset")
    parser.add_argument("--no-sha256-check", action="store_true",
                        help="do not require the known image hash")
    parser.add_argument("--list", action="store_true", help="list the pieces and exit")
    args = parser.parse_args()

    image = open(args.image, "rb").read()
    digest = hashlib.sha256(image).hexdigest()
    print(f"{args.image}: {len(image)} bytes, sha256 {digest}", file=sys.stderr)
    if not args.no_sha256_check and digest != EXPECTED_SHA256:
        print(
            "error: this is not the image this tool was written against.\n"
            "       re-run with --no-sha256-check if you know the layout is the same",
            file=sys.stderr,
        )
        return 1

    offset = args.table_offset if args.table_offset is not None else DEFAULT_TABLES[args.revision]
    try:
        records = parse_table(image, offset)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        print(
            "error: table layout not recognised; this tool refuses to guess",
            file=sys.stderr,
        )
        return 1

    pieces = split_pieces(records)
    print(f"{args.revision} table at {offset:#x}: {len(records)} records, {len(pieces)} pieces", file=sys.stderr)
    for addr, length, data_ptr in pieces:
        print(f"  {addr:#06x} {length:6d} bytes @ image {data_ptr:#x}", file=sys.stderr)

    if args.list:
        return 0

    with open(args.output, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", len(pieces)))
        f.write(struct.pack("<I", 0))
        for addr, length, data_ptr in pieces:
            f.write(struct.pack("<III", addr, length, 0))
            f.write(image[data_ptr:data_ptr + length])
    print(
        f"wrote {args.output}: {len(pieces)} pieces.\n"
        "This file contains copyrighted Broadcom firmware extracted from your own device.\n"
        "Keep it out of version control and do not redistribute it.",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
