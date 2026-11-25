# Windows CUDA 智能裁剪轻量化实验

本实验不改变默认后端和正式发行包。目标是仅保留三个锁定模型需要的 CUDA 算子，以同源码 cuDNN 路径为对照。

## 探针

在 x64 Visual Studio Developer PowerShell 中运行：

```powershell
cmake "-DCUDA_ROOT=C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.5" `
  "-DWORK=$PWD/build/smart-crop-cutlass" -P scripts/probe_smart_crop_cutlass.cmake
```

只编译 CUTLASS 的头文件模板。探针在 MSVC 14.51.36231、nvcc 13.5.37、驱动 621.14、RTX 4060 Laptop（sm_89）上通过 SIMT FP32 GEMM、3×3 卷积和 3×3 空洞卷积精确结果检查。`/Zc:preprocessor` 是本工具链的 CCCL 要求；使用了原工具链，未修改 CUTLASS 源码。

## 对比方式

用两个独立安装树中的 CLI，避免加载对侧的 OpenCV DLL。对 200 张动漫和 32 张独立真人图分别运行：

```powershell
python scripts/bench_smart_crop_cuda.py <基线CLI> <输入目录> <新报告目录> `
  --study cutlass --candidate-exe <候选CLI> --mode smart-anime --rounds 3
```

真人用 `--mode smart-real`。交错运行，报告逐文件 SHA-256、智能裁剪坐标、CPU 回退、墙钟时间、工作集及显存采样。这个对比不替代仅驱动环境中的模块加载验收。
