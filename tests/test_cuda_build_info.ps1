Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../scripts/Assert-CudaBuildInfo.ps1"
Assert-CudaBuildInfo @{ Runtime = 'cuda' }
$info = @{
    Runtime = 'cuda'; GpuValidation = 'not-run'
    CudaArchitectures = 'sm_75;sm_80;sm_86;sm_89;sm_90;sm_120'; CudaPtx = 'compute_75'
    GpuValidationDevice = 'not-run'; GpuValidationComputeCapability = 'not-run'; GpuValidationDriver = 'not-run'
}
Assert-CudaBuildInfo $info
foreach ($mutation in @(@{CudaArchitectures='sm_89'}, @{CudaPtx='compute_120'}, @{GpuValidationDevice='RTX 4060'}, @{Runtime='cpu-only'}))
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
