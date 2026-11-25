#!/usr/bin/env python3
"""Compare smart-normalize devices, pipelines, and experimental inference routes."""

import argparse
from contextlib import ExitStack
import csv
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
OPENCV_CPU_FALLBACK = (b"running the whole graph on CPU", b"fallback to the CPU implementation",
                       b"switching to CPU")


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


def run(args, name, parts, *, reuse_weights=None, executable=None):
    samples_path = args.work / f"{name}-vram.csv"
    hetero = args.study in {"heterogeneous", "dual", "dual-dispatch"}
    trace = hetero or args.study == "cutlass"
    has_cuda = any(device in {"cuda", "dual"} for device, *_ in parts)
    wddm_path = args.work / f"{name}-wddm.csv"
    baseline = None
    processes = []
    with ExitStack() as stack:
        samples = stack.enter_context(samples_path.open("w", encoding="utf-8"))
        if has_cuda:
            query = nvidia_query(args.gpu, "memory.used")
            baseline = int(subprocess.check_output(query, text=True).strip())
            monitor = subprocess.Popen(query + ["-lms", "250"], stdout=samples, stderr=subprocess.DEVNULL)
            stack.callback(stop, monitor)
        if hetero:
            counters = [r"\GPU Adapter Memory(*)\Dedicated Usage", r"\GPU Adapter Memory(*)\Shared Usage"]
            monitor = subprocess.Popen(["typeperf", *counters, "-si", "1", "-o", str(wddm_path)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            stack.callback(stop, monitor)
        start = time.perf_counter()
        for device, pipeline, source, indices in parts:
            prefix = args.work / (name if len(parts) == 1 else f"{name}-{device}")
            stdout = stack.enter_context(prefix.with_suffix(".json").open("wb"))
            stderr = stack.enter_context(prefix.with_suffix(".log").open("wb"))
            env = {**os.environ, "CUDA_VISIBLE_DEVICES": args.gpu_uuid, "OPENCV_LOG_LEVEL": "WARNING",
                   "MOSAICRAFT_SMART_CROP_PIPELINE": str(int(pipeline))}
            env.pop("MOSAICRAFT_SMART_CROP_EXPERIMENT", None)
            policy = device
            if device in {"vulkan", "dual"}:
                env["MOSAICRAFT_SMART_CROP_EXPERIMENT"] = device
                env["MOSAICRAFT_SMART_CROP_VULKAN_DEVICE"] = str(args.vulkan_device)
                policy = "auto"
            if reuse_weights is not None:
                env["OPENCV_DNN_CUDA_REUSE_CONV2_WEIGHTS"] = str(int(reuse_weights))
            if device == "cpu" and args.study == "hybrid":
                env["OPENCV_FOR_THREADS_NUM"] = "2"
            if trace:
                env["MOSAICRAFT_SMART_CROP_TRACE_ROI"] = "1"
                # OpenCV graph fallback can happen below the app's CUDA session metrics.
                env["OPENCV_LOG_LEVEL"] = "INFO"
            command = [str(executable or args.exe), "library", "normalize", str(source), "--recursive",
                       "-o", str(prefix), "--size", args.size, "--threads", "1" if args.study == "devices" else "4" if hetero else "3",
                       "--crop", args.mode, "--crop-device", policy, "--json"]
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
        opencv_cpu_fallback = False
        try:
            stdout = prefix.with_suffix(".json")
            raw = stdout.read_bytes()
            if trace:
                log = prefix.with_suffix(".log").read_bytes()
                opencv_cpu_fallback = device in {"cuda", "dual"} and any(
                    marker in raw or marker in log for marker in OPENCV_CPU_FALLBACK)
                lines = raw.splitlines()
                if lines and lines[-1].startswith(b"{"):
                    # OpenCV INFO goes to stdout before the CLI's single JSON response.
                    raw = lines[-1]
                    prefix.with_suffix(".opencv.log").write_bytes(b"\n".join(lines[:-1]) + b"\n")
                    stdout.write_bytes(raw + b"\n")
            runtime = json.loads(raw).get("runtime", {})
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
                         "passed": not timed_out and not opencv_cpu_fallback and process.returncode == 0 and actual == device
                         and len(saved) == expected and all(metrics.get(k, 0) > 0 for k in
                                                           ("smartCropSaliencyMs", "smartCropFaceMs"))
                         and (args.study == "devices" or metrics.get("smartCropPipeline") == int(pipeline))})
        if trace:
            children[-1]["opencvCpuFallback"] = opencv_cpu_fallback
    result = {"name": name, "wallSeconds": elapsed, "imagesPerSecond": len(outputs) / elapsed,
            "gpuBaselineMiB": baseline, "gpuSampledPeakMiB": max(memory, default=None),
            "peakWorkingSetBytes": sum(peaks) if all(peaks) else None, "children": children,
            "passed": all(child["passed"] for child in children) and (not has_cuda or bool(memory))
            and len(outputs) == len(args.sources)}
    if args.study == "devices":
        # Keep the original CPU/CUDA report's per-run fields for existing consumers.
        result.update(children[0], passed=result["passed"])
    if trace:
        # The standalone writer publishes ROI traces in input order.
        result["cropRois"] = [line.decode("ascii") for line in (args.work / f"{name}.log").read_bytes().splitlines()
                              if line.startswith(b"smart-crop-roi ")]
        result["passed"] &= bool(result["cropRois"]) and all(
            child["metrics"].get(k) == 0 for child in children
            for k in ("smartCropCudaFallbacks", "smartCropCudaRecoveries"))
        result["passed"] &= all(child["device"] == "cpu" or child["metrics"].get("smartCropCpuImages") == 0 for child in children)
    if hetero:
        result["wddmSampledPeakBytes"] = wddm_peaks(wddm_path)
        metrics = children[0]["metrics"]
        expected = {"cpu": ("Cpu",), "cuda": ("Cuda",), "vulkan": ("Vulkan",), "dual": ("Cuda", "Vulkan")}
        counts = [metrics.get(f"smartCrop{key}Images", 0) for key in expected[children[0]["device"]]]
        result["passed"] &= all(counts) and sum(counts) == len(result["cropRois"])
    return result, outputs


