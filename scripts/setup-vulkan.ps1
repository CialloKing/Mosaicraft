#Requires -Version 7.0
param([string]$Destination = "$PSScriptRoot/../build/tools/vulkan")
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Only build-time headers/compiler are installed here; archives embed the shaders.
New-Item -ItemType Directory -Force $Destination | Out-Null
$root = (Resolve-Path $Destination).Path
$headers = Join-Path $root 'Vulkan-Headers'
$revision = 'b379292b2ab6df5771ba9870d53cf8b2c9295daf'
if (-not (Test-Path (Join-Path $headers '.git')))
{
    git clone --depth 1 --branch v1.3.290 https://github.com/KhronosGroup/Vulkan-Headers.git $headers
    if ($LASTEXITCODE -ne 0) { throw 'Vulkan headers download failed' }
}
if ((git -C $headers rev-parse HEAD).Trim() -ne $revision) { throw 'Unexpected Vulkan headers revision' }
$env:MOSAICRAFT_VULKAN_INCLUDE = Join-Path $headers 'include'
if ($IsWindows)
{
    $archive = Join-Path $root 'glslang-16.6.0.zip'
    $sha256 = '82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b'
    if (-not (Test-Path $archive))
    {
        Invoke-WebRequest https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip -OutFile $archive
    }
    if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha256)
    {
        throw 'glslang archive hash mismatch'
    }
    Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $root 'glslang') -Force
    $env:MOSAICRAFT_GLSLANG = Join-Path $root 'glslang/bin/glslang.exe'
}
else
{
    $env:MOSAICRAFT_GLSLANG = (Get-Command glslangValidator -ErrorAction Stop).Source
}
if ($env:GITHUB_ENV)
{
    "MOSAICRAFT_VULKAN_INCLUDE=$env:MOSAICRAFT_VULKAN_INCLUDE" | Add-Content $env:GITHUB_ENV
    "MOSAICRAFT_GLSLANG=$env:MOSAICRAFT_GLSLANG" | Add-Content $env:GITHUB_ENV
}
Write-Host "Vulkan headers: $revision; offline compiler: $env:MOSAICRAFT_GLSLANG"
