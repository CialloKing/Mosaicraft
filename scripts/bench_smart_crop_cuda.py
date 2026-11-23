#!/usr/bin/env python3
"""Compare CPU/CUDA smart-normalize end to end; no runtime Python dependency."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import time

IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"}


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def nvidia_query(gpu, query):
    return ["nvidia-smi", "-i", gpu, f"--query-gpu={query}", "--format=csv,noheader,nounits"]


def run(args, device, name, expected):
    samples_path = args.work / f"{name}-vram.csv"
    baseline = None
    with samples_path.open("w", encoding="utf-8") as samples:
        monitor = None
        if device == "cuda":
            query = nvidia_query(args.gpu, "memory.used")
            baseline = int(subprocess.check_output(query, text=True).strip())
            monitor = subprocess.Popen(query + ["-lms", "250"], stdout=samples, stderr=subprocess.DEVNULL)
        output = args.work / name
        command = [str(args.exe), "library", "normalize", str(args.input), "--recursive",
                   "-o", str(output), "--size", args.size, "--threads", "1",
                   "--crop", args.mode, "--crop-device", device, "--json"]
        start = time.perf_counter()
        try:
            with (args.work / f"{name}.json").open("wb") as stdout, (args.work / f"{name}.log").open("wb") as stderr:
                code = subprocess.run(command, cwd=args.work, stdout=stdout, stderr=stderr,
                                      timeout=args.timeout, check=False,
                                      env={**os.environ, "CUDA_VISIBLE_DEVICES": args.gpu_uuid,
                                           "OPENCV_LOG_LEVEL": "WARNING"}).returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
        finally:
            elapsed = time.perf_counter() - start
            if monitor:
                monitor.terminate()
                monitor.wait()
    saved = sum(path.is_file() for path in output.iterdir()) if output.is_dir() else 0
    memory = [int(s) for s in samples_path.read_text(encoding="utf-8").splitlines() if s.strip().isdigit()]
    try:
        runtime = json.loads((args.work / f"{name}.json").read_bytes()).get("runtime", {})
    except (ValueError, OSError):
        runtime = {}
    actual = runtime.get("device")
    metrics = runtime.get("metrics", {})
    return {"device": device, "actualDevice": actual, "exit": code, "saved": saved,
            "failed": max(0, expected - saved), "wallSeconds": elapsed, "imagesPerSecond": saved / elapsed,
            "gpuBaselineMiB": baseline, "gpuSampledPeakMiB": max(memory, default=None),
            "passed": code == 0 and saved == expected and actual == device
            and (device != "cuda" or bool(memory))
            and all(metrics.get(k, 0) > 0 for k in ("smartCropSaliencyMs", "smartCropFaceMs")),
            "metrics": metrics}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "input", "work"):
        parser.add_argument(name, type=lambda path: Path(path).resolve())
    parser.add_argument("--mode", choices=("smart-real", "smart-anime"), required=True)
    parser.add_argument("--size", default="180x320")
    parser.add_argument("--gpu", default="0", help="nvidia-smi index/UUID of the only visible GPU")
    parser.add_argument("--timeout", type=int, default=3600, help="seconds per run")
    args = parser.parse_args()
    if not args.exe.is_file() or not args.input.is_dir() or shutil.which("nvidia-smi") is None:
        parser.error("CLI, input directory and nvidia-smi must exist")
    if args.work == args.input or args.work.is_relative_to(args.input) or args.input.is_relative_to(args.work):
        parser.error("work and input must not overlap")
    inputs = sorted(p for p in args.input.rglob("*") if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS)
    if not inputs:
        parser.error("no supported inputs")
    args.gpu_uuid = subprocess.check_output(nvidia_query(args.gpu, "uuid"), text=True).strip()
    args.work.mkdir(parents=True, exist_ok=False)
    source_hashes = {str(p.relative_to(args.input)): sha256(p) for p in inputs}
    report = {"executableSha256": sha256(args.exe), "mode": args.mode, "size": args.size,
              "gpu": subprocess.check_output(nvidia_query(args.gpu, "name,driver_version,memory.total"), text=True).strip(),
              "inputSha256": source_hashes, "runs": []}
    report_path = args.work / "report.json"
    for cycle in range(3):
        for device in (("cpu", "cuda") if cycle % 2 == 0 else ("cuda", "cpu")):
            name = f"{cycle + 1}-{device}"
            result = run(args, device, name, len(inputs))
            report["runs"].append(result)
            report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
            print(f"{name}: {result['saved']}/{len(inputs)}; exit={result['exit']}", flush=True)
            if not result["passed"]:
                return 1
    report["inputUnchanged"] = all(sha256(p) == source_hashes[str(p.relative_to(args.input))] for p in inputs)
    report["medianImagesPerSecond"] = {
        dev: statistics.median(r["imagesPerSecond"] for r in report["runs"] if r["device"] == dev)
        for dev in ("cpu", "cuda")}
    rates = report["medianImagesPerSecond"]
    report["speedup"] = rates["cuda"] / rates["cpu"]
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    return 0 if report["inputUnchanged"] and report["speedup"] >= 1.5 else 1


if __name__ == "__main__":
    raise SystemExit(main())