def wddm_peaks(path):
    if not path.is_file():
        return {}
    with path.open(encoding="utf-8-sig", errors="replace", newline="") as file:
        rows = csv.reader(file)
        header = next(rows, [])
        peaks = {}
        for row in rows:
            for name, value in zip(header[1:], row[1:]):
                try:
                    peaks[name] = max(peaks.get(name, 0), int(float(value)))
                except ValueError:
                    pass
        return peaks


def staged_input(args, name, indices, *, renumber=False):
    folder = args.work / "inputs" / name
    folder.mkdir(parents=True)
    for slot, index in enumerate(indices):
        source = args.sources[index]
        number = slot if renumber else index
        target = folder / f"{number:08d}{source.suffix}"
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
    parser.add_argument("--study", choices=("devices", "pipeline", "hybrid", "weights", "cutlass", "heterogeneous", "dual", "dual-dispatch"),
                        default="devices")
    parser.add_argument("--candidate-exe", type=lambda path: Path(path).resolve(),
                        help="candidate CLI in a separate directory with its own runtime (cutlass or dual-dispatch)")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--repeat-to", type=int, metavar="COUNT",
                        help="repeat the sorted source set to COUNT items for the dual-dispatch throughput study")
    parser.add_argument("--size", default="180x320")
    parser.add_argument("--gpu", default="0", help="nvidia-smi index/UUID of the only visible GPU")
    parser.add_argument("--vulkan-device", type=int, default=1, help="device index from the ncnn Vulkan probe")
    parser.add_argument("--with-dual", action="store_true", help="include the dual-GPU candidate after single-GPU correctness")
    parser.add_argument("--timeout", type=int, default=3600, help="seconds per run")
    args = parser.parse_args()
    if not args.exe.is_file() or not args.input.is_dir() or shutil.which("nvidia-smi") is None:
        parser.error("CLI, input directory and nvidia-smi must exist")
    if args.study in {"cutlass", "dual-dispatch"} and (
            os.name != "nt" or not args.candidate_exe or args.exe.parent == args.candidate_exe.parent
            or any(not exe.is_file() or not exe.with_name("opencv_dnn5.dll").is_file()
                   for exe in (args.exe, args.candidate_exe))):
        parser.error(f"{args.study} requires two Windows CLI directories, each with its own opencv_dnn5.dll")
    dnn = args.exe.with_name("opencv_dnn5.dll")
    if args.study == "weights" and (os.name != "nt" or not dnn.is_file()):
        parser.error("the weights study requires a Windows CUDA CLI beside the experimental opencv_dnn5.dll")
    if args.work.is_relative_to(args.input) or args.input.is_relative_to(args.work):
        parser.error("work and input must not overlap")
    def input_files(root):
        return sorted((p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS),
                      key=lambda p: p.relative_to(root).parts)

    def input_hashes(root):
        return {p.relative_to(root).as_posix(): sha256(p) for p in input_files(root)}

    if args.study == "dual-dispatch" and args.mode != "smart-real":
        parser.error("the dual-GPU experiment only supports smart-real")
    original_input = args.input
    args.sources = input_files(original_input)
    if args.repeat_to is not None and (
            args.study != "dual-dispatch" or args.repeat_to < max(len(args.sources), 8)):
        parser.error("--repeat-to requires dual-dispatch and a count at least as large as the source set and eight")
    if args.rounds < 1 or len(args.sources) < (1 if args.repeat_to is not None or args.study == "devices" else 8):
        parser.error("positive round count and enough supported images are required")
    args.gpu_uuid = subprocess.check_output(nvidia_query(args.gpu, "uuid"), text=True).strip()
    args.work.mkdir(parents=True, exist_ok=False)

    repeat_input = None
    if args.repeat_to is not None:
        original_hashes = input_hashes(original_input)
        count = len(args.sources)
        args.input = staged_input(args, "repeat", (i % count for i in range(args.repeat_to)), renumber=True)
        args.sources = input_files(args.input)
        repeat_input = {"sourceCount": count, "itemCount": len(args.sources), "sourceSha256": original_hashes}
    source_hashes = input_hashes(args.input)
    report = {"executableSha256": sha256(args.exe), "mode": args.mode, "size": args.size, "study": args.study,
              "gpu": subprocess.check_output(nvidia_query(args.gpu, "name,driver_version,memory.total"), text=True).strip(),
              "inputSha256": source_hashes, "runs": []}
    if repeat_input is not None:
        report["repeatInput"] = repeat_input
    if args.study == "hybrid":
        variants = hybrid_variants(args, report)
    elif args.study == "pipeline":
        variants = {label: [("cuda", pipeline, args.input, None)]
                    for label, pipeline in (("serial", False), ("pipeline", True))}
    elif args.study == "weights":
        report["opencvDnnSha256"] = sha256(dnn)
        variants = {label: [("cuda", True, args.input, None)] for label in ("pipeline", "reuse-weights")}
    elif args.study in {"cutlass", "dual-dispatch"}:
        report["candidateExecutableSha256"] = sha256(args.candidate_exe)
        report["opencvDnnSha256ByVariant"] = {
            label: sha256(exe.with_name("opencv_dnn5.dll"))
            for label, exe in (("baseline", args.exe), ("cutlass", args.candidate_exe))}
        if args.study == "cutlass":
            variants = {label: [("cuda", True, args.input, None)] for label in ("baseline", "cutlass")}
        else:
            if len(set(report["opencvDnnSha256ByVariant"].values())) != 1:
                parser.error("dual-dispatch requires the same OpenCV DNN build")
            variants = {label: [(device, True, args.input, None)]
                        for label, device in (("cuda", "cuda"), ("dual-ordered", "dual"), ("dual", "dual"))}
            report["vulkanDeviceIndex"] = args.vulkan_device
    elif args.study == "dual":
        if os.name != "nt":
            parser.error("the dual study requires Windows")
        variants = {label: [(label, True, args.input, None)] for label in ("cuda", "dual")}
        report["vulkanDeviceIndex"] = args.vulkan_device
    elif args.study == "heterogeneous":
        if os.name != "nt":
            parser.error("the heterogeneous study requires Windows")
        variants = {label: [(label, label != "cpu", args.input, None)]
                    for label in ("cpu", "cuda", "vulkan", "dual")
                    if label != "dual" or args.with_dual}
        report["vulkanDeviceIndex"] = args.vulkan_device
    else:
        variants = {device: [(device, False, args.input, None)] for device in ("cpu", "cuda")}
    labels = list(variants)
    reference = None
    reference_rois = None
    report_path = args.work / "report.json"
    reference_by_device = {}
    for cycle in range(args.rounds):
        offset = cycle % len(labels)
        for label in labels[offset:] + labels[:offset]:
            name = f"{cycle + 1}-{label}"
            reuse_weights = label == "reuse-weights" if args.study == "weights" else None
            use_candidate = label == "cutlass" or (args.study == "dual-dispatch" and label != "dual-ordered")
            result, outputs = run(args, name, variants[label], reuse_weights=reuse_weights,
                                  executable=args.candidate_exe if use_candidate else None)
            if args.study in {"dual", "dual-dispatch"}:
                startup = result["children"][0]["metrics"].get("smartCropStartupMs")
                if startup is not None and 0 <= startup <= result["wallSeconds"] * 1000:
                    result["modelPreflightSeconds"] = startup / 1000
                    result["wallMinusModelPreflightSeconds"] = result["wallSeconds"] - startup / 1000
            if reference is None:
                reference = outputs
                report["referenceOutputSha256"] = reference
                reference_rois = result.get("cropRois")
            result.update(variant=label, round=cycle + 1, identical=outputs == reference,
                          differingOutputs=sorted(k for k in reference.keys() | outputs.keys()
                                                  if reference.get(k) != outputs.get(k)))
            if args.study in {"cutlass", "heterogeneous", "dual", "dual-dispatch"}:
                result["identicalCropRois"] = result["cropRois"] == reference_rois
                result["identical"] &= result["identicalCropRois"]
                result["differingOutputSha256"] = {key: outputs.get(key) for key in result["differingOutputs"]}
            if args.study == "heterogeneous":
                if cycle == 0 and label in {"cpu", "cuda"}:
                    reference_by_device[label] = outputs, result["cropRois"]
                basis = "cuda" if label in {"cuda", "dual"} else "cpu"
                expected_outputs, expected_rois = reference_by_device[basis]
                different = sorted(k for k in expected_outputs.keys() | outputs.keys()
                                   if expected_outputs.get(k) != outputs.get(k))
                result.update(referenceVariant=basis, identical=not different and result["cropRois"] == expected_rois,
                              identicalCropRois=result["cropRois"] == expected_rois, differingOutputs=different,
                              differingOutputSha256={key: outputs.get(key) for key in different})
            report["runs"].append(result)
            print(f"{name}: {result['wallSeconds']:.2f}s, run OK={result['passed']}, identical={result['identical']}", flush=True)
            write_report(report_path, report)
    report["inputUnchanged"] = input_hashes(args.input) == source_hashes
    if repeat_input is not None:
        repeat_input["sourceUnchanged"] = input_hashes(original_input) == repeat_input["sourceSha256"]
        report["inputUnchanged"] &= repeat_input["sourceUnchanged"]
    grouped = {label: sorted((r for r in report["runs"] if r["variant"] == label), key=lambda r: r["round"])
               for label in labels}
    medians = {label: statistics.median(r["wallSeconds"] for r in runs) for label, runs in grouped.items()}
    report["medianWallSeconds"] = medians
    report["medianImagesPerSecond"] = {label: statistics.median(r["imagesPerSecond"] for r in runs)
                                       for label, runs in grouped.items()}
    report["wallSavings"] = {label: 1 - medians[label] / medians[labels[0]] for label in labels[1:]}
    report["speedup"] = medians[labels[0]] / medians[labels[1]]
    if args.study in {"pipeline", "hybrid", "weights", "cutlass", "heterogeneous", "dual", "dual-dispatch"}:
        def working_set_deltas(label, base=labels[0]):
            pairs = zip(grouped[base], grouped[label])
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
        elif args.study == "cutlass":
            vram = [((b["gpuSampledPeakMiB"] - b["gpuBaselineMiB"]) -
                     (a["gpuSampledPeakMiB"] - a["gpuBaselineMiB"]))
                    if a["gpuSampledPeakMiB"] is not None and b["gpuSampledPeakMiB"] is not None else None
                    for a, b in zip(grouped["baseline"], grouped["cutlass"])]
            report["sampledVramIncreaseMiB"] = vram
            gates = ("cutlass" in acceptable and medians["cutlass"] <= medians["baseline"] * 1.10
                     and all(delta is not None and delta <= 512 for delta in vram))
        elif args.study in {"dual", "dual-dispatch"}:
            overlap = [run["children"][0]["metrics"].get("smartCropDualOverlapMs", 0) for run in grouped["dual"]]
            report["dualOverlapMs"] = overlap
            for field, key in (("modelPreflightSeconds", "medianModelPreflightSeconds"),
                               ("wallMinusModelPreflightSeconds", "medianWallMinusModelPreflightSeconds")):
                report[key] = {label: statistics.median(r[field] for r in runs)
                               if all(field in r for r in runs) else None for label, runs in grouped.items()}
            if args.study == "dual-dispatch":
                report["gainVsOrderedDual"] = 1 - medians["dual"] / medians["dual-ordered"]
            gates = ("dual" in acceptable and all(value > 0 for value in overlap)
                     and report["wallSavings"]["dual"] >= .05)
        elif args.study == "heterogeneous":
            correct = {label: all(r["passed"] and r["identical"] for r in runs) for label, runs in grouped.items()}
            vulkan_gain = report["wallSavings"]["vulkan"]
            vulkan_pass = correct["cpu"] and "vulkan" in acceptable and vulkan_gain >= .10
            report["vulkanCorrect"] = correct["cpu"] and correct["vulkan"]
            report["vulkanCandidate"] = vulkan_pass
            gates = vulkan_pass
            if "dual" in grouped:
                eligible = [label for label in ("cpu", "cuda", "vulkan") if correct[label] and (label != "vulkan" or vulkan_pass)]
                fastest = min(eligible, key=medians.get, default="cuda")
                growth = working_set_deltas("dual", fastest)
                gain = 1 - medians["dual"] / medians[fastest]
                report["dualReference"] = fastest
                report["dualWallSavings"] = gain
                report["dualWorkingSetIncreaseBytes"] = growth
                report["dualCandidate"] = (report["vulkanCorrect"] and correct["cuda"] and correct["dual"] and gain >= .05
                                           and all(d is not None and d <= 512 * 1024**2 for d in growth))
                gates = gates or report["dualCandidate"]
        else:
            # For multi-process runs, the sum of both individual peaks is a conservative ceiling.
            report["hybridCandidates"] = [label for label in acceptable if report["wallSavings"][label] >= .05]
            gates = bool(report["hybridCandidates"]) and all(r["passed"] and r["identical"] for r in grouped["pipeline"])
    else:
        gates = report["speedup"] >= 1.5
    if args.study not in {"hybrid", "heterogeneous"}:
        gates &= all(r["passed"] and (args.study == "devices" or r["identical"]) for r in report["runs"])
    report["passed"] = report["inputUnchanged"] and gates
    write_report(report_path, report)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
