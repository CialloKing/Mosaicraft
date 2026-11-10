"""Explicit reproducible quality study; writes only under the selected study folder.

Production CLI/API and matching rules are unchanged. Requires NumPy, Pillow and
Python OpenCV and Matplotlib; executables use the project's own OpenCV and scoring.
"""
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import shutil
import sqlite3
import statistics
import subprocess
import sys
import time

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

from benchmark import gpu_memory, peak_rss

SEEDS = (42, 20261110, 20261111)
VARIANTS = {"baseline": (150, 10), "best": (150, 1),
            "top3": (150, 3), "recall600": (600, 1)}


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def save(path, value):
    temporary = Path(str(path) + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding="utf-8")
    temporary.replace(path)


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def command_output(command):
    return subprocess.check_output(command, text=True, encoding="utf-8", errors="replace").strip()


def readonly_database(path):
    return sqlite3.connect(Path(path).resolve().as_uri() + "?mode=ro", uri=True)


def source_inventory(database):
    """Metadata audit includes every original image and independent feature file."""
    summary = hashlib.sha256()
    with readonly_database(database) as connection:
        for row in connection.execute("SELECT id,file_path,tiny_path,hist_path FROM images ORDER BY id"):
            summary.update(str(row[0]).encode())
            for value in row[1:]:
                path = Path(value)
                stat = path.stat()
                summary.update(str(path).encode("utf-8"))
                summary.update(f"{stat.st_size}:{stat.st_mtime_ns}".encode())
    return summary.hexdigest()


