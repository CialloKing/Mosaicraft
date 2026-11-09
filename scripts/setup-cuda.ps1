# Install only the official, hash-locked compiler/runtime redistribution components.
# No display driver or GPU is needed to build a candidate.
#Requires -Version 7.0
param([string]$Destination = 'build/tools/cuda-13.5')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$lock = Get-Content (Join-Path $repo 'cmake/cuda-13.5.json') -Raw | ConvertFrom-Json
$windows = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
$platform = if ($windows) { 'windows-x86_64' } else { 'linux-x86_64' }
$root = [IO.Path]::GetFullPath((Join-Path $repo $Destination))
$downloads = Join-Path $repo 'build/cuda-downloads'
New-Item -ItemType Directory -Force -Path $root, $downloads | Out-Null
foreach ($component in $lock.components.PSObject.Properties)
{
    $asset = $component.Value.$platform
    $archive = Join-Path $downloads (Split-Path $asset.relative_path -Leaf)
    if (-not (Test-Path $archive) -or (Get-FileHash $archive).Hash -ne $asset.sha256)
    {
        Invoke-WebRequest "https://developer.download.nvidia.com/compute/cuda/redist/$($asset.relative_path)" `
            -OutFile $archive -MaximumRetryCount 3 -RetryIntervalSec 3
    }
    if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne $asset.sha256) { throw "CUDA hash mismatch: $archive" }
    & tar -xf $archive -C $root --strip-components=1
    if ($LASTEXITCODE -ne 0) { throw "Cannot extract $archive" }
    # Each component's license would otherwise overwrite the previous one.
    $license = Join-Path $root 'LICENSE.txt'
    if (Test-Path $license)
    {
        $licenseDir = Join-Path $root 'licenses'
        New-Item -ItemType Directory -Force -Path $licenseDir | Out-Null
        Copy-Item -LiteralPath $license -Destination (Join-Path $licenseDir "$($component.Name).txt")
    }
}
$nvcc = Join-Path $root $(if ($windows) { 'bin/nvcc.exe' } else { 'bin/nvcc' })
$version = & $nvcc --version | Out-String
$expectedVersion = [regex]::Escape($lock.components.cuda_nvcc.version)
if ($LASTEXITCODE -ne 0 -or $version -notmatch "V$expectedVersion(?![0-9])") { throw "Unexpected CUDA compiler: $version" }
Write-Host $version
$env:CUDA_PATH = $root
$env:CUDAToolkit_ROOT = $root
$env:PATH = "$(Join-Path $root 'bin')$([IO.Path]::PathSeparator)$env:PATH"
if ($env:GITHUB_ENV)
{
    "CUDA_PATH=$root", "CUDAToolkit_ROOT=$root" | Add-Content $env:GITHUB_ENV
    (Join-Path $root 'bin') | Add-Content $env:GITHUB_PATH
}
