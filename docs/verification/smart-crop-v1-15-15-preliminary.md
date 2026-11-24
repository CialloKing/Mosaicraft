# v1.15.15 Smart Crop Preliminary Release Review

## Scope and approval

This preliminary review covers the Windows x64 CUDA smart-crop package, which
includes CPU inference fallback. It does not approve a separate CPU-only smart
crop package, Linux/macOS smart crop, or the Qt desktop package. The project
maintainer approved this scope on 2026-11-24 with the model provenance
limitations listed below.

## Quality evidence

- Windows x64 normal, CPU smart-crop, and CUDA smart-crop builds each passed
  7/7 CTest checks. Fault tests covered automatic CUDA-to-CPU fallback and
  recovery, strict CUDA failure, and model-contract errors.
- Two manually annotated positive images, one real-person and one anime,
  passed CLI and Web/API crop checks on private Windows CPU and CUDA packages.
  Pixel verification confirmed that center cropping clipped the annotated
  main face while smart crop preserved it. The source images remain local and
  are not redistributed with the package or repository.
- A separate runtime test kept only Windows system directories in PATH,
  launched from another working directory, and loaded the CUDA package's
  locked runtime DLLs. This is a runtime-isolation check, not an independent
  clean operating-system installation or network-disconnection test.
- On an RTX 4060 Laptop with driver 621.14, three CPU and three CUDA rounds
  produced byte-identical outputs. The sampled anime batch measured 9.83x
  median throughput improvement; the measurements are development evidence,
  not a broad performance guarantee.

## Release limitations

The review is preliminary and does not establish broad accuracy across image
populations. The pinned U2-NetP and DeepGHS ONNX artifacts have upstream
license declarations, but their exact artifact provenance has not been
independently confirmed. The maintainer's release approval accepts that
limitation for this package; this record does not claim upstream confirmation.
The YuNet model has a pinned upstream MIT notice. Final package release still
requires real-GPU build validation, exact archive hash verification, and
offline package checks bound to that archive.
