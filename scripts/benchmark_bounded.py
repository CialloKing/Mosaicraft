"""Compare the small/default matching budgets using the explicit C++ benchmark.

Use a private library snapshot: ANN caches remain beside the source features.
The supplied database snapshot is opened read-only; each run restores its counts.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sqlite3
import statistics
import subprocess
import time

from benchmark import peak_rss


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--baseline-exe", type=Path)
    parser.add_argument("--request", type=Path, required=True, help="canonical API mosaic JSON")
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("runs must be positive")
    folder = args.output_dir.resolve()
    folder.mkdir(parents=True, exist_ok=True)
    snapshot = args.snapshot.resolve()
    database = folder / "run.db"
    if database == snapshot:
        parser.error("snapshot must differ from disposable run.db")
    request = json.loads(args.request.read_text(encoding="utf-8-sig"))
    request.update(topNrandom=1, colorAdjust=False, analyze=False)
    variants = [("small", args.exe.resolve(), ["small"]), ("default", args.exe.resolve(), [])]
    if args.baseline_exe:
        variants.insert(0, ("baseline", args.baseline_exe.resolve(), []))
    manifest = dict(snapshot_sha256=digest(snapshot), request=request,
                    target_sha256=digest(Path(request["inputPath"])),
                    executables={name: dict(path=str(exe), sha256=digest(exe)) for name, exe, _ in variants},
                    host_memory_metric="Windows PeakWorkingSetSize; 25 ms polling; excludes VRAM")
    (folder / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    records = []
    for run in range(args.runs + 1):
        # 轮换起始版本，让两档预算和基线都覆盖相邻的采样位置。
        offset = run % len(variants)
        for name, exe, suffix in variants[offset:] + variants[:offset]:
            with sqlite3.connect(snapshot.as_uri() + "?mode=ro", uri=True) as source, sqlite3.connect(database) as dest:
                source.backup(dest)
            output = folder / (name + "." + request.get("format", "png"))
            request.update(dbPath=str(database), outputPath=str(output))
            configuration = folder / "request.json"
            configuration.write_text(json.dumps(request), encoding="utf-8")
            log = folder / f"{name}-{run}.log"
            start = time.perf_counter()
            peak = 0
            with log.open("wb") as stream:
                process = subprocess.Popen([str(exe), str(configuration)] + suffix,
                                           stdout=stream, stderr=subprocess.STDOUT)
                while process.poll() is None:
                    peak = max(peak, peak_rss(process) or 0)
                    time.sleep(.025)
            seconds = time.perf_counter() - start
            text = log.read_text(encoding="utf-8", errors="replace")
            if process.returncode:
                raise RuntimeError(f"Benchmark failed: {log}\n{text[-2000:]}")
            phases = {key.strip(): float(value) for key, value in
                      re.findall(r"^\s*([^\r\n:]+):\s*([\d.]+)\s*ms", text, re.MULTILINE)}
            counters = {key.strip(): int(value) for key, value in re.findall(
                r"^\s*((?:Matching|Feature cache|Feature source|Library feature|Image |Score temporary)[^\r\n:]+):\s*(\d+)\s*$",
                text, re.MULTILINE)}
            record = dict(variant=name, run=run, warmup=run == 0, wall_seconds=seconds,
                          peak_rss_bytes=peak or None, phases_ms=phases, counters=counters,
                          output_sha256=digest(output), output_bytes=output.stat().st_size)
            records.append(record)
            (folder / "results.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
            print(json.dumps(record), flush=True)
    medians = {name: statistics.median(row["wall_seconds"] for row in records
                                      if row["variant"] == name and not row["warmup"])
               for name, _, _ in variants}
    print(json.dumps(dict(median_seconds=medians,
                         all_output_hashes_equal=len({row["output_sha256"] for row in records}) == 1)), flush=True)


if __name__ == "__main__":
    main()
