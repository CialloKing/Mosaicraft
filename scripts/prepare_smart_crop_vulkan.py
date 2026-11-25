#!/usr/bin/env python3
"""Reproduce the local FP32 ncnn smart-crop model conversion and probe."""

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VERSION = "20261020"
NCNN_REVISION = "7e36d25cb153092ba87a60530455c7376fedf452"
ASSETS = {
    f"ncnn-{VERSION}-windows-vs2026.zip": "3e65e31846afd96e83a4a1c17e2aa825fde75eb9c927663a06a059336cf84312",
    f"pnnx-{VERSION}-windows.zip": "e4f91348a399a0060a6e7dd323d40427e1cbdcab67469cf1904b5d09b23606c5",
}
PNNX_EXE_SHA256 = "b8c56a0b0319e6ce4d229e9a339e4ee377fb0c42f7f3e66eec235ed00cd71b18"


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def yunet_dynamic(source, target):
    import onnx

    model = onnx.load(source)
    # OpenCV accepts original-size images. These dimensions are metadata; the
    # existing Reshape nodes already infer grid lengths with -1.
    dims = model.graph.input[0].type.tensor_type.shape.dim
    dims[2].dim_param = "height"
    dims[3].dim_param = "width"
    for output in model.graph.output:
        output.type.tensor_type.shape.dim[1].dim_param = "cells_" + output.name.rsplit("_", 1)[1]
    onnx.checker.check_model(model)
    onnx.save(model, target)


def run(command, directory, log):
    with (directory / log).open("wb") as out:
        try:
            return subprocess.run(command, cwd=directory, stdout=out, stderr=subprocess.STDOUT,
                                  timeout=120, check=False).returncode
        except subprocess.TimeoutExpired:
            return "timeout"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("models", "tools", "work"):
        parser.add_argument(arg, type=lambda p: Path(p).resolve())
    parser.add_argument("--probe", type=lambda p: Path(p).resolve())
    parser.add_argument("--device", type=int, default=1)
    parser.add_argument("--max-real", nargs=2, type=int, metavar=("WIDTH", "HEIGHT"))
    parser.add_argument("--max-anime", nargs=2, type=int, metavar=("WIDTH", "HEIGHT"))
    args = parser.parse_args()
    for name, expected in ASSETS.items():
        if sha256(args.tools / name) != expected:
            parser.error(f"unexpected SHA-256: {name}")
    pnnx = args.tools / f"pnnx-{VERSION}-windows" / "pnnx.exe"
    if sha256(pnnx) != PNNX_EXE_SHA256:
        parser.error("unexpected pnnx executable SHA-256")
    lock = json.loads((ROOT / "cmake/smart-crop-models.json").read_text(encoding="utf-8"))
    args.work.mkdir(parents=True, exist_ok=False)
    report = {"ncnnRevision": NCNN_REVISION, "toolArchives": ASSETS, "pnnxSha256": PNNX_EXE_SHA256, "models": {}}
    if args.probe:
        report["probeSha256"] = sha256(args.probe)
        report["deviceIndex"] = args.device
        report["enumerationExit"] = run([args.probe], args.work, "adapters.log")
    for row in lock["models"]:
        model_id = row["id"]
        source = args.models / row["file"]
        if sha256(source) != row["sha256"]:
            raise ValueError(f"wrong source model: {source}")
        directory = args.work / model_id
        directory.mkdir()
        result = report["models"][model_id] = {"sourceSha256": row["sha256"]}
        saliency = model_id == "saliency"
        shapes = ["inputshape=[1,3,320,320]f32"] if saliency else [
            "inputshape=[1,3,256,352]f32", "inputshape2=[1,3,384,640]f32"]
        shutil.copyfile(source, directory / "locked.onnx")
        if model_id == "real":
            result["lockedDynamicConversionExit"] = run([pnnx, "locked.onnx", "fp16=0", *shapes], directory, "locked.log")
            yunet_dynamic(source, directory / "model.onnx")
        else:
            (directory / "locked.onnx").rename(directory / "model.onnx")
        result["conversionInputSha256"] = sha256(directory / "model.onnx")
        result["conversionExit"] = run([pnnx, "model.onnx", "fp16=0", *shapes], directory, "conversion.log")
        result["conversionWarnings"] = sorted({line.split()[1] for line in
            (directory / "conversion.log").read_text(encoding="utf-8", errors="replace").splitlines()
            if line.startswith("ignore ")})
        artifacts = [directory / "model.ncnn.param", directory / "model.ncnn.bin"]
        result["artifacts"] = {p.name: sha256(p) for p in artifacts if p.is_file()}
        if args.probe and result["conversionExit"] == 0 and len(result["artifacts"]) == len(artifacts):
            sizes = [(320, 320)] if saliency else [(352, 256), (640, 384), (352, 256)]
            pressure = args.max_real if model_id == "real" else args.max_anime if not saliency else None
            if pressure:
                sizes.extend((tuple((dimension + 31) // 32 * 32 for dimension in pressure), (352, 256)))
            result["probeSizes"] = sizes
            result["probeExit"] = run([args.probe, str(args.device), model_id, *artifacts, "tensor",
                                       *(str(n) for size in sizes for n in size)], directory, "vulkan.log")
        print(f"{model_id}: convert={result['conversionExit']} probe={result.get('probeExit', 'not run')}")
    (args.work / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return int(any(row["conversionExit"] != 0 or row["conversionWarnings"] or
                   (args.probe and row.get("probeExit") != 0) for row in report["models"].values()))


if __name__ == "__main__":
    raise SystemExit(main())
