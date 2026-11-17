#!/usr/bin/env python3
"""Compare compute backends with balanced, sequential, repeated full tasks."""
import argparse
import array
from contextlib import closing
from datetime import datetime, timezone
import json
from pathlib import Path
import random
import sqlite3
import statistics
import subprocess
import time

from study_paths import digest, private_library, readonly_database


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def run(command, log):
    with log.open("w", encoding="utf-8") as stream:
        start = time.perf_counter()
        result = subprocess.run([str(value) for value in command], stdout=stream, stderr=subprocess.STDOUT)
        elapsed = (time.perf_counter() - start) * 1000
    if result.returncode:
        raise RuntimeError(f"{log}: exit {result.returncode}\n{log.read_text(encoding='utf-8')[-3000:]}")
    return elapsed


def percentile(values, fraction):
    values = sorted(values)
    index = (len(values) - 1) * fraction
    lower = int(index)
    return values[lower] + (values[min(lower + 1, len(values) - 1)] - values[lower]) * (index - lower)


def describe(values):
    median = statistics.median(values)
    return dict(n=len(values), median=median, mean=statistics.mean(values),
                min=min(values), max=max(values), p10=percentile(values, 0.1),
                p90=percentile(values, 0.9), stdev=statistics.stdev(values),
                mad=statistics.median(abs(value - median) for value in values))


