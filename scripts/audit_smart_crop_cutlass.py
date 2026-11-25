#!/usr/bin/env python3
"""Audit PE imports and measure two local smart-crop ZIPs with the same compression."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

FORBIDDEN = re.compile(r"\b(?:cublas|cudnn)[\w.-]*\.dll\b", re.IGNORECASE)
LIMIT = 100 * 1024**2


def archive(tree, target):
    files = sorted(path for path in tree.rglob("*") if path.is_file())
    with zipfile.ZipFile(target, "x", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in files:
            z.write(path, path.relative_to(tree).as_posix())
        entries = [{"file": entry.filename, "bytes": entry.file_size, "compressedBytes": entry.compress_size}
                   for entry in z.infolist()]
    with target.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"zipBytes": target.stat().st_size, "uncompressedBytes": sum(row["bytes"] for row in entries),
            "sha256": digest, "files": entries}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("baseline", "candidate", "work"):
        parser.add_argument(arg, type=lambda path: Path(path).resolve())
    parser.add_argument("--dumpbin", default="dumpbin.exe")
    args = parser.parse_args()
    for tree in (args.baseline, args.candidate):
        if not (tree / "mosaicraft.exe").is_file() or not (tree / "opencv_dnn5.dll").is_file():
            parser.error(f"missing CLI or DNN in {tree}")
        if args.work.is_relative_to(tree):
            parser.error("work must not be inside either installation tree")
    if args.baseline == args.candidate:
        parser.error("the installation trees must be separate")
    args.work.mkdir(parents=True, exist_ok=False)
    reports = {}
    for label, tree in (("baseline", args.baseline), ("cutlass", args.candidate)):
        imports = {}
        binaries = sorted(path for path in tree.rglob("*") if path.suffix.lower() in {".exe", ".dll"})
        with (args.work / f"{label}-imports.txt").open("wb") as log:
            for binary in binaries:
                rel = binary.relative_to(tree).as_posix()
                # /IMPORTS reports both the normal and delay-load import tables.
                data = subprocess.check_output([args.dumpbin, "/nologo", "/imports", str(binary)])
                log.write(f"\n[{rel}]\n".encode("utf-8"))
                log.write(data)
                # The inspected DLL's own path also appears in the dump header.
                names = {line.strip() for line in data.decode("ascii", errors="ignore").splitlines()}
                if matches := sorted(name for name in names if FORBIDDEN.fullmatch(name)):
                    imports[rel] = matches
        reports[label] = archive(tree, args.work / f"{label}.zip")
        reports[label]["targetDllImports"] = imports
        reports[label]["targetDllFiles"] = [row["file"] for row in reports[label]["files"]
                                            if FORBIDDEN.fullmatch(Path(row["file"]).name)]
    candidate = reports["cutlass"]
    required = ("licenses/cutlass/LICENSE.txt", "cutlass-build-info.txt")
    candidate["missingNotices"] = sorted(set(required) - {row["file"] for row in candidate["files"]})
    reports["passed"] = (candidate["zipBytes"] <= LIMIT and not candidate["targetDllImports"]
                         and not candidate["targetDllFiles"] and not candidate["missingNotices"])
    (args.work / "report.json").write_text(json.dumps(reports, indent=2) + "\n", encoding="utf-8")
    return 0 if reports["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
