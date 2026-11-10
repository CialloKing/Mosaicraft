"""Selection-only ablation. Real-library photographs are never decoded or exported.

Reuses the first quality study's inputs, feature snapshot and evaluator. Full-image
comparisons use a deterministic procedural library, independently of that snapshot.
"""
import argparse
from contextlib import closing
import collections
import hashlib
import json
import platform
import shutil
import sqlite3
import statistics
import subprocess
import sys
import time
import zipfile
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw

from benchmark import gpu_memory, peak_rss
from study_paths import REPO, configure, bind_database, private_library
from quality_study import median_available, gpu_description
from public_paths import public_report
from quality_study import (SEEDS, command_output, digest, gray_ssim, load,
                           read_image, readonly_database, save, source_inventory)

STRATEGIES = ("L", "Q", "D", "QD", "QDX")
BACKENDS = ("cpu", "cuda")


def validate_configuration(manifest, binaries):
    if tuple(manifest["backends"]) != BACKENDS:
        raise RuntimeError("Backend configuration changed; use a fresh study directory")
    if {k:v["sha256"] for k,v in manifest["executables"].items()} != {k:v["sha256"] for k,v in binaries.items()}:
        raise RuntimeError("Binary changed; use a fresh study directory")


def prepare(args):
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    path = root / "manifest.json"
    if path.exists():
        manifest = load(path)
        validate_configuration(manifest, executables(args))
        return manifest
    previous = load(args.baseline_study / "manifest.json")
    source = args.database or Path(previous["source_database"])
    bind_database(source)
    subprocess.run([str(args.quality), "--open-library", str(source)], check=True)
    if args.database is None and digest(source) != previous["source_database_sha256"]:
        raise RuntimeError("Original library differs from the fixed baseline")
    snapshot, row_count = private_library(source, root, images=False)
    inputs = root / "inputs"
    inputs.mkdir(exist_ok=True)
    cases = []
    for case in previous["cases"]:
        target = inputs / f"{case['id']}.png"
        shutil.copy2((args.inputs / Path(case["input"]).name) if args.inputs else case["input"], target)
        cases.append(dict(case, input=str(target), sha256=digest(target)))
    manifest = dict(schema=1, source_commit=command_output(["git", "rev-parse", "HEAD"]),
                    executables=executables(args), snapshot=str(snapshot), snapshot_sha256=digest(snapshot),
                    previous_snapshot_sha256=digest(args.baseline_study / "snapshot.db"),
                    source_database=str(source), source_database_sha256=digest(source),
                    source_inventory=source_inventory(source), library_rows=row_count, cases=cases,
                    seeds=SEEDS, backends=BACKENDS, strategies=STRATEGIES, flags=previous["flags"],
                    matching_only=True, baseline_study=str(args.baseline_study.resolve()),
                    environment=dict(gpu=gpu_description(),
                                     platform=platform.platform(), python=sys.version, python_opencv=cv2.__version__),
                    script_sha256=digest(__file__),
                    memory_metric="PeakWorkingSet; GPU sampling covers the whole device, not this process alone")
    with (root / "prepare.log").open("w", encoding="utf-8") as log:
        subprocess.run([str(args.quality), "--prepare-cache", str(snapshot)], stdout=log, stderr=log, check=True)
    save(path, manifest)
    return manifest


def executables(args):
    return {name: dict(path=str(getattr(args, name)), sha256=digest(getattr(args, name)))
            for name in ("match", "quality", "cli")}


