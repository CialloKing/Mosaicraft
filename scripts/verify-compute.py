#!/usr/bin/env python3
"""Run fixed-seed GPU gates on a private library, using the existing benchmarks."""
import argparse
import array
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

from study_paths import private_library, readonly_database


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def run(command, log):
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run([str(item) for item in command], stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"{log}: exit {result.returncode}\n{log.read_text(encoding='utf-8')[-3000:]}")


def selections(path):
    values = array.array("i")
    values.frombytes(path.read_bytes())
    if values.itemsize != 4:
        raise RuntimeError("selection reader requires 32-bit int")
    return values.tolist()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="source library, cases, devices; no source writes")
    parser.add_argument("--benchmark", type=Path, required=True)
    parser.add_argument("--quality", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--performance", action="store_true", help="cold + warmup + five alternating end-to-end pairs")
    parser.add_argument("--performance-case", help="case id for end-to-end timing; defaults to the first case")
    parser.add_argument("--baseline-benchmark", type=Path, help="original CUDA benchmark for the 5%% regression gate")
    options = parser.parse_args()
    specification = json.loads(options.manifest.read_text(encoding="utf-8"))
    root = options.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    source = Path(specification["source"]).resolve()
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    snapshot, count = private_library(source, root / "library", options.performance)
    run([options.quality.resolve(), "--prepare-cache", snapshot], root / "prepare-cache.log")
    with readonly_database(source) as original:
        usage = dict(original.execute("SELECT id,use_count FROM images"))
    with readonly_database(snapshot) as database:
        ids = [row[0] for row in database.execute("SELECT id FROM images ORDER BY id")]
    study = {"snapshot": str(snapshot), "cases": specification["cases"], "runs": [], "oracle": False}
    report = {"source_sha256": source_hash, "library_rows": count, "quality_limit": 0.01,
              "selection_limit": 0.01, "gates": [], "performance": [], "complete": False,
              "benchmark_sha256": hashlib.sha256(options.benchmark.read_bytes()).hexdigest(),
              "quality_sha256": hashlib.sha256(options.quality.read_bytes()).hexdigest()}

    def match(case, device, mode, seed, top, tag, full=False, benchmark=None):
        request = dict(inputPath=str(Path(case["input"]).resolve()), dbPath=str(snapshot),
                       outputPath=str(root / (tag + ".png")), backend=device.split(":")[0],
                       device=device, scoreMode=mode, tileW=9, tileH=16, outW=case["width"], outH=case["height"],
                       nativeTileW=9, nativeTileH=16, lRange=255, candidates=150, threads=8,
                       topNRandom=top, seed=seed, format="png", overwrite=True, resourceMonitor=True)
        if benchmark is not None:
            # API 1.5 以前的基线不认识设备和评分模式字段，计算参数仍保持相同。
            request.pop("device")
            request.pop("scoreMode")
        # Each process sees exactly the same use counts, including end-to-end runs.
        import sqlite3
        with sqlite3.connect(snapshot) as database:
            database.executemany("UPDATE images SET use_count=? WHERE id=?", ((value, key) for key, value in usage.items()))
        path = root / (tag + ".json")
        save(path, request)
        command = [(benchmark or options.benchmark).resolve(), path, "--seed", seed,
                   "--summary", root / (tag + ".summary.json")]
        command += ["--selections"] if full else ["--match-only"]
        run(command, root / (tag + ".log"))
        summary = json.loads((root / (tag + ".summary.json")).read_text(encoding="utf-8"))
        selected_path = root / (tag + ".png.indices.bin")
        selected = selections(selected_path)
        if summary["failed"] or any(index < 0 or index >= len(ids) for index in selected):
            raise RuntimeError(f"{tag}: unmatched/invalid selections")
        if device.split(":")[0] in ("vulkan", "metal") and (summary["feature_backend"] != device.split(":")[0] or
                                summary["score_backend"] != (device.split(":")[0] if mode == "fast" else "cpu")):
            raise RuntimeError(f"{tag}: requested GPU path did not run")
        if device == "cuda:0" and any("cuda" not in summary[key] for key in ("feature_backend", "score_backend")):
            raise RuntimeError(f"{tag}: CUDA regression must not measure a CPU fallback")
        return summary, selected, selected_path

    for case in specification["cases"]:
        for seed in specification.get("seeds", [42, 20261113]):
            for top in specification.get("top_n", [1, 10]):
                reference_tag = f"{case['id']}-{seed}-{top}-cpu"
                _, reference, path = match(case, "cpu", "compatible", seed, top, reference_tag)
                study["runs"].append(dict(id=reference_tag, case=case["id"], selection=str(path),
                                          selection_sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
                for device in specification["devices"]:
                    for mode in ("compatible", "fast"):
                        tag = f"{case['id']}-{seed}-{top}-{device.replace(':', '-')}-{mode}"
                        summary, selected, path = match(case, device, mode, seed, top, tag)
                        if len(selected) != len(reference):
                            raise RuntimeError(f"{tag}: grid length mismatch")
                        changed = [i for i, (a, b) in enumerate(zip(reference, selected)) if ids[a] != ids[b]]
                        limit = len(reference) // 100 if mode == "fast" else 0
                        report["gates"].append(dict(id=tag, reference=reference_tag, mode=mode, device=device,
                            tiles=len(reference), different=len(changed), allowed=limit, positions=changed,
                            selection_passed=len(changed) <= limit, metrics=summary["metrics"]))
                        study["runs"].append(dict(id=tag, case=case["id"], selection=str(path),
                                                  selection_sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
                        print(tag, f"{len(changed)}/{len(reference)}", flush=True)
    save(root / "study.json", study)
    run([options.quality.resolve(), root / "study.json", root / "evaluation.json"], root / "evaluation.log")
    evaluation = json.loads((root / "evaluation.json").read_text(encoding="utf-8"))
    if not evaluation.get("complete"):
        raise RuntimeError("incomplete quality evaluation")
    qualities = {entry["id"]: entry["quality"] for entry in evaluation["runs"]}
    for gate in report["gates"]:
        baseline, current = qualities[gate["reference"]], qualities[gate["id"]]
        gate["reference_quality"], gate["quality"] = baseline, current
        gate["quality_passed"] = all(current[key] <= baseline[key] * 1.01 + 1e-6 for key in ("mean", "p90", "p99"))
        gate["passed"] = gate["selection_passed"] and gate["quality_passed"] and current["missing"] == baseline["missing"] == 0
    save(root / "acceptance.json", report)
    if options.performance:
        case = next((item for item in specification["cases"] if item["id"] == options.performance_case),
                    specification["cases"][0] if options.performance_case is None else None)
        if case is None:
            raise RuntimeError(f"unknown performance case: {options.performance_case}")
        for device in specification["devices"]:
            samples = {"cpu": [], device: []}
            cold = {}
            for iteration in range(7):
                order = ["cpu", device] if iteration % 2 == 0 else [device, "cpu"]
                for backend in order:
                    tag = f"perf-{device.replace(':', '-')}-{iteration}-{backend.replace(':', '-')}"
                    summary, _, _ = match(case, backend, "fast" if backend != "cpu" else "compatible", 42, 10, tag, True)
                    if iteration == 0:
                        cold[backend] = summary["metrics"]
                    elif iteration > 1:
                        samples[backend].append(summary["metrics"])
            medians = {key: {metric: statistics.median(row[metric] for row in rows)
                            for metric in ("matchingMs", "totalMs")} for key, rows in samples.items()}
            report["performance"].append(dict(device=device, case=case["id"], cold=cold, samples=samples, medians=medians,
                auto_eligible=all(gate["passed"] for gate in report["gates"] if gate["device"] == device) and
                medians[device]["matchingMs"] <= medians["cpu"]["matchingMs"] * 0.90 and
                medians[device]["totalMs"] <= medians["cpu"]["totalMs"] * 1.05))
        if options.baseline_benchmark:
            samples = {"baseline": [], "current": []}
            cold = {}
            for iteration in range(7):
                for variant in (["baseline", "current"] if iteration % 2 == 0 else ["current", "baseline"]):
                    summary, _, _ = match(case, "cuda:0", "compatible", 42, 10,
                        f"cuda-{iteration}-{variant}", True,
                        options.baseline_benchmark if variant == "baseline" else None)
                    if iteration == 0:
                        cold[variant] = summary["metrics"]
                    elif iteration > 1:
                        samples[variant].append(summary["metrics"])
            medians = {key: {metric: statistics.median(row[metric] for row in rows)
                            for metric in ("matchingMs", "totalMs")} for key, rows in samples.items()}
            report["cuda_regression"] = dict(case=case["id"], cold=cold, samples=samples, medians=medians,
                baseline_sha256=hashlib.sha256(options.baseline_benchmark.read_bytes()).hexdigest(),
                passed=all(medians["current"][metric] <= medians["baseline"][metric] * 1.05
                           for metric in ("matchingMs", "totalMs")))
    if hashlib.sha256(source.read_bytes()).hexdigest() != source_hash:
        raise RuntimeError("source library changed during acceptance")
    report["complete"] = True
    report["passed"] = all(gate["passed"] for gate in report["gates"]) and report.get("cuda_regression", {}).get("passed", True)
    save(root / "acceptance.json", report)
    if not report["passed"]:
        raise SystemExit("GPU acceptance gate failed; see acceptance.json")


if __name__ == "__main__":
    main()
