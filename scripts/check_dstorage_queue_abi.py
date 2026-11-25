#!/usr/bin/env python3
"""Read-only check of title-observed Xbox DirectStorage callsite bytes.

This does not execute, decrypt, patch, dump, or redistribute any guest data.
Works only for a title version whose recorded RVA signatures match.
Independent of external Python packages.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

# Original title build observed by data-only disassembly on 2026-11-25.
# Only minimal x64 CALL instruction bytes and addresses are retained.
CALLS = (
    ("Factory CreateStatusArray", 0x1428C9522, bytes.fromhex("41 ff 52 28"), 0x28),
    ("Factory CreateQueue1", 0x1428C95C7, bytes.fromhex("ff 50 50"), 0x50),
    ("Queue Close", 0x1428C9637, bytes.fromhex("ff 50 40"), 0x40),
    ("Queue EnqueueRequest", 0x1428C9A0B, bytes.fromhex("ff 50 18"), 0x18),
)


class PEError(ValueError):
    pass


def read_field(raw: bytes, offset: int, fmt: str) -> int:
    if offset < 0 or offset + struct.calcsize(fmt) > len(raw):
        raise PEError("PE metadata extends outside file")
    return struct.unpack_from(fmt, raw, offset)[0]


def callsite_results(data: bytes) -> tuple[int, list[dict[str, object]]]:
    if data[:2] != b"MZ":
        raise PEError("Not a DOS/PE executable")
    pe = read_field(data, 0x3C, "<I")
    if data[pe : pe + 4] != b"PE\x00\x00":
        raise PEError("Invalid PE signature")
    machine = read_field(data, pe + 4, "<H")
    sections_count = read_field(data, pe + 6, "<H")
    optional_size = read_field(data, pe + 20, "<H")
    optional = pe + 24
    if machine != 0x8664 or read_field(data, optional, "<H") != 0x20B:
        raise PEError("Only 64-bit AMD64 PE32+ images are supported")
    image_base = read_field(data, optional + 24, "<Q")
    headers_size = read_field(data, optional + 60, "<I")
    table = optional + optional_size
    if not 1 <= sections_count <= 96 or table + sections_count * 40 > len(data):
        raise PEError("Section table invalid or truncated")

    sections = []
    for idx in range(sections_count):
        entry = table + idx * 40
        name = data[entry:entry+8].rstrip(b"\x00").decode("ascii", "replace")
        virtual_size = read_field(data, entry + 8, "<I")
        virtual_address = read_field(data, entry + 12, "<I")
        raw_size = read_field(data, entry + 16, "<I")
        raw_start = read_field(data, entry + 20, "<I")
        sections.append((name, virtual_address, virtual_size, raw_start, raw_size))

    def mapped_bytes(virtual_address: int, size: int) -> bytes:
        rva = virtual_address - image_base
        if rva < 0 or size <= 0:
            raise PEError("Invalid image-relative address")
        if rva < headers_size and rva + size <= len(data):
            return data[rva : rva + size]
        for name, va, vs, ofs, raw_size in sections:
            if va <= rva and rva + size <= va + min(vs, raw_size):
                start = ofs + rva - va
                if start + size > len(data):
                    raise PEError("Section extends beyond file")
                return data[start : start + size]
        raise PEError(f"Address 0x{virtual_address:X} is not backed by file bytes")

    results = []
    for label, address, signature, slot in CALLS:
        try:
            matches = mapped_bytes(address, len(signature)) == signature
            reason = "match" if matches else "call instruction differs"
        except PEError as exc:
            matches = False
            reason = str(exc)
        results.append(
            {"name": label, "address": f"0x{address:X}",
             "vtable_offset": f"+0x{slot:X}", "passed": matches,
             "reason": reason}
        )
    return image_base, results


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify observed DirectStorage COM callsite opcodes (read only)"
    )
    parser.add_argument("executable", type=Path, help="local unmodified AMD64 title PE")
    parser.add_argument("--json", action="store_true", help="emit simple JSON results")
    args = parser.parse_args()
    try:
        data = args.executable.read_bytes()
        image_base, results = callsite_results(data)
    except (OSError, PEError) as exc:
        print(f"ABI probe failed: {exc}", file=sys.stderr)
        return 2
    passed = all(item["passed"] for item in results)
    if args.json:
        print(json.dumps(
            {"image": args.executable.name, "image_base": f"0x{image_base:X}",
             "passed": passed, "checks": results}, indent=2))
    else:
        print(f"DirectStorage ABI callsite check: {args.executable.name}")
        for item in results:
            print(f"  {'PASS' if item['passed'] else 'FAIL'} "
                  f"{item['name']} {item['vtable_offset']} "
                  f"@{item['address']} ({item['reason']})")
        print("Guest image data was inspected only; never executed or modified.")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
