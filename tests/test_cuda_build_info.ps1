Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../scripts/Assert-CudaBuildInfo.ps1"
Assert-CudaBuildInfo @{ Runtime = 'cuda' }
$info = @{
    Runtime = 'cuda'; GpuValidation = 'not-run'
    CudaArchitectures = 'sm_75;sm_80;sm_86;sm_89;sm_90;sm_100;sm_120'; CudaPtx = 'compute_75;compute_80;compute_86;compute_89;compute_90;compute_100;compute_120'
    GpuValidationDevice = 'not-run'; GpuValidationComputeCapability = 'not-run'; GpuValidationDriver = 'not-run'
}
Assert-CudaBuildInfo $info
$linux = $info.Clone(); $linux.Platform = 'linux'; $linux.Arch = 'x64'
Assert-CudaPackageName $linux 'Mosaicraft_v1.14.8_linux-x64_cuda.tar.gz'
Assert-CudaPackageName $linux 'Mosaicraft_v1.14.8_linux-x64_cuda_candidate.tar.gz'
$windows = $linux.Clone(); $windows.Platform = 'windows'
Assert-CudaPackageName $windows 'Mosaicraft_v1.14.8_windows-x64_cuda_candidate.zip'
foreach ($case in @(
    @($windows, 'Mosaicraft_v1.14.8_windows-x64_cuda.zip'),
    @($linux, 'Mosaicraft_v1.14.8_linux-x64_cuda.zip'),
    @($linux, 'Mosaicraft_v1.14.8_windows-x64_cuda.zip')
))
{
    $rejected = $false
    try { Assert-CudaPackageName $case[0] $case[1] } catch { $rejected = $true }
    if (-not $rejected) { throw 'Invalid unvalidated release name accepted' }
}
$legacy = $info.Clone()
$legacy.CudaArchitectures = 'sm_75;sm_80;sm_86;sm_89;sm_90;sm_120'; $legacy.CudaPtx = 'compute_75'
Assert-CudaBuildInfo $legacy
$toolkit = $linux.Clone()
$toolkit.CudaToolkit = '13.5.37'; $toolkit.CudaRuntimeMode = 'shared'; $toolkit.GpuValidationRuntime = 'not-run'
Assert-CudaBuildInfo $toolkit
foreach ($mutation in @(@{CudaRuntimeMode='hybrid'}, @{CudaToolkit='13.5'}, @{GpuValidationRuntime='13050'}, @{Runtime='cpu-only'}))
{
    $invalid = $toolkit.Clone()
    foreach ($key in $mutation.Keys) { $invalid[$key] = $mutation[$key] }
    $rejected = $false
    try { Assert-CudaBuildInfo $invalid } catch { $rejected = $true }
    if (-not $rejected) { throw 'Invalid toolkit metadata accepted' }
}
$toolkit.Platform = 'windows'; $toolkit.CudaRuntimeMode = 'hybrid'
$toolkit.GpuValidation = 'passed'; $toolkit.GpuValidationDevice = 'RTX 4060 Laptop'
$toolkit.GpuValidationComputeCapability = 'sm_89'; $toolkit.GpuValidationDriver = '13050'; $toolkit.GpuValidationRuntime = '13050'
Assert-CudaBuildInfo $toolkit
$toolkit.Remove('CudaRuntimeMode')
$rejected = $false
try { Assert-CudaBuildInfo $toolkit } catch { $rejected = $true }
if (-not $rejected) { throw 'Partial toolkit metadata accepted' }
foreach ($mutation in @(@{CudaArchitectures='sm_89'}, @{CudaPtx='compute_120'}, @{CudaPtx='compute_75'}, @{GpuValidationDevice='RTX 4060'}, @{Runtime='cpu-only'}))
{
    $invalid = $info.Clone()
    foreach ($key in $mutation.Keys) { $invalid[$key] = $mutation[$key] }
    $rejected = $false
    try { Assert-CudaBuildInfo $invalid } catch { $rejected = $true }
    if (-not $rejected) { throw 'Invalid CUDA metadata accepted' }
}
$info.GpuValidation = 'passed'; $info.GpuValidationDevice = 'RTX 4060 Laptop'
$info.GpuValidationComputeCapability = 'sm_89'; $info.GpuValidationDriver = '13030'
Assert-CudaBuildInfo $info
$info.Remove('CudaPtx')
$rejected = $false
try { Assert-CudaBuildInfo $info } catch { $rejected = $true }
if (-not $rejected) { throw 'Partial CUDA metadata accepted' }
