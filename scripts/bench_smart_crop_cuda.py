#!/usr/bin/env python3
"""Compare smart-normalize devices, CUDA pipelining, hybrid work, or Conv2 weight reuse."""

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import time

from benchmark import peak_rss

IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"}


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_report(path, report):
    # Preserve the last complete checkpoint if a long benchmark is interrupted.
    temporary = path.with_suffix(".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def nvidia_query(gpu, query):
    return ["nvidia-smi", "-i", gpu, f"--query-gpu={query}", "--format=csv,noheader,nounits"]


def stop(process, kill=False):
    if process.poll() is None:
        (process.kill if kill else process.terminate)()
    process.wait()


def run(args, name, parts, *, reuse_weights=None):
    samples_path = args.work / f"{name}-vram.csv"
    has_cuda = any(device == "cuda" for device, *_ in parts)
    baseline = None
    processes = []
    with ExitStack() as stack:
        samples = stack.enter_context(samples_path.open("w", encoding="utf-8"))
        if has_cuda:
            query = nvidia_query(args.gpu, "memory.used")
            baseline = int(subprocess.check_output(query, text=True).strip())
            monitor = subprocess.Popen(query + ["-lms", "250"], stdout=samples, stderr=subprocess.DEVNULL)
            stack.callback(stop, monitor)
        start = time.perf_counter()
        for device, pipeline, source, indices in parts:
            prefix = args.work / (name if len(parts) == 1 else f"{name}-{device}")
            stdout = stack.enter_context(prefix.with_suffix(".json").open("wb"))
            stderr = stack.enter_context(prefix.with_suffix(".log").open("wb"))
            env = {**os.environ, "CUDA_VISIBLE_DEVICES": args.gpu_uuid, "OPENCV_LOG_LEVEL": "WARNING",
                   "MOSAICRAFT_SMART_CROP_PIPELINE": str(int(pipeline))}
            if reuse_weights is not None:
                env["OPENCV_DNN_CUDA_REUSE_CONV2_WEIGHTS"] = str(int(reuse_weights))
            if device == "cpu" and args.study == "hybrid":
                env["OPENCV_FOR_THREADS_NUM"] = "2"
            command = [str(args.exe), "library", "normalize", str(source), "--recursive",
                       "-o", str(prefix), "--size", args.size, "--threads", "1" if args.study == "devices" else "3",
                       "--crop", args.mode, "--crop-device", device, "--json"]
            process = subprocess.Popen(command, cwd=args.work, stdout=stdout, stderr=stderr, env=env)
            stack.callback(stop, process, True)
            processes.append((process, device, pipeline, indices, prefix))
        peaks = [0] * len(processes)
        timed_out = False
        while True:
            running = False
            for i, (process, *_) in enumerate(processes):
                peaks[i] = max(peaks[i], peak_rss(process) or 0)
                running |= process.poll() is None
            elapsed = time.perf_counter() - start
            if not running:
                break
            if elapsed >= args.timeout:
                timed_out = True
                break
            time.sleep(0.025)

    memory = [int(s) for s in samples_path.read_text(encoding="utf-8").splitlines() if s.strip().isdigit()]
    children, outputs = [], {}
    for (process, device, pipeline, indices, prefix), peak in zip(processes, peaks):
        try:
            runtime = json.loads(prefix.with_suffix(".json").read_bytes()).get("runtime", {})
        except (ValueError, OSError):
            runtime = {}
        actual = runtime.get("device")
        metrics = runtime.get("metrics", {})
        saved = {p.name: p for p in prefix.iterdir() if p.is_file()} if prefix.is_dir() else {}
        if indices is None:
            outputs.update({name: sha256(path) for name, path in saved.items()})
        else:
            for local, original in enumerate(indices):
                suffix = args.sources[original].suffix
                if (path := saved.get(f"{local}{suffix}")) is not None:
                    outputs[f"{original}{suffix}"] = sha256(path)
        expected = len(args.sources) if indices is None else len(indices)
        children.append({"device": device, "actualDevice": actual, "exit": "timeout" if timed_out else process.returncode,
                         "saved": len(saved), "failed": max(0, expected - len(saved)), "metrics": metrics,
                         "peakWorkingSetBytes": peak or None,
                         "passed": not timed_out and process.returncode == 0 and actual == device
                         and len(saved) == expected and all(metrics.get(k, 0) > 0 for k in
                                                           ("smartCropSaliencyMs", "smartCropFaceMs"))
                         and (args.study == "devices" or metrics.get("smartCropPipeline") == int(pipeline))})
    result = {"name": name, "wallSeconds": elapsed, "imagesPerSecond": len(outputs) / elapsed,
            "gpuBaselineMiB": baseline, "gpuSampledPeakMiB": max(memory, default=None),
            "peakWorkingSetBytes": sum(peaks) if all(peaks) else None, "children": children,
            "passed": all(child["passed"] for child in children) and (not has_cuda or bool(memory))
            and len(outputs) == len(args.sources)}
    if args.study == "devices":
        # Keep the original CPU/CUDA report's per-run fields for existing consumers.
        result.update(children[0], passed=result["passed"])
    return result, outputs


def staged_input(args, name, indices):
    folder = args.work / "inputs" / name
    folder.mkdir(parents=True)
    for index in indices:
        source = args.sources[index]
        target = folder / f"{index:08d}{source.suffix}"
        try:
            os.link(source, target)
        except OSError:
            shutil.copyfile(source, target)
    return folder


def hybrid_variants(args, report):
    # Pillow only reads dimensions; inference still uses the unmodified original bytes.
    from PIL import Image

    def pixels(path):
        with Image.open(path) as img:
            return img.width * img.height

    sizes = [pixels(source) for source in args.sources]
    ordered = sorted(range(len(sizes)), key=sizes.__getitem__)
    eligible = ordered[:len(sizes) * 9 // 10]
    all_indices = list(range(len(sizes)))
    total = sum(sizes)
    baseline = staged_input(args, "all", all_indices)
    variants = {"pipeline": [("cuda", True, baseline, all_indices)]}
    report["partitions"] = {}
    for share in (5, 10):
        cpu = []
        weight = 0
        for index in eligible:
            if (weight + sizes[index]) * 100 > total * share:
                break
            cpu.append(index)
            weight += sizes[index]
        cpu.sort()
        gpu = sorted(set(all_indices) - set(cpu))
        if not cpu or len(gpu) < 8:
            raise ValueError(f"input does not support the {share}% split with at least eight CUDA images")
        name = f"hybrid{share}"
        report["partitions"][name] = {"cpuPixelShare": weight / total, "cpuIndices": cpu}
        variants[name] = [("cuda", True, staged_input(args, f"{name}-cuda", gpu), gpu),
                          ("cpu", False, staged_input(args, f"{name}-cpu", cpu), cpu)]
    return variants


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "input", "work"):
        parser.add_argument(name, type=lambda path: Path(path).resolve())
    parser.add_argument("--mode", choices=("smart-real", "smart-anime"), required=True)
    parser.add_argument("--study", choices=("devices", "pipeline", "hybrid", "weights"), default="devices",
                        help="device, pipelining, CPU/GPU split or CUDA Conv2 weight reuse comparison")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--size", default="180x320")
    parser.add_argument("--gpu", default="0", help="nvidia-smi index/UUID of the only visible GPU")
    parser.add_argument("--timeout", type=int, default=3600, help="seconds per run")
    args = parser.parse_args()
    if not args.exe.is_file() or not args.input.is_dir() or shutil.which("nvidia-smi") is None:
        parser.error("CLI, input directory and nvidia-smi must exist")
    dnn = args.exe.with_name("opencv_dnn5.dll")
    if args.study == "weights" and (os.name != "nt" or not dnn.is_file()):
        parser.error("the weights study requires a Windows CUDA CLI beside the experimental opencv_dnn5.dll")
    if args.work.is_relative_to(args.input) or args.input.is_relative_to(args.work):
        parser.error("work and input must not overlap")
    args.sources = sorted((p for p in args.input.rglob("*") if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS),
                          key=lambda p: p.relative_to(args.input).parts)
    if args.rounds < 1 or len(args.sources) < (1 if args.study == "devices" else 8):
        parser.error("positive round count and enough supported images are required")
    args.gpu_uuid = subprocess.check_output(nvidia_query(args.gpu, "uuid"), text=True).strip()
    args.work.mkdir(parents=True, exist_ok=False)

    def input_hashes():
        return {p.relative_to(args.input).as_posix(): sha256(p) for p in args.sources}

    source_hashes = input_hashes()
    report = {"executableSha256": sha256(args.exe), "mode": args.mode, "size": args.size, "study": args.study,
              "gpu": subprocess.check_output(nvidia_query(args.gpu, "name,driver_version,memory.total"), text=True).strip(),
              "inputSha256": source_hashes, "runs": []}
    if args.study == "hybrid":
        variants = hybrid_variants(args, report)
    elif args.study == "pipeline":
        variants = {label: [("cuda", pipeline, args.input, None)]
                    for label, pipeline in (("serial", False), ("pipeline", True))}
    elif args.study == "weights":
        report["opencvDnnSha256"] = sha256(dnn)
        variants = {label: [("cuda", True, args.input, None)] for label in ("pipeline", "reuse-weights")}
    else:
        variants = {device: [(device, False, args.input, None)] for device in ("cpu", "cuda")}
    labels = list(variants)
    reference = None
    report_path = args.work / "report.json"
    for cycle in range(args.rounds):
        offset = cycle % len(labels)
        for label in labels[offset:] + labels[:offset]:
            name = f"{cycle + 1}-{label}"
            reuse_weights = label == "reuse-weights" if args.study == "weights" else None
            result, outputs = run(args, name, variants[label], reuse_weights=reuse_weights)
            if reference is None:
                reference = outputs
                report["referenceOutputSha256"] = reference
            result.update(variant=label, round=cycle + 1, identical=outputs == reference,
                          differingOutputs=sorted(k for k in reference.keys() | outputs.keys()
                                                  if reference.get(k) != outputs.get(k)))
            report["runs"].append(result)
            print(f"{name}: {result['wallSeconds']:.2f}s, run OK={result['passed']}, identical={result['identical']}", flush=True)
            write_report(report_path, report)
    report["inputUnchanged"] = input_hashes() == source_hashes
    grouped = {label: sorted((r for r in report["runs"] if r["variant"] == label), key=lambda r: r["round"])
               for label in labels}
    medians = {label: statistics.median(r["wallSeconds"] for r in runs) for label, runs in grouped.items()}
    report["medianWallSeconds"] = medians
    report["medianImagesPerSecond"] = {label: statistics.median(r["imagesPerSecond"] for r in runs)
                                       for label, runs in grouped.items()}
    report["wallSavings"] = {label: 1 - medians[label] / medians[labels[0]] for label in labels[1:]}
    report["speedup"] = medians[labels[0]] / medians[labels[1]]
    if args.study in {"pipeline", "hybrid", "weights"}:
        def working_set_deltas(label):
            pairs = zip(grouped[labels[0]], grouped[label])
            return [b["peakWorkingSetBytes"] - a["peakWorkingSetBytes"] if a["peakWorkingSetBytes"] and
                    b["peakWorkingSetBytes"] else None for a, b in pairs]

        deltas = {label: working_set_deltas(label) for label in labels[1:]}
        report["workingSetIncreaseBytesByVariant"] = deltas
        acceptable = [label for label in labels[1:]
                      if all(r["passed"] and r["identical"] for r in grouped[label])
                      and all(d is not None and d <= 512 * 1024**2 for d in deltas[label])]
        if args.study == "pipeline":
            gates = "pipeline" in acceptable and report["wallSavings"]["pipeline"] >= .10
        elif args.study == "weights":
            gates = "reuse-weights" in acceptable and report["wallSavings"]["reuse-weights"] >= .05
        else:
            # For multi-process runs, the sum of both individual peaks is a conservative ceiling.
            report["hybridCandidates"] = [label for label in acceptable if report["wallSavings"][label] >= .05]
            gates = bool(report["hybridCandidates"]) and all(r["passed"] and r["identical"] for r in grouped["pipeline"])
    else:
        gates = report["speedup"] >= 1.5
    if args.study != "hybrid":
        gates &= all(r["passed"] and (args.study == "devices" or r["identical"]) for r in report["runs"])
    report["passed"] = report["inputUnchanged"] and gates
    write_report(report_path, report)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
