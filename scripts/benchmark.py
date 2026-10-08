"""Repeat a fixed mosaic workload against a disposable database copy.

The supplied snapshot is read-only. Images/features may still refer to shared
files, so prepare a private feature directory before comparing index versions.
"""
import argparse
import ctypes
import hashlib
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
    parser.add_argument("--baseline-exe", type=Path, help="interleave an older binary with the current binary")
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--format", default="jpg")
    parser.add_argument("--write-mode", default="auto")
    parser.add_argument("--cpu", action="store_true")
    parser.add_argument("--out-w", type=int)
    parser.add_argument("--out-h", type=int)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--analysis-mode", choices=("off", "on", "both"), default="both")
    args = parser.parse_args()
    folder = args.output_dir.resolve()
    folder.mkdir(parents=True, exist_ok=True)
    db = folder / "run.db"
    if db == args.snapshot.resolve():
        parser.error("snapshot must differ from disposable run.db")
    variants = [("current", args.exe)]
    if args.baseline_exe:
        variants.insert(0, ("baseline", args.baseline_exe))
    def digest(path):
        with path.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()
    manifest = dict(snapshot_sha256=digest(args.snapshot), target_sha256=digest(args.target),
                    executables={name: {"path": str(exe.resolve()), "sha256": digest(exe)} for name, exe in variants},
                    color_adjust=False, topn_random=1, memory_metric="Windows peak working set; sampled total device VRAM")
    (folder / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    records = []
    modes = [False, True] if args.analysis_mode == "both" else [args.analysis_mode == "on"]
    jobs = [(analysis, run, name, exe) for analysis in modes for run in range(args.runs + 1)
            for name, exe in (variants if run % 2 == 0 else list(reversed(variants)))]
    for analysis, run, name, executable in jobs:
        mode = "analysis" if analysis else "normal"
        shutil.copy2(args.snapshot, db)
        output = folder / (name + "-" + mode + "." + args.format)
        command = [str(executable.resolve()), "mosaic", "-i", str(args.target.resolve()),
                   "-d", str(db), "-o", str(output), "--format", args.format,
                   "--write-mode", args.write_mode, "--topn-random", "1", "--benchmark"]
        if analysis:
            command.append("--analyze")
        if args.cpu:
            command.append("--cpu")
        if args.out_w and args.out_h:
            command += ["--out-w", str(args.out_w), "--out-h", str(args.out_h)]
        before_gpu = gpu_memory()
        gpu_peak = before_gpu
        rss = 0
        log = folder / f"{name}-{mode}-{run}.log"
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
        record = dict(variant=name, analysis=analysis, run=run, warmup=run == 0, wall_seconds=elapsed,
                      output_bytes=output.stat().st_size,
                      peak_rss_bytes=rss or None, gpu_device_before_mib=before_gpu,
                      gpu_device_peak_mib=gpu_peak, phases_ms=phases,
                      mean=float(quality[1]) if quality else None,
                      p90=float(quality[2]) if quality else None, command=command)
        records.append(record)
        (folder / "results.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
        print(json.dumps(record), flush=True)
    for analysis in modes:
        for name, _ in variants:
            print(name, "analysis" if analysis else "normal", "median wall seconds:", statistics.median(
                r["wall_seconds"] for r in records
                if r["variant"] == name and r["analysis"] == analysis and not r["warmup"]))


if __name__ == "__main__":
    main()