def balanced_orders(count, rounds, rng):
    # Williams designs balance position and preceding backend within each block.
    # Odd backend counts need the reversed sequences as well.
    base = [0] + [i // 2 + 1 if i % 2 else count - i // 2 for i in range(1, count)]
    block = [[(value + offset) % count for value in base] for offset in range(count)]
    if count % 2:
        block += [list(reversed(row)) for row in block]
    result = []
    while len(result) < rounds:
        labels = list(range(count))
        rng.shuffle(labels)
        rows = [[labels[value] for value in row] for row in block]
        rng.shuffle(rows)
        result.extend(rows)
    return result[:rounds]


def summarize(report):
    measured = [row for row in report["samples"] if row["phase"] == "measured"]
    metrics = ("matchingMs", "totalMs", "processWallMs")
    result = {}
    rng = random.Random(20261118)
    reference = report["reference"]
    for case in report["cases"]:
        groups = {variant["id"]: sorted(
            (row for row in measured if row["case"] == case["id"] and row["variant"] == variant["id"]),
            key=lambda row: row["round"]) for variant in report["variants"]}
        result[case["id"]] = {}
        for name, rows in groups.items():
            summary = {metric: describe([row["metrics"][metric] for row in rows]) for metric in metrics}
            if name != reference:
                comparison = {}
                for metric in metrics:
                    ratios = [row["metrics"][metric] / baseline["metrics"][metric]
                              for row, baseline in zip(rows, groups[reference])]
                    # Pair within rounds so slow/fast periods affect both sides.
                    boot = [statistics.median(rng.choices(ratios, k=len(ratios))) for _ in range(10000)]
                    comparison[metric] = dict(median_ratio=statistics.median(ratios),
                        bootstrap95=[percentile(boot, 0.025), percentile(boot, 0.975)],
                        faster_rounds=sum(value < 1 for value in ratios), rounds=len(ratios))
                summary["relative_to_reference"] = comparison
            result[case["id"]][name] = summary
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--benchmark", type=Path, required=True)
    parser.add_argument("--quality", type=Path, required=True, help="existing cache preparation tool")
    parser.add_argument("--output", type=Path, required=True, help="new, disposable result directory")
    parser.add_argument("--rounds", type=int, default=24)
    parser.add_argument("--warmups", type=int, default=3)
    options = parser.parse_args()
    specification = json.loads(options.manifest.read_text(encoding="utf-8-sig"))
    variants = specification["variants"]
    block_size = len(variants) * (2 if len(variants) % 2 else 1)
    if len(variants) < 2 or options.rounds < 2 or options.rounds % block_size or options.warmups < 1:
        parser.error("use >=2 variants, >=1 warmup and measured rounds divisible by the balanced block size")
    for items in (variants, specification["cases"]):
        names = [item["id"] for item in items]
        if len(set(names)) != len(names) or any(not name or any(c not in
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in name) for name in names):
            parser.error("case/variant ids must be unique safe filenames")
    reference = specification.get("reference", "cpu")
    if reference not in [variant["id"] for variant in variants]:
        parser.error("reference must name a variant")
    base = options.manifest.resolve().parent
    source = (base / specification["source"]).resolve()
    cases = [dict(item, input=str((base / item["input"]).resolve())) for item in specification["cases"]]
    root = options.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    benchmark, quality = options.benchmark.resolve(), options.quality.resolve()
    report = dict(complete=False, started_utc=datetime.now(timezone.utc).isoformat(),
        source_sha256=digest(source), benchmark_sha256=digest(benchmark), quality_sha256=digest(quality),
        source_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=Path(__file__).resolve().parents[1], text=True).strip(),
        environment=specification.get("environment", {}), cases=[dict(item, sha256=digest(item["input"])) for item in cases],
        variants=variants, reference=reference, rounds=options.rounds, warmups=options.warmups,
        common_request=specification.get("request", {}), samples=[],
        method="Fresh process per task; warm filesystem/driver caches; database restored before each run; "
               "balanced backend order; all samples retained; no resource polling inside timings; "
               "paired-round bootstrap percentile CI (10000 resamples); not a cold-disk or energy benchmark.")
    save(root / "report.json", report)
    print("Preparing isolated library and caches...", flush=True)
    snapshot, count = private_library(source, root / "library", images=True)
    report["library_rows"] = count
    run([quality, "--prepare-cache", snapshot], root / "prepare-cache.log")
    database = snapshot.with_name("run.db")
    first_selections = {}
    rng = random.Random(20261118)
    plans = {case["id"]: balanced_orders(len(variants), options.rounds, rng) for case in cases}
    report["orders"] = plans
    for phase, rounds in (("warmup", options.warmups), ("measured", options.rounds)):
        for iteration in range(rounds):
            # Alternate case order as well; each sample remains strictly serial.
            for case in (cases if iteration % 2 == 0 else list(reversed(cases))):
                order = plans[case["id"]][iteration % options.rounds]
                for position, variant_index in enumerate(order):
                    variant = variants[variant_index]
                    tag = f"{phase}-{iteration:02d}-{case['id']}-{variant['id']}"
                    with readonly_database(snapshot) as original, closing(sqlite3.connect(database)) as target:
                        original.backup(target)
                    output = root / (tag + ".png")
                    request = dict(report["common_request"], inputPath=case["input"], dbPath=str(database),
                        outputPath=str(output), outW=case["width"], outH=case["height"],
                        backend=variant["backend"], device=variant["device"], scoreMode=variant["scoreMode"],
                        resourceMonitor=False, overwrite=True, format="png")
                    seed = request.get("seed", 42)
                    request_path = root / (tag + ".request.json")
                    summary_path = root / (tag + ".summary.json")
                    save(request_path, request)
                    wall = run([benchmark, request_path, "--seed", seed, "--selections", "--summary", summary_path],
                               root / (tag + ".log"))
                    summary = json.loads(summary_path.read_text(encoding="utf-8"))
                    selection_path = Path(str(output) + ".indices.bin")
                    selected = array.array("i")
                    selected.frombytes(selection_path.read_bytes())
                    expected = ((case["width"] + request.get("tileW", 9) - 1) // request.get("tileW", 9) *
                                ((case["height"] + request.get("tileH", 16) - 1) // request.get("tileH", 16)))
                    if selected.itemsize != 4 or len(selected) != expected or summary["failed"] or summary["warnings"] or any(
                            index < 0 or index >= count for index in selected) or not output.is_file():
                        raise RuntimeError(f"{tag}: incomplete output, warning or invalid selections")
                    backend = variant["backend"]
                    expected_score = "cpu" if backend == "cpu" or (backend == "vulkan" and variant["scoreMode"] == "compatible") else backend
                    if backend not in summary["feature_backend"] or expected_score not in summary["score_backend"]:
                        raise RuntimeError(f"{tag}: requested backend did not run")
                    key = (case["id"], variant["id"])
                    hashes = (digest(selection_path), digest(output))
                    if key in first_selections and first_selections[key]["hashes"] != hashes:
                        raise RuntimeError(f"{tag}: fixed-input selection/output changed between repetitions")
                    first_selections.setdefault(key, dict(hashes=hashes, indices=list(selected)))
                    report["samples"].append(dict(phase=phase, round=iteration, position=position,
                        case=case["id"], variant=variant["id"], metrics=dict(summary["metrics"], processWallMs=wall),
                        feature_backend=summary["feature_backend"], score_backend=summary["score_backend"],
                        device=summary["device"], selected_count=len(selected), selection_sha256=hashes[0],
                        output_sha256=hashes[1], log=tag + ".log"))
                save(root / "report.json", report)
                last = report["samples"][-len(variants):]
                print(f"{phase} {iteration + 1}/{rounds} {case['id']}: " + ", ".join(
                    f"{row['variant']}={row['metrics']['totalMs']:.0f}ms" for row in last), flush=True)
    report["selection_differences"] = {}
    for case in cases:
        baseline = first_selections[(case["id"], reference)]["indices"]
        report["selection_differences"][case["id"]] = {variant["id"]: sum(a != b for a, b in zip(
            baseline, first_selections[(case["id"], variant["id"])]["indices"])) for variant in variants}
    report["source_unchanged"] = digest(source) == report["source_sha256"]
    if not report["source_unchanged"]:
        raise RuntimeError("source database changed during benchmark")
    report["summary"] = summarize(report)
    report["complete"] = True
    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    save(root / "report.json", report)
    print(f"Complete: {root / 'report.json'}", flush=True)


if __name__ == "__main__":
    main()