def prepare(args):
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    manifest_path = root / "manifest.json"
    if manifest_path.exists():
        manifest = load(manifest_path)
        if digest(args.database) != manifest["source_database_sha256"]:
            raise RuntimeError("Original database differs from study manifest")
        return manifest
    source_hash = digest(args.database)
    snapshot = root / "snapshot.db"
    with readonly_database(args.database) as source, sqlite3.connect(snapshot) as target:
        source.backup(target)
    features = root / "features"
    features.mkdir()
    with sqlite3.connect(snapshot) as connection:
        rows = connection.execute("SELECT id,tiny_path,hist_path FROM images ORDER BY id").fetchall()
        for image_id, tiny, histogram in rows:
            paths = []
            for old, extension in ((tiny, ".tiny"), (histogram, ".hist")):
                new = features / (f"id_{image_id}" + extension)
                try:
                    os.link(old, new)
                except OSError:
                    shutil.copy2(old, new)
                paths.append(str(new))
            connection.execute("UPDATE images SET tiny_path=?,hist_path=? WHERE id=?", (*paths, image_id))
        connection.commit()
    inputs = root / "inputs"
    inputs.mkdir()
    sources = [("screen", "屏幕测试图.png"), ("rainbow", "屏幕彩虹渐变测试图.png"),
               ("portrait", "target5.png")]
    for name, source in sources:
        shutil.copy2(args.inputs / source, inputs / f"{name}.png")
    y, x = np.mgrid[0:640, 0:360]
    smooth = np.repeat(np.rint(y / 639 * 255).astype(np.uint8)[..., None], 3, axis=2)
    diagonal = np.full((640, 360, 3), 225, np.uint8)
    diagonal[np.abs(x - y * 360 / 640) < 12] = (25, 25, 25)
    diagonal[((x // 60 + y // 100) % 3 == 0) & (x > 220)] = (70, 110, 160)
    pattern = ((x // 4 + y // 4) % 2) * 160 + 45
    texture = np.stack((pattern, np.roll(pattern, 2, axis=0), np.roll(pattern, 2, axis=1)), axis=2).astype(np.uint8)
    for name, array in (("smooth", smooth), ("diagonal", diagonal), ("texture", texture)):
        Image.fromarray(array).save(inputs / f"{name}.png")
    cases = []
    for name in ("screen", "rainbow", "portrait", "smooth", "diagonal", "texture"):
        path = inputs / f"{name}.png"
        with Image.open(path) as image:
            width, height = image.size
        scale = 640 / max(width, height)
        cases.append(dict(id=name, input=str(path), width=max(1, round(width * scale)),
                          height=max(1, round(height * scale)), sha256=digest(path)))
    manifest = dict(schema=1, created_date="2026-11-10", source_commit=command_output(["git", "rev-parse", "HEAD"]),
                    tools_commit=command_output(["git", "rev-parse", "HEAD"]),
                    tool_sources_sha256={"scripts/" + name: digest(Path(__file__).with_name(name)) for name in
                                         ("quality_study.py", "benchmark_match.cpp", "benchmark_quality.cpp")},
                    source_database=str(args.database.resolve()), source_database_sha256=source_hash,
                    source_inventory=source_inventory(args.database), snapshot=str(snapshot),
                    snapshot_sha256=digest(snapshot), library_rows=len(rows), cases=cases,
                    seeds=SEEDS, variants=VARIANTS, python_opencv=cv2.__version__,
                    python_version=sys.version, platform=platform.platform(),
                    runtime_dependencies_sha256={p.name: digest(p) for p in args.match.resolve().parent.glob("*.dll")},
                    gpu=command_output(["nvidia-smi", "--query-gpu=name,memory.total,driver_version", "--format=csv,noheader"]),
                    executables={"match": dict(path=str(args.match.resolve()), sha256=digest(args.match)),
                                 "quality": dict(path=str(args.quality.resolve()), sha256=digest(args.quality))},
                    flags=dict(colorAdjust=False, overlayStrength=0, adaptiveWeights=False,
                               nativeTileW=45, nativeTileH=80, tileW=9, tileH=16, lRange=20),
                    memory_metric="Windows process peak working set; sampled total GPU device usage, not per-process VRAM")
    save(manifest_path, manifest)
    subprocess.run([str(args.quality.resolve()), "--prepare-cache", str(snapshot)], check=True)
    return manifest


def execute(args, manifest, case, variant, seed, backend, tag):
    root = args.output_dir.resolve()
    folder = root / "runs" / tag
    folder.mkdir(parents=True, exist_ok=True)
    saved = folder / "run.json"
    if saved.exists():
        result = load(saved)
        if (result["executable_sha256"] == digest(args.match) and
                digest(result["selection"]) == result["selection_sha256"] and
                digest(result["output"]) == result["output_sha256"]):
            return result
        raise RuntimeError(f"Stale run needs a new study folder: {tag}")
    database = root / "run.db"
    # A closed child owns this disposable database; restore its history for every run.
    for suffix in ("-wal", "-shm", "-journal"):
        Path(str(database) + suffix).unlink(missing_ok=True)
    shutil.copy2(manifest["snapshot"], database)
    candidates, topn = VARIANTS[variant]
    output = folder / "mosaic.png"
    request = dict(input=case["input"], db=str(database), output=str(output),
                   outW=case["width"], outH=case["height"], candidates=candidates,
                   topNrandom=topn, cpu=backend == "cpu", format="png", writeMode="stream",
                   **manifest["flags"])
    request_path = folder / "request.json"
    save(request_path, request)
    summary = folder / "runtime.json"
    command = [str(args.match.resolve()), str(request_path), "--seed", str(seed),
               "--selections", "--summary", str(summary)]
    before_gpu = gpu_memory()
    gpu_peak = before_gpu
    rss = 0
    start = time.perf_counter()
    with (folder / "stdout.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        next_gpu = 0
        while process.poll() is None:
            rss = max(rss, peak_rss(process) or 0)
            if time.monotonic() >= next_gpu:
                used = gpu_memory()
                if used is not None:
                    gpu_peak = max(gpu_peak or 0, used)
                next_gpu = time.monotonic() + 0.5
            time.sleep(0.025)
        rss = max(rss, peak_rss(process) or 0)
    elapsed = time.perf_counter() - start
    if process.returncode:
        raise RuntimeError(f"Study run failed: {folder / 'stdout.log'}")
    runtime = load(summary)
    if runtime["failed"] or runtime["matched"] != runtime["selected_count"]:
        raise RuntimeError(f"Incomplete mosaic: {tag}")
    if backend == "cuda" and "cuda" not in runtime["score_backend"]:
        raise RuntimeError(f"CUDA unexpectedly fell back: {runtime}")
    if backend == "cpu" and runtime["score_backend"] != "cpu":
        raise RuntimeError(f"CPU request did not select CPU: {runtime}")
    selection = str(output) + ".indices.bin"
    result = dict(id=tag, case=case["id"], variant=variant, seed=seed, backend=backend,
                  executable_sha256=digest(args.match), request=request, runtime=runtime,
                  selection=selection, selection_sha256=digest(selection), output=str(output),
                  output_sha256=digest(output), wall_seconds=elapsed, peak_rss_bytes=rss,
                  gpu_device_before_mib=before_gpu, gpu_device_peak_mib=gpu_peak)
    save(saved, result)
    print(f"{tag}: {elapsed:.2f}s, {runtime['matched']} tiles, {runtime['score_backend']}", flush=True)
    return result


def runs(args, manifest):
    results = []
    # Interleave backend and variant order so heat/cache effects do not favor one group.
    for case_number, case in enumerate(manifest["cases"]):
        for seed_number, seed in enumerate(SEEDS):
            variants = list(VARIANTS)
            if (case_number + seed_number) % 2:
                variants.reverse()
            for variant in variants:
                backends = ("cpu", "cuda") if seed_number % 2 == 0 else ("cuda", "cpu")
                for backend in backends:
                    tag = f"{case['id']}-{variant}-{seed}-{backend}"
                    results.append(execute(args, manifest, case, variant, seed, backend, tag))
                    save(args.output_dir / "runs.json", results)
    # Repeat a random-selection run on every scene/backend; Top-1 alone would not test the seed.
    repeats = []
    for case in manifest["cases"]:
        for backend in ("cpu", "cuda"):
            tag = f"{case['id']}-repeat-42-{backend}"
            repeated = execute(args, manifest, case, "baseline", 42, backend, tag)
            original = next(r for r in results if r["case"] == case["id"] and
                            r["variant"] == "baseline" and r["seed"] == 42 and r["backend"] == backend)
            if repeated["selection_sha256"] != original["selection_sha256"]:
                raise RuntimeError(f"Fixed seed is not reproducible: {tag}")
            pixels_equal = repeated["output_sha256"] == original["output_sha256"] or np.array_equal(
                read_image(repeated["output"]), read_image(original["output"]))
            if not pixels_equal:
                raise RuntimeError(f"Same selection produced different pixels: {tag}")
            repeats.append(dict(id=tag, selection_equal=True, pixels_equal=pixels_equal))
    save(args.output_dir / "reproducibility.json", repeats)
    performance = []
    for case_id in ("portrait", "rainbow"):
        case = next(c for c in manifest["cases"] if c["id"] == case_id)
        for backend in ("cpu", "cuda"):
            for iteration in range(4):
                for variant in (("baseline", "best") if iteration % 2 == 0 else ("best", "baseline")):
                    tag = f"perf-{case_id}-{variant}-{backend}-{iteration}"
                    result = execute(args, manifest, case, variant, 42, backend, tag)
                    result["warmup"] = iteration == 0
                    performance.append(result)
                    save(args.output_dir / "performance.json", performance)
    return results


def read_image(path):
    image = cv2.imdecode(np.fromfile(path, np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        raise RuntimeError(f"Cannot decode {path}")
    return image


def gray_ssim(a, b):
    a = cv2.cvtColor(a, cv2.COLOR_BGR2GRAY).astype(np.float64)
    b = cv2.cvtColor(b, cv2.COLOR_BGR2GRAY).astype(np.float64)
    mean_a = cv2.GaussianBlur(a, (11, 11), 1.5)
    mean_b = cv2.GaussianBlur(b, (11, 11), 1.5)
    variance_a = cv2.GaussianBlur(a * a, (11, 11), 1.5) - mean_a * mean_a
    variance_b = cv2.GaussianBlur(b * b, (11, 11), 1.5) - mean_b * mean_b
    covariance = cv2.GaussianBlur(a * b, (11, 11), 1.5) - mean_a * mean_b
    result = ((2 * mean_a * mean_b + 6.5025) * (2 * covariance + 58.5225) /
              ((mean_a * mean_a + mean_b * mean_b + 6.5025) * (variance_a + variance_b + 58.5225)))
    return float(result[5:-5, 5:-5].mean())


def evaluate(args, manifest):
    root = args.output_dir.resolve()
    results = load(root / "runs.json")
    study = dict(snapshot=manifest["snapshot"], cases=manifest["cases"], runs=results)
    save(root / "study.json", study)
    subprocess.run([str(args.quality.resolve()), str(root / "study.json"), str(root / "evaluation.json")], check=True)
    evaluation = load(root / "evaluation.json")
    if not evaluation.get("complete") or len(evaluation["runs"]) != len(results):
        raise RuntimeError("Incomplete quality evaluation")
    by_id = {r["id"]: r["quality"] for r in evaluation["runs"]}
    for case in manifest["cases"]:
        target = read_image(case["input"])
        target = cv2.resize(target, (case["width"], case["height"]), interpolation=cv2.INTER_AREA)
        columns = (target.shape[1] + 8) // 9
        rows = (target.shape[0] + 15) // 16
        target = cv2.copyMakeBorder(target, 0, rows * 16 - target.shape[0],
                                   0, columns * 9 - target.shape[1], cv2.BORDER_REFLECT)
        target_lab = cv2.cvtColor(target, cv2.COLOR_BGR2LAB).astype(float)
        for run in (r for r in results if r["case"] == case["id"]):
            output = read_image(run["output"])
            thumbnail = cv2.resize(output, (target.shape[1], target.shape[0]), interpolation=cv2.INTER_AREA)
            difference = np.abs(cv2.cvtColor(thumbnail, cv2.COLOR_BGR2LAB).astype(float) - target_lab)
            run["quality"] = by_id[run["id"]]
            run["appearance"] = dict(lab_mae_encoded_0_255=float(difference.mean()),
                                     lab_channel_mae=difference.mean((0, 1)).tolist(),
                                     gray_ssim=gray_ssim(target, thumbnail))
    summaries = []
    for case in manifest["cases"]:
        for backend in ("cpu", "cuda"):
            for variant in VARIANTS:
                group = [r for r in results if r["case"] == case["id"] and
                         r["backend"] == backend and r["variant"] == variant]
                summary = dict(case=case["id"], backend=backend, variant=variant)
                for name in ("mean", "p90", "p99", "unique_images", "max_image_uses"):
                    summary[name] = statistics.mean(r["quality"][name] for r in group)
                summary["lab_mae"] = statistics.mean(r["appearance"]["lab_mae_encoded_0_255"] for r in group)
                summary["gray_ssim"] = statistics.mean(r["appearance"]["gray_ssim"] for r in group)
                summary["same_pair_rate_r3"] = statistics.mean(r["quality"]["neighborhood"]["3"]["same_pair_rate"] for r in group)
                summaries.append(summary)
    performance = load(root / "performance.json")
    perf_summaries = []
    for case in ("portrait", "rainbow"):
        for backend in ("cpu", "cuda"):
            for variant in ("baseline", "best"):
                group = [r for r in performance if r["case"] == case and r["backend"] == backend and
                         r["variant"] == variant and not r["warmup"]]
                perf_summaries.append(dict(case=case, backend=backend, variant=variant,
                    wall_median_seconds=statistics.median(r["wall_seconds"] for r in group),
                    peak_rss_median_bytes=statistics.median(r["peak_rss_bytes"] for r in group),
                    gpu_total_peak_median_mib=statistics.median(r["gpu_device_peak_mib"] for r in group)))
    report = dict(manifest=manifest, runs=results, summaries=summaries, oracle=evaluation["cases"],
                  analysis_commit=command_output(["git", "rev-parse", "HEAD"]),
                  analysis_script_sha256=digest(__file__),
                  performance=performance, performance_summary=perf_summaries,
                  reproducibility=load(root / "reproducibility.json"),
                  source_database_unchanged=digest(args.database) == manifest["source_database_sha256"],
                  source_inventory_unchanged=source_inventory(args.database) == manifest["source_inventory"])
    check = root / "metric-crosscheck.json"
    if check.exists():
        report["independent_metric_crosscheck"] = load(check)
    if not report["source_database_unchanged"] or not report["source_inventory_unchanged"]:
        raise RuntimeError("Source library changed during the study")
    save(root / "results.json", report)
    comparisons(root, manifest, results)
    summary_plot(root, report)
    export_data(root, report)
    print(f"Complete study: {root / 'results.json'}", flush=True)


def export_data(root, report):
    # 可交付数据保留全部测量与选图摘要，不附带原始马赛克图像。
    public = json.loads(json.dumps(report))
    public["comparison_visualization"] = "stored mean colors and numeric image IDs; no library photographs"
    public["visualization_script_sha256"] = digest(__file__)
    for group in ("runs", "performance"):
        for run in public[group]:
            run.pop("output", None)
            run["request"].pop("output", None)
            run["selection"] = f"selections/{run['id']}.bin"
    save(root / "public-results.json", public)


def summary_plot(root, report):
    import matplotlib.pyplot as plt
    names = [c["id"] for c in report["manifest"]["cases"]]
    summaries = {(r["case"], r["variant"]): r for r in report["summaries"] if r["backend"] == "cuda"}
    x = np.arange(len(names))
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5), layout="constrained")
    for i, variant in enumerate(("best", "top3", "recall600")):
        values = [(summaries[name, variant]["mean"] / summaries[name, "baseline"]["mean"] - 1) * 100 for name in names]
        axes[0].bar(x + (i - 1) * 0.25, values, 0.25, label=variant)
    axes[0].set(title="Feature error vs baseline (lower is better)", ylabel="Change (%)")
    axes[0].axhline(0, color="black", linewidth=0.7)
    axes[0].set_xticks(x, names, rotation=35, ha="right")
    axes[0].legend(fontsize=8)
    for i, variant in enumerate(VARIANTS):
        values = [summaries[name, variant]["same_pair_rate_r3"] * 100 for name in ("screen", "smooth")]
        axes[1].bar(np.arange(2) + (i - 1.5) * 0.2, values, 0.2, label=variant)
    axes[1].set(title="Same-image neighbor pairs, radius 3", ylabel="Pair rate (%)")
    axes[1].set_xticks([0, 1], ["screen", "smooth"])
    axes[1].legend(fontsize=8)
    for i, count in enumerate(("150", "600")):
        values = [statistics.mean(r["ann"][count]["top10_recall"] for r in report["oracle"][name]["oracle"]
                                  if r["ann"][count]["top10_recall"] is not None) * 100 for name in names]
        axes[2].bar(x + (i - 0.5) * 0.35, values, 0.35, label=count + " candidates")
    axes[2].set(title="Top-10 recall: 32 probes per target", ylabel="Recall (%)", ylim=(80, 101))
    axes[2].set_xticks(x, names, rotation=35, ha="right")
    axes[2].legend(fontsize=8)
    fig.suptitle("Mosaicraft quality study | CUDA selections, common CPU scoring | 2026-11-10", fontsize=12)
    for extension in ("svg", "png"):
        fig.savefig(root / "comparisons" / ("summary." + extension), dpi=160)
    plt.close(fig)


def comparisons(root, manifest, results):
    """Draw numeric color/identity maps, without reproducing library photographs."""
    folder = root / "comparisons"
    folder.mkdir(exist_ok=True)
    labels = list(VARIANTS)
    np.random.default_rng(20261110).shuffle(labels)
    key = {chr(65 + i): variant for i, variant in enumerate(labels)}
    save(folder / "key.json", key)
    font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 18) if os.name == "nt" else ImageFont.load_default()
    # Match Database::allRecords exactly: omitting its WHERE clause changes equal-count row order.
    with readonly_database(manifest["snapshot"]) as connection:
        records = connection.execute(
            "SELECT id,avg_l,avg_a,avg_b FROM images WHERE avg_l BETWEEN ? AND ? "
            "ORDER BY use_count ASC LIMIT ?", (-np.finfo(float).max, np.finfo(float).max,
                                             manifest["library_rows"] + 1)).fetchall()
    save(folder / "record-index-to-id.json", [record[0] for record in records])
    colors = np.clip(np.rint(np.array([r[1:] for r in records])), 0, 255).astype(np.uint8)
    colors = cv2.cvtColor(colors.reshape(-1, 1, 3), cv2.COLOR_LAB2RGB).reshape(-1, 3)
    for case in manifest["cases"]:
        baseline = next(r for r in results if r["case"] == case["id"] and
                        r["backend"] == "cpu" and r["variant"] == "baseline" and r["seed"] == 42)
        columns = (case["width"] + 8) // 9
        rows = (case["height"] + 15) // 16
        centers = [rows // 2 * columns + columns // 2, *baseline["quality"]["worst_tiles"][:2]]
        save(folder / f"{case['id']}-crop-tiles.json", centers)
        reference = read_image(case["input"])
        reference = cv2.resize(reference, (case["width"], case["height"]), interpolation=cv2.INTER_AREA)
        reference = cv2.copyMakeBorder(reference, 0, rows * 16 - reference.shape[0],
                                       0, columns * 9 - reference.shape[1], cv2.BORDER_REFLECT)
        reference = Image.fromarray(cv2.cvtColor(reference, cv2.COLOR_BGR2RGB))
        for backend in ("cpu", "cuda"):
            selections = []
            images = []
            for variant in labels:
                run = next(r for r in results if r["case"] == case["id"] and
                           r["backend"] == backend and r["variant"] == variant and r["seed"] == 42)
                selected = np.fromfile(run["selection"], dtype=np.int32).reshape(rows, columns)
                if np.any(selected < 0) or np.any(selected >= len(records)):
                    raise RuntimeError("Invalid selection in numeric comparison")
                selections.append(selected)
                images.append(Image.fromarray(colors[selected]).resize((columns * 45, rows * 80),
                                                                       Image.Resampling.NEAREST))
            preview_height = min(470, round(304 * rows * 80 / (columns * 45)))
            sheet = Image.new("RGB", (1600, preview_height + 80), "#eeeeee")
            draw = ImageDraw.Draw(sheet)
            for i, (label, image) in enumerate(zip(["Reference", *key], [reference, *images])):
                preview = image.copy()
                preview.thumbnail((304, preview_height), Image.Resampling.LANCZOS)
                sheet.paste(preview, (i * 320 + (320 - preview.width) // 2, 40))
                draw.text((i * 320 + 12, 10), label, fill="black", font=font)
            draw.text((12, preview_height + 48), "A-D: stored mean colors only; source photographs are not shown.",
                      fill="black", font=font)
            sheet.save(folder / f"{case['id']}-{backend}-overview.png")
            crops = Image.new("RGB", (1200, 1530), "#eeeeee")
            draw = ImageDraw.Draw(crops)
            for row, tile in enumerate(centers):
                x = min(max(tile % columns - 1, 0), max(columns - 3, 0))
                y = min(max(tile // columns - 1, 0), max(rows - 3, 0))
                for col, (label, selected) in enumerate(zip(key, selections)):
                    draw.text((col * 300 + 10, row * 510 + 7), f"{label} / tile {tile}", fill="black", font=font)
                    for dy in range(min(3, rows)):
                        for dx in range(min(3, columns)):
                            index = selected[y + dy, x + dx]
                            color = tuple(int(v) for v in colors[index])
                            left = col * 300 + 15 + dx * 90
                            top = row * 510 + 30 + dy * 160
                            draw.rectangle((left, top, left + 89, top + 159), fill=color, outline="#888888")
                            ink = "white" if sum(color) < 350 else "black"
                            draw.text((left + 4, top + 60), str(records[index][0]), fill=ink, font=font)
            crops.save(folder / f"{case['id']}-{backend}-crops.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, default=Path("library/180x320/mosaicraft.db"))
    parser.add_argument("--inputs", type=Path, default=Path("test_input_photos"))
    parser.add_argument("--output-dir", type=Path, default=Path("build/quality-study-20261110"))
    parser.add_argument("--match", type=Path, required=True)
    parser.add_argument("--quality", type=Path, required=True)
    parser.add_argument("--phase", choices=("prepare", "runs", "evaluate", "all"), default="all")
    args = parser.parse_args()
    manifest = prepare(args)
    for key, executable in (("match", args.match), ("quality", args.quality)):
        if digest(executable) != manifest["executables"][key]["sha256"]:
            raise RuntimeError("Executable changed; use a fresh output folder")
    if args.phase in ("runs", "all"):
        runs(args, manifest)
    if args.phase in ("evaluate", "all"):
        evaluate(args, manifest)


if __name__ == "__main__":
    main()