def audit(selected, rows, strategy, columns):
    if strategy == "L":
        return {}
    if len(selected) != len(rows):
        raise RuntimeError("Diagnostic length differs from selection length")
    for tile, d in enumerate(rows):
        if selected[tile] != d["selected"]:
            raise RuntimeError("Diagnostic selection differs from capture")
        if strategy in ("Q", "QD", "QDX") and d["error"] > d["limit"]:
            raise RuntimeError("Quality bound violated")
        if strategy in ("D", "QD", "QDX"):
            radius = d["radius"]
            x, y = tile % columns, tile // columns
            if not 0 <= radius <= 3 or (radius < 3 and d["radius3_candidates"] != 0):
                raise RuntimeError("Unexplained radius fallback")
            for dy in range(-radius, 1):
                for dx in range(-radius, radius + 1):
                    nx, ny = x + dx, y + dy
                    other = ny * columns + nx
                    if dx * dx + dy * dy <= radius * radius and 0 <= nx < columns and ny >= 0 and other < tile:
                        if selected[other] == selected[tile]:
                            raise RuntimeError("Chosen spatial radius violated")
        if strategy == "QDX":
            if d["initial_radius3_candidates"] >= 3 and d["expansion_steps"]:
                raise RuntimeError("Unnecessary expansion")
            if d["initial_radius3_candidates"] < 3 and not d["expansion_steps"]:
                raise RuntimeError("Expansion was skipped")
            if d["queried"] < 600 and d["radius3_candidates"] < 3:
                raise RuntimeError("Expansion stopped prematurely")
    expanded = [d for d in rows if d["expansion_steps"]]
    return dict(quality_violations=0, spatial_violations=0,
                radius_counts=dict(collections.Counter(d["radius"] for d in rows)),
                similar_fallback_count=sum(d["similar_fallback"] for d in rows),
                quality_candidates_mean=statistics.mean(d["quality_candidates"] for d in rows),
                quality_candidates_min=min(d["quality_candidates"] for d in rows),
                expanded_count=len(expanded), expanded_fraction=len(expanded) / len(rows),
                expansion_ms=sum(d["expansion_ms"] for d in rows),
                added_candidates=sum(d["candidates"] - d["initial_candidates"] for d in rows),
                no_new_candidates=sum(d["candidates"] == d["initial_candidates"] for d in expanded),
                still_scarce_count=sum(d["radius3_candidates"] < 3 for d in expanded),
                expansion_best_error_improvement=sum(d["initial_minimum"] - d["minimum"] for d in expanded),
                queried_counts=dict(collections.Counter(d["queried"] for d in rows)))


