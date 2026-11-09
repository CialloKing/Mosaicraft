#Requires -Version 7.0
# vcpkg already locks and verifies its CMake download. Reuse it for presets on
# runners whose system CMake is older, without changing dependency cache keys.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$text = & cmake --version | Out-String
if ($LASTEXITCODE -ne 0 -or $text -notmatch 'cmake version ([0-9]+\.[0-9]+\.[0-9]+)') { throw "Cannot query CMake: $text" }
if ([version]$Matches[1] -ge [version]'4.2.0') { Write-Host $text; return }
if (-not $env:VCPKG_ROOT) { throw 'Run setup-dependencies.ps1 first, or install CMake 4.2+' }
$vcpkg = Join-Path $env:VCPKG_ROOT $(if ($IsWindows) { 'vcpkg.exe' } else { 'vcpkg' })
$output = & $vcpkg fetch cmake
if ($LASTEXITCODE -ne 0) { throw 'Cannot fetch the CMake locked by vcpkg' }
$executable = ([string]($output | Select-Object -Last 1)).Trim().Trim('"')
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Cannot locate vcpkg CMake: $output" }
$directory = Split-Path $executable
$env:PATH = "$directory$([IO.Path]::PathSeparator)$env:PATH"
if ($env:GITHUB_PATH) { $directory | Add-Content $env:GITHUB_PATH }
$text = & cmake --version | Out-String
if ($LASTEXITCODE -ne 0 -or $text -notmatch 'cmake version ([0-9]+\.[0-9]+\.[0-9]+)' -or
    [version]$Matches[1] -lt [version]'4.2.0') { throw "CMake 4.2+ required: $text" }
Write-Host $text
