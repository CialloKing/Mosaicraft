#!/usr/bin/env python3
"""Check the six archive names and, for local staging, their detached checksums."""

import argparse
import hashlib
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    parser.add_argument("--hosted", action="store_true", help="Expect the two hosted Windows candidates")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--directory", type=Path)
    source.add_argument("--assets", type=Path, help="JSON from gh release view --json assets")
    args = parser.parse_args()
    if not re.fullmatch(r"v\d+\.\d+\.\d+", args.tag):
        parser.error("expected a stable version tag")

    matrix = json.loads((ROOT / "cmake/release-packages.json").read_text(encoding="utf-8"))["include"]
    expected = set()
    for package in matrix:
        channel = "_candidate" if args.hosted and package["hostedCandidate"] else ""
        expected.add(f"Mosaicraft_{args.tag}_{package['platform']}_{package['suffix']}{channel}.{package['extension']}")
    if len(matrix) != 6 or len(expected) != 6:
        parser.error("the release matrix must define exactly six distinct archives")
    if args.directory:
        names = [file.name for file in args.directory.iterdir() if file.is_file()]
    else:
        names = [asset["name"] for asset in json.loads(args.assets.read_text(encoding="utf-8"))["assets"]]
    archives = {name for name in names if name != "SHA256SUMS.txt" and not name.endswith(".sha256")}
    if archives != expected:
        parser.error(f"missing archives: {sorted(expected - archives)}; unexpected archives: {sorted(archives - expected)}")
    if args.directory:
        lines = []
        for name in sorted(expected):
            with (args.directory / name).open("rb") as source_file:
                digest = hashlib.sha256()
                for block in iter(lambda: source_file.read(1024 * 1024), b""):
                    digest.update(block)
            line = f"{digest.hexdigest()}  {name}"
            checksum = (args.directory / f"{name}.sha256").read_text(encoding="utf-8").strip()
            expected_hash, _, expected_name = checksum.partition("  ")
            if expected_hash.lower() != digest.hexdigest() or expected_name != name:
                parser.error(f"checksum mismatch: {name}")
            lines.append(line)
        (args.directory / "SHA256SUMS.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Verified {len(expected)} release archives for {args.tag}")


if __name__ == "__main__":
    main()
