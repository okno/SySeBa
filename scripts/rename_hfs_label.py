#!/usr/bin/env python3
"""Replace a same-length UTF-16LE volume label in an HFS+ template."""

from __future__ import annotations

import argparse
import os
import pathlib
import tempfile


def replace_label(path: pathlib.Path, old: str, new: str) -> int:
    if len(old) != len(new):
        raise ValueError("old and new labels must have the same character count")
    original = path.read_bytes()
    needle = old.encode("utf-16-le")
    replacement = new.encode("utf-16-le")
    occurrences = original.count(needle)
    if occurrences < 1:
        raise ValueError(f"label {old!r} was not found in {path}")
    updated = original.replace(needle, replacement)

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.",
        dir=path.parent,
    )
    temporary = pathlib.Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(updated)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, path.stat().st_mode)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
    return occurrences


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("old")
    parser.add_argument("new")
    arguments = parser.parse_args()
    image = arguments.image.resolve()
    count = replace_label(image, arguments.old, arguments.new)
    print(f"{image}: replaced {count} label record(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