def execute(args, manifest, case, strategy, seed, backend, tag=None, epsilon=0.05, batch=1024, render=False):
    root = args.output_dir
    tag = tag or f"{case['id']}-{strategy}-{seed}-{backend}"
    folder = root / "runs" / tag
    folder.mkdir(parents=True, exist_ok=True)
    output = folder / ("mosaic.png" if render else "selection")
    request = dict(input=case["input"], db=str(Path(manifest["snapshot"]).parent / "run.db"), output=str(output),
                   outW=case["width"], outH=case["height"], candidates=150, topNrandom=10,
                   cpu=backend == "cpu", format="png", writeMode="stream", **manifest["flags"])
    request["lRange"] = case.get("lRange", 20)
    identity = dict(request=request, seed=seed, strategy=strategy, epsilon=epsilon,
                    batch=batch, render=render, executables=manifest["executables"],
                    snapshot=manifest["snapshot_sha256"], input_sha256=digest(case["input"]))
    saved = folder / "run.json"
    if saved.exists():
        result = load(saved)
        if (result["identity"] == identity and digest(result["selection"]) == result["selection_sha256"] and
                digest(result["diagnostics"]) == result["diagnostics_sha256"] and
                (not render or digest(output) == result["output_sha256"])):
            return result
        raise RuntimeError(f"Stale checkpoint: {tag}; use a fresh directory")
    for suffix in ("-wal", "-shm", "-journal"):
        Path(request["db"] + suffix).unlink(missing_ok=True)
    shutil.copy2(manifest["snapshot"], request["db"])
    save(folder / "request.json", request)
    command = [str(args.match), str(folder / "request.json"), "--seed", str(seed),
               "--strategy", strategy, "--epsilon", str(epsilon), "--batch-tiles", str(batch),
               "--selections", "--summary", str(folder / "runtime.json"),
               "--diagnostics", str(folder / "diagnostics.json")]
    if not render:
        command.append("--match-only")
    gpu = gpu_memory()
    gpu_peak, rss, last_gpu = gpu, 0, 0.0
    start = time.perf_counter()
    with (folder / "console.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        while process.poll() is None:
            rss = max(rss, peak_rss(process) or 0)
            now = time.perf_counter()
            if now - last_gpu >= 0.5:
                sample = gpu_memory()
                if sample is not None:
                    gpu_peak = sample if gpu_peak is None else max(gpu_peak, sample)
                last_gpu = now
            time.sleep(0.025)
    elapsed = time.perf_counter() - start
    if process.returncode:
        raise RuntimeError(f"Run failed: {tag}; see {folder / 'console.log'}")
    runtime = load(folder / "runtime.json")
    expected = "cpu" if backend == "cpu" else "template_cuda"
    if runtime["feature_backend"] != expected or runtime["score_backend"] != expected or runtime["failed"]:
        raise RuntimeError(f"Unexpected backend or missing tiles: {tag}")
    if not render and output.exists():
        raise RuntimeError("Match-only unexpectedly rendered a mosaic")
    if not render and digest(request["db"]) != manifest["snapshot_sha256"]:
        raise RuntimeError("Match-only changed database bytes")
    selection = Path(str(output) + ".indices.bin")
    values = np.fromfile(selection, dtype=np.int32)
    rows = load(folder / "diagnostics.json")
    diagnostics = audit(values, rows, strategy, (case["width"] + 8) // 9)
    result = dict(id=tag, case=case["id"], strategy=strategy, seed=seed, backend=backend,
                  epsilon=epsilon, batch=batch, identity=identity, runtime=runtime,
                  selection=str(selection), selection_sha256=digest(selection),
                  diagnostics=str(folder / "diagnostics.json"), diagnostics_sha256=digest(folder / "diagnostics.json"),
                  diagnostic_summary=diagnostics, wall_seconds=elapsed, peak_rss_bytes=rss,
                  gpu_device_before_mib=gpu, gpu_device_peak_mib=gpu_peak, rendered=render)
    if render:
        result.update(output=str(output), output_sha256=digest(output))
    save(saved, result)
    print(f"{tag}: {runtime['metrics']['matchingMs']:.0f} ms matching", flush=True)
    return result


def run_main(args, manifest):
    results = []
    for number, case in enumerate(manifest["cases"]):
        for seed_number, seed in enumerate(SEEDS):
            strategies = STRATEGIES if (number + seed_number) % 2 == 0 else tuple(reversed(STRATEGIES))
            for strategy in strategies:
                for backend in tuple(b for b in (("cpu", "cuda") if seed_number % 2 == 0 else ("cuda", "cpu")) if b in BACKENDS):
                    result = execute(args, manifest, case, strategy, seed, backend)
                    if strategy == "L":
                        previous = args.baseline_study / "runs" / f"{case['id']}-baseline-{seed}-{backend}"
                        if result["selection_sha256"] != digest(previous / "mosaic.png.indices.bin"):
                            raise RuntimeError("Legacy fixed-seed selections changed")
                        result["legacy_selection_equal"] = True
                    results.append(result)
                    save(args.output_dir / "runs.json", results)
    sensitivity = []
    for case in manifest["cases"] if "cpu" in BACKENDS else ():
        for epsilon in (0.02, 0.10):
            tag = f"sensitivity-{case['id']}-{epsilon}"
            sensitivity.append(execute(args, manifest, case, "QDX", 42, "cpu", tag, epsilon=epsilon))
            save(args.output_dir / "sensitivity.json", sensitivity)
    save(args.output_dir / "sensitivity.json", sensitivity)


def verify(args, manifest):
    main = load(args.output_dir / "runs.json")
    checks = []
    for case in manifest["cases"]:
        for strategy in STRATEGIES[1:]:
            for backend in BACKENDS:
                original = next(r for r in main if (r["case"], r["strategy"], r["backend"], r["seed"]) ==
                                (case["id"], strategy, backend, 42))
                for label, batch in (("repeat", 1024), ("batch17", 17)):
                    tag = f"{label}-{case['id']}-{strategy}-{backend}"
                    repeated = execute(args, manifest, case, strategy, 42, backend, tag, batch=batch)
                    if repeated["selection_sha256"] != original["selection_sha256"]:
                        raise RuntimeError(f"Selection determinism failed: {tag}")
                    checks.append(dict(id=tag, batch=batch, equal=True, selection_sha256=repeated["selection_sha256"]))
                    save(args.output_dir / "reproducibility.json", checks)
    performance = []
    for case_id in ("portrait", "smooth"):
        case = next(c for c in manifest["cases"] if c["id"] == case_id)
        for backend in BACKENDS:
            for iteration in range(4):
                for strategy in (("L", "QDX") if iteration % 2 == 0 else ("QDX", "L")):
                    tag = f"perf-{case_id}-{strategy}-{backend}-{iteration}"
                    result = execute(args, manifest, case, strategy, 42, backend, tag)
                    result["warmup"] = iteration == 0
                    performance.append(result)
                    save(args.output_dir / "performance.json", performance)


def synthetic(args, original):
    root = args.output_dir / "synthetic"
    root.mkdir(exist_ok=True)
    manifest_path = root / "manifest.json"
    if manifest_path.exists():
        manifest = load(manifest_path)
        validate_configuration(manifest, original["executables"])
    else:
        inputs = root / "source"
        targets = root / "targets"
        inputs.mkdir()
        targets.mkdir()
        rng = np.random.default_rng(20261110)
        y, x = np.mgrid[:320, :180]
        for number in range(1024):
            color = rng.integers(55, 200, size=3)
            pattern = ((x // (8 + number % 19) + y // (12 + number % 23)) % 2)[..., None]
            pixels = np.broadcast_to(color, (320, 180, 3)).copy()
            pixels = np.clip(pixels + pattern * rng.integers(-25, 26, size=3), 35, 220).astype(np.uint8)
            pixels[10:15, number % 170:number % 170 + 5] = rng.integers(60, 190, size=3)
            Image.fromarray(pixels).save(inputs / f"pattern-{number:04}.png")
        for number in range(2):
            pixels = np.zeros((320, 180, 3), np.uint8)
            pixels[0, 0] = number * 8
            Image.fromarray(pixels).save(inputs / f"dark-{number}.png")
        for number in range(3):
            pixels = np.full((320, 180, 3), 255, np.uint8)
            pixels[0, 0] = 252 + number
            Image.fromarray(pixels).save(inputs / f"light-{number}.png")
        template = np.full((320, 180, 3), (110, 140, 170), np.uint8)
        template[:, 60:100] = (130, 155, 180)
        for number in range(8):
            pixels = template.copy()
            pixels[number, 0] = (100 + number, 130, 160)
            Image.fromarray(pixels).save(inputs / f"near-{number}.png")
        y, x = np.mgrid[:256, :144]
        abundant = np.stack((75 + x * 100 // 143, 80 + y * 100 // 255,
                             100 + ((x // 9 + y // 16) % 2) * 30), axis=2).astype(np.uint8)
        repeated = np.tile(np.array(Image.fromarray(template).resize((9, 16))), (16, 16, 1))
        arrays = {"abundant": abundant, "scarce2": np.zeros((128, 72, 3), np.uint8),
                  "scarce3": np.full((128, 72, 3), 255, np.uint8), "near_cross_row": repeated}
        cases = []
        for name, pixels in arrays.items():
            path = targets / f"{name}.png"
            Image.fromarray(pixels).save(path)
            cases.append(dict(id=name, input=str(path), sha256=digest(path),
                              width=pixels.shape[1], height=pixels.shape[0],
                              lRange=2 if name.startswith("scarce") else 20))
        library = root / "library"
        database = library / "mosaicraft.db"
        with (root / "build.log").open("w", encoding="utf-8") as log:
            subprocess.run([str(args.cli), "build", "-i", str(inputs), "-o", str(library),
                            "-d", str(database), "--normalize-size", "180x320", "-t", "4", "--yes"],
                           stdout=log, stderr=log, check=True)
        snapshot = library / "snapshot.db"
        with readonly_database(database) as source, closing(sqlite3.connect(snapshot)) as target:
            source.backup(target)
        manifest = dict(original, snapshot=str(snapshot), snapshot_sha256=digest(snapshot), cases=cases,
                        library="procedural", generator_seed=20261110, generated_source_images=1037)
        with readonly_database(snapshot) as db:
            manifest["library_rows"] = db.execute("SELECT COUNT(*) FROM images").fetchone()[0]
            manifest["brightness_eligible"] = dict(scarce2=db.execute(
                "SELECT COUNT(*) FROM images WHERE avg_l BETWEEN -2 AND 2").fetchone()[0],
                scarce3=db.execute("SELECT COUNT(*) FROM images WHERE avg_l BETWEEN 253 AND 257").fetchone()[0])
        if manifest["brightness_eligible"] != dict(scarce2=2, scarce3=3):
            raise RuntimeError("Synthetic fixture did not retain the intended scarce candidate counts")
        with (root / "prepare.log").open("w", encoding="utf-8") as log:
            subprocess.run([str(args.quality), "--prepare-cache", str(snapshot)], stdout=log, stderr=log, check=True)
        save(manifest_path, manifest)
    results = []
    for case in manifest["cases"]:
        for strategy in STRATEGIES:
            for backend in BACKENDS:
                tag = f"synthetic-{case['id']}-{strategy}-{backend}"
                results.append(execute(args, manifest, case, strategy, 42, backend, tag, render=True))
                save(root / "runs.json", results)
    checks = []
    for case in manifest["cases"]:
        for backend in BACKENDS:
            original_run = next(r for r in results if (r["case"], r["strategy"], r["backend"]) ==
                                (case["id"], "QDX", backend))
            tag = f"synthetic-repeat-{case['id']}-{backend}"
            repeated_run = execute(args, manifest, case, "QDX", 42, backend, tag, render=True, batch=17)
            pixels_equal = np.array_equal(read_image(original_run["output"]), read_image(repeated_run["output"]))
            if original_run["selection_sha256"] != repeated_run["selection_sha256"] or not pixels_equal:
                raise RuntimeError("Synthetic output is not deterministic across batches")
            checks.append(dict(id=tag, selection_equal=True, pixels_equal=pixels_equal))
            save(root / "reproducibility.json", checks)


def evaluate_runs(args, root, manifest, runs):
    study = dict(snapshot=manifest["snapshot"], cases=manifest["cases"], runs=runs)
    save(root / "study.json", study)
    with (root / "evaluation.log").open("w", encoding="utf-8") as log:
        subprocess.run([str(args.quality), str(root / "study.json"), str(root / "evaluation.json")],
                       stdout=log, stderr=log, check=True)
    evaluation = load(root / "evaluation.json")
    if not evaluation.get("complete"):
        raise RuntimeError("Incomplete evaluation")
    by_id = {r["id"]: r["quality"] for r in evaluation["runs"]}
    for run in runs:
        run["quality"] = by_id[run["id"]]
    return evaluation


def summarize(runs):
    output = []
    for case in dict.fromkeys(r["case"] for r in runs):
        for backend in BACKENDS:
            for strategy in STRATEGIES:
                group = [r for r in runs if (r["case"], r["backend"], r["strategy"]) == (case, backend, strategy)]
                if not group:
                    continue
                row = dict(case=case, backend=backend, strategy=strategy, runs=len(group))
                for metric in ("mean", "p90", "p99", "unique_images", "max_image_uses"):
                    row[metric] = statistics.mean(r["quality"][metric] for r in group)
                for radius in ("1", "2", "3"):
                    row[f"same_pair_rate_r{radius}"] = statistics.mean(
                        r["quality"]["neighborhood"][radius]["same_pair_rate"] for r in group)
                    row[f"similar_including_same_rate_r{radius}"] = statistics.mean(
                        r["quality"]["neighborhood"][radius]["similar_including_same_rate"] for r in group)
                row["matching_median_ms"] = statistics.median(r["runtime"]["metrics"]["matchingMs"] for r in group)
                if strategy != "L":
                    row["expanded_fraction"] = statistics.mean(r["diagnostic_summary"]["expanded_fraction"] for r in group)
                    row["radius_fallback_fraction"] = statistics.mean(
                        sum(int(v) for k, v in r["diagnostic_summary"]["radius_counts"].items() if int(k) in (0, 1, 2)) /
                        r["runtime"]["selected_count"] for r in group)
                output.append(row)
    return output


def synthetic_images(root, manifest, runs):
    comparisons = root / "comparisons"
    comparisons.mkdir(exist_ok=True)
    key = {"A": "D", "B": "QDX", "C": "L", "D": "Q", "E": "QD"}
    save(comparisons / "key.json", key)
    for case in manifest["cases"]:
        baseline = next(r for r in runs if (r["case"], r["strategy"], r["backend"]) == (case["id"], "L", BACKENDS[0]))
        columns = (case["width"] + 8) // 9
        rows = (case["height"] + 15) // 16
        centers = [rows // 2 * columns + columns // 2, *baseline["quality"]["worst_tiles"][:2]]
        save(comparisons / f"{case['id']}-coordinates.json", centers)
        for backend in BACKENDS:
            images = []
            for label, strategy in key.items():
                run = next(r for r in runs if (r["case"], r["strategy"], r["backend"]) ==
                           (case["id"], strategy, backend))
                pixels = read_image(run["output"])
                target = read_image(case["input"])
                target = cv2.copyMakeBorder(target, 0, rows * 16 - target.shape[0], 0,
                                           columns * 9 - target.shape[1], cv2.BORDER_REFLECT)
                small = cv2.resize(pixels, (target.shape[1], target.shape[0]), interpolation=cv2.INTER_AREA)
                run["pixel_quality"] = dict(lab_mae=float(np.abs(cv2.cvtColor(small, cv2.COLOR_BGR2Lab).astype(float) -
                    cv2.cvtColor(target, cv2.COLOR_BGR2Lab).astype(float)).mean()), gray_ssim=gray_ssim(small, target))
                full = Image.fromarray(cv2.cvtColor(pixels, cv2.COLOR_BGR2RGB))
                full.thumbnail((240, 400), Image.Resampling.LANCZOS)
                panel = Image.new("RGB", (250, 430), "white")
                panel.paste(full, ((250 - full.width) // 2, 25))
                ImageDraw.Draw(panel).text((10, 5), label, fill="black")
                images.append(panel)
                for number, tile in enumerate(centers):
                    tx, ty = tile % columns, tile // columns
                    area = pixels[max(0, ty - 1) * 80:min(rows, ty + 2) * 80,
                                  max(0, tx - 1) * 45:min(columns, tx + 2) * 45]
                    Image.fromarray(cv2.cvtColor(area, cv2.COLOR_BGR2RGB)).save(
                        comparisons / f"{case['id']}-{backend}-{label}-crop{number}.png")
            combined = Image.new("RGB", (250 * len(images), 430), "white")
            for number, panel in enumerate(images):
                combined.paste(panel, (number * 250, 0))
            combined.save(comparisons / f"{case['id']}-{backend}.png")


def evaluate(args, manifest):
    root = args.output_dir
    main = load(root / "runs.json")
    sensitivity = load(root / "sensitivity.json")
    performance = load(root / "performance.json")
    evaluation = evaluate_runs(args, root, manifest, main + sensitivity + performance)
    synthetic_manifest = load(root / "synthetic" / "manifest.json")
    synthetic_runs = load(root / "synthetic" / "runs.json")
    synthetic_evaluation = evaluate_runs(args, root / "synthetic", synthetic_manifest, synthetic_runs)
    synthetic_images(root / "synthetic", synthetic_manifest, synthetic_runs)
    performance_summary = []
    for case in ("portrait", "smooth"):
        for backend in BACKENDS:
            for strategy in ("L", "QDX"):
                group = [r for r in performance if (r["case"], r["backend"], r["strategy"]) ==
                         (case, backend, strategy) and not r["warmup"]]
                performance_summary.append(dict(case=case, backend=backend, strategy=strategy,
                    matching_median_ms=statistics.median(r["runtime"]["metrics"]["matchingMs"] for r in group),
                    wall_median_seconds=statistics.median(r["wall_seconds"] for r in group),
                    peak_rss_median_bytes=statistics.median(r["peak_rss_bytes"] for r in group),
                    gpu_device_peak_median_mib=median_available(r["gpu_device_peak_mib"] for r in group)))
    report = dict(manifest=manifest, runs=main, summaries=summarize(main), sensitivity=sensitivity,
                  record_index_to_id=evaluation["record_index_to_id"],
                  performance=performance, performance_summary=performance_summary,
                  oracle=evaluation["cases"], reproducibility=load(root / "reproducibility.json"),
                  synthetic=dict(manifest=synthetic_manifest, runs=synthetic_runs,
                                 record_index_to_id=synthetic_evaluation["record_index_to_id"],
                                 summaries=summarize(synthetic_runs), oracle=synthetic_evaluation["cases"],
                                 reproducibility=load(root / "synthetic" / "reproducibility.json")),
                  source_database_unchanged=digest(manifest["source_database"]) == manifest["source_database_sha256"],
                  source_inventory_unchanged=source_inventory(manifest["source_database"]) == manifest["source_inventory"],
                  analysis_commit=command_output(["git", "rev-parse", "HEAD"]), script_sha256=digest(__file__))
    if not report["source_database_unchanged"] or not report["source_inventory_unchanged"]:
        raise RuntimeError("Original library changed")
    save(root / "results.json", report)
    plot(root, report)
    export(root, report)


def plot(root, report):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    names = [c["id"] for c in report["manifest"]["cases"]]
    by_key = {(r["case"], r["strategy"]): r for r in report["summaries"] if r["backend"] == ("cuda" if "cuda" in BACKENDS else "cpu")}
    figure, axes = plt.subplots(2, 1, figsize=(11, 7), constrained_layout=True)
    x = np.arange(len(names))
    for number, strategy in enumerate(STRATEGIES[1:]):
        values = [(by_key[name, strategy]["mean"] / by_key[name, "L"]["mean"] - 1) * 100 for name in names]
        axes[0].bar(x + (number - 1.5) * 0.18, values, 0.18, label=strategy)
    axes[0].set(title="Selection study: reference feature error", ylabel="Change vs L (%)", xticks=x, xticklabels=names)
    axes[0].legend(ncol=4)
    for number, strategy in enumerate(STRATEGIES):
        axes[1].bar(np.arange(2) + (number - 2) * 0.15,
                    [by_key[name, strategy]["same_pair_rate_r3"] * 100 for name in ("screen", "smooth")],
                    0.15, label=strategy)
    axes[1].set(title="Same image pairs within radius 3", ylabel="Pairs (%)", xticks=[0, 1],
                xticklabels=["screen", "smooth"])
    axes[1].legend(ncol=5)
    figure.savefig(root / "summary.png", dpi=150)
    figure.savefig(root / "summary.svg")
    svg = root / "summary.svg"
    svg.write_text("\n".join(line.rstrip() for line in svg.read_text(encoding="utf-8").splitlines()) + "\n", encoding="utf-8")
    plt.close(figure)
    for name in ("screen", "smooth"):
        case = next(c for c in report["manifest"]["cases"] if c["id"] == name)
        shape = ((case["height"] + 15) // 16, (case["width"] + 8) // 9)
        figure, axes = plt.subplots(1, 3, figsize=(12, 5), constrained_layout=True)
        for axis, strategy in zip(axes[:2], ("QD", "QDX")):
            run = next(r for r in report["runs"] if (r["case"], r["strategy"], r["backend"], r["seed"]) ==
                       (name, strategy, ("cuda" if "cuda" in BACKENDS else "cpu"), 42))
            data = load(run["diagnostics"])
            chart = axis.imshow(np.array([d["radius"] for d in data]).reshape(shape), vmin=0, vmax=3,
                                interpolation="nearest", cmap="viridis")
            axis.set_title(f"{strategy}: chosen radius")
            figure.colorbar(chart, ax=axis, ticks=[0, 1, 2, 3])
        chart = axes[2].imshow(np.array([d["queried"] for d in data]).reshape(shape), vmin=150, vmax=600,
                               interpolation="nearest", cmap="plasma")
        axes[2].set_title("QDX: ANN query K")
        figure.colorbar(chart, ax=axes[2], ticks=[150, 300, 600])
        figure.savefig(root / f"diagnostic-{name}.png", dpi=140)
        plt.close(figure)


def export(root, report):
    public = json.loads(json.dumps(report))
    groups = (public["runs"], public["sensitivity"], public["performance"], public["synthetic"]["runs"])
    for group in groups:
        for run in group:
            run["selection"] = f"selections/{run['id']}.bin"
            run["diagnostics"] = f"diagnostics/{run['id']}.json"
            if run.get("rendered"):
                run["output"] = f"synthetic/outputs/{run['id']}.png"
    save(root / "public-results.json", public_report(public))
    archive = root / "selection-study-evidence.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
        bundle.write(root / "public-results.json", "results.json")
        bundle.write(root / "summary.svg", "summary.svg")
        bundle.write(root / "summary.png", "summary.png")
        for path in root.glob("diagnostic-*.png"):
            bundle.write(path, path.name)
        for folder in sorted((root / "runs").iterdir()):
            if not (folder / "run.json").exists():
                continue
            run = load(folder / "run.json")
            bundle.write(run["selection"], f"selections/{run['id']}.bin")
            bundle.write(run["diagnostics"], f"diagnostics/{run['id']}.json")
            if run.get("rendered"):
                bundle.write(run["output"], f"synthetic/outputs/{run['id']}.png")
        for path in sorted((root / "synthetic" / "comparisons").glob("*")):
            bundle.write(path, f"synthetic/comparisons/{path.name}")
        for path in sorted((root / "synthetic" / "targets").glob("*.png")):
            bundle.write(path, f"synthetic/targets/{path.name}")
    with zipfile.ZipFile(archive) as bundle:
        if bundle.testzip() is not None:
            raise RuntimeError("Evidence archive failed CRC validation")
    (root / "selection-study-evidence.zip.sha256").write_text(digest(archive) + "\n", encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--match", type=Path, required=True)
    parser.add_argument("--quality", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--baseline-study", type=Path, default=REPO / "build/quality-study-20261110")
    parser.add_argument("--output-dir", type=Path, default=REPO / "build/selection-study-20261110")
    parser.add_argument("--phase", choices=("prepare", "runs", "verify", "synthetic", "evaluate", "all"), default="all")
    parser.add_argument("--database", type=Path, help="local source library binding")
    parser.add_argument("--inputs", type=Path, help="local input directory for importing a legacy study")
    parser.add_argument("--backends", choices=("cpu", "cuda", "both"), default="both")
    args = parser.parse_args()
    for name in ("match", "quality", "cli", "baseline_study", "output_dir"):
        setattr(args, name, getattr(args, name).resolve())
    global BACKENDS
    BACKENDS = ("cpu", "cuda") if args.backends == "both" else (args.backends,)
    import quality_study
    quality_study.BACKENDS = BACKENDS
    configure(args)
    if args.phase == "synthetic":
        args.output_dir.mkdir(parents=True, exist_ok=True)
        manifest = dict(schema=2, executables=executables(args), backends=BACKENDS,
                        flags=dict(colorAdjust=False, overlayStrength=0, adaptiveWeights=False,
                                   nativeTileW=45, nativeTileH=80, tileW=9, tileH=16, lRange=20))
    else:
        manifest = prepare(args)
    if args.phase in ("runs", "all"):
        run_main(args, manifest)
    if args.phase in ("verify", "all"):
        verify(args, manifest)
    if args.phase in ("synthetic", "all"):
        synthetic(args, manifest)
    if args.phase in ("evaluate", "all"):
        evaluate(args, manifest)


if __name__ == "__main__":
    main()
