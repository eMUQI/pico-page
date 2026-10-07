#!/usr/bin/env python3
"""Regenerate the compact GB18030 decoder tables from Python's standard codec.

The checked-in table was reproduced byte for byte with CPython 3.14.7; its SHA-256 and the
license review are in docs/dependency-review.md. Update that record when the output changes.
"""

from pathlib import Path


def four_bytes(pointer):
    fourth = pointer % 10 + 0x30
    pointer //= 10
    third = pointer % 126 + 0x81
    pointer //= 126
    second = pointer % 10 + 0x30
    first = pointer // 10 + 0x81
    return bytes((first, second, third, fourth))


def main():
    values = []
    for lead in range(0x81, 0xFF):
        for trail in (*range(0x40, 0x7F), *range(0x80, 0xFF)):
            values.append(ord(bytes((lead, trail)).decode("gb18030")))
    assert len(values) == 23940 and max(values) <= 0xFFFF

    ranges = []
    for pointer in range(126 * 10 * 126 * 10):
        try:
            scalar = ord(four_bytes(pointer).decode("gb18030"))
        except UnicodeDecodeError:
            continue
        if ranges and ranges[-1][1] == pointer - 1 and ranges[-1][2] + ranges[-1][1] - ranges[-1][0] + 1 == scalar:
            ranges[-1][1] = pointer
        else:
            ranges.append([pointer, pointer, scalar])

    lines = ["static constexpr std::uint16_t kTwoByte[] = {"]
    for start in range(0, len(values), 12):
        lines.append("    " + ", ".join(f"0x{value:04x}" for value in values[start:start + 12]) + ",")
    lines.extend(("};", "", "static constexpr Range kFourByteRanges[] = {"))
    lines.extend(f"    {{{first}, {last}, 0x{scalar:x}}}," for first, last, scalar in ranges)
    lines.extend(("};", ""))
    target = Path(__file__).resolve().parents[1] / "firmware/components/reader_document/gb18030_data.inc"
    target.write_text("\n".join(lines), encoding="ascii")
    print(f"{target}: {len(values)} two-byte entries, {len(ranges)} four-byte ranges")


if __name__ == "__main__":
    main()
