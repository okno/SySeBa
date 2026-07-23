#!/usr/bin/env python3
"""Create a deterministic Universal 2 Mach-O from two unsigned thin binaries."""

from __future__ import annotations

import argparse
import os
import pathlib
import struct
import tempfile

MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
CPU_TYPE_X86_64 = 0x01000007
CPU_TYPE_ARM64 = 0x0100000C
ALIGNMENT_EXPONENT = 14
ALIGNMENT = 1 << ALIGNMENT_EXPONENT


def parse_thin(path: pathlib.Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if len(data) < 32:
        raise ValueError(f"{path}: truncated Mach-O header")

    magic, cpu_type, cpu_subtype = struct.unpack_from("<III", data)
    if magic != MH_MAGIC_64:
        raise ValueError(f"{path}: expected a little-endian 64-bit thin Mach-O")
    if cpu_type not in (CPU_TYPE_X86_64, CPU_TYPE_ARM64):
        raise ValueError(f"{path}: unsupported CPU type 0x{cpu_type:08x}")
    return cpu_type, cpu_subtype, data


def align(value: int) -> int:
    return (value + ALIGNMENT - 1) & ~(ALIGNMENT - 1)


def build_universal(inputs: list[pathlib.Path], output: pathlib.Path) -> None:
    slices = sorted((parse_thin(path) for path in inputs), key=lambda item: item[0])
    cpu_types = {item[0] for item in slices}
    expected = {CPU_TYPE_X86_64, CPU_TYPE_ARM64}
    if len(slices) != 2 or cpu_types != expected:
        raise ValueError("exactly one x86_64 and one arm64 Mach-O are required")

    header_size = 8 + len(slices) * 20
    offset = align(header_size)
    entries: list[tuple[int, int, int, int, int]] = []
    for cpu_type, cpu_subtype, data in slices:
        if offset > 0xFFFFFFFF or len(data) > 0xFFFFFFFF:
            raise ValueError("fat_arch cannot represent an offset or slice over 4 GiB")
        entries.append(
            (
                cpu_type,
                cpu_subtype,
                offset,
                len(data),
                ALIGNMENT_EXPONENT,
            )
        )
        offset = align(offset + len(data))

    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.",
        dir=output.parent,
    )
    temporary = pathlib.Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(struct.pack(">II", FAT_MAGIC, len(entries)))
            for entry in entries:
                stream.write(struct.pack(">IIIII", *entry))
            for entry, (_, _, data) in zip(entries, slices):
                current = stream.tell()
                stream.write(b"\0" * (entry[2] - current))
                stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o755)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("x86_64", type=pathlib.Path)
    parser.add_argument("arm64", type=pathlib.Path)
    arguments = parser.parse_args()

    build_universal(
        [arguments.x86_64.resolve(), arguments.arm64.resolve()],
        arguments.output.resolve(),
    )
    print(arguments.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
