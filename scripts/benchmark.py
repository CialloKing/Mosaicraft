"""Repeat a fixed mosaic workload against a disposable database copy.

The supplied snapshot is read-only. Images/features may still refer to shared
files, so prepare a private feature directory before comparing index versions.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time


def gpu_memory():
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=3,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        return int(result.stdout.splitlines()[0]) if result.returncode == 0 else None
    except (OSError, ValueError, IndexError, subprocess.TimeoutExpired):
        return None


def peak_rss(process):
    if os.name != "nt":
        return None
    class Counters(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_ulong), ("faults", ctypes.c_ulong)] + [
            (name, ctypes.c_size_t) for name in
            ("peak", "working", "peak_paged", "paged", "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile")]
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    ok = ctypes.windll.psapi.GetProcessMemoryInfo(
        ctypes.c_void_p(int(process._handle)), ctypes.byref(counters), counters.cb)
    return counters.peak if ok else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--format", default="jpg")
    parser.add_argument("--write-mode", default="auto")
    parser.add_argument("--cpu", action="store_true")
    parser.add_argument("--out-w", type=int)
    parser.add_argument("--out-h", type=int)
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()
    folder = args.output_dir.resolve()
    folder.mkdir(parents=True, exist_ok=True)
    db = folder / "run.db"
    if db == args.snapshot.resolve():
        parser.error("snapshot must differ from disposable run.db")
    records = []
    for run in range(args.runs + 1):
        shutil.copy2(args.snapshot, db)
        output = folder / ("mosaic." + args.format)
        command = [str(args.exe.resolve()), "mosaic", "-i", str(args.target.resolve()),
                   "-d", str(db), "-o", str(output), "--format", args.format,
                   "--write-mode", args.write_mode, "--topn-random", "1", "--benchmark", "--analyze"]
        if args.cpu:
            command.append("--cpu")
        if args.out_w and args.out_h:
            command += ["--out-w", str(args.out_w), "--out-h", str(args.out_h)]
        before_gpu = gpu_memory()
        gpu_peak = before_gpu
        rss = 0
        log = folder / f"run-{run}.log"
        start = time.perf_counter()
        with log.open("wb") as stream:
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
            while process.poll() is None:
                rss = max(rss, peak_rss(process) or 0)
                used = gpu_memory()
                if used is not None:
                    gpu_peak = max(gpu_peak or 0, used)
                time.sleep(0.2)
            rss = max(rss, peak_rss(process) or 0)
        elapsed = time.perf_counter() - start
        text = log.read_text(encoding="utf-8", errors="replace")
        if process.returncode:
            raise RuntimeError(f"Benchmark failed: {log}\n{text[-2000:]}")
        phases = {key.strip(): float(value) for key, value in
                  re.findall(r"^\s*([^\r\n:]+):\s*([\d.]+)\s*ms", text, re.MULTILINE)}
        quality = re.search(r"Score: mean=([\d.]+).*?p90=([\d.]+)", text)
        record = dict(run=run, warmup=run == 0, wall_seconds=elapsed,
                      peak_rss_bytes=rss or None, gpu_device_before_mib=before_gpu,
                      gpu_device_peak_mib=gpu_peak, phases_ms=phases,
                      mean=float(quality[1]) if quality else None,
                      p90=float(quality[2]) if quality else None, command=command)
        records.append(record)
        (folder / "results.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
        print(json.dumps(record), flush=True)
    print("Median wall seconds:", statistics.median(r["wall_seconds"] for r in records[1:]))


if __name__ == "__main__":
    main()
