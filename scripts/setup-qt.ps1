# Download only the SDK components needed to compile and deploy the desktop client.
param([Parameter(Mandatory)][string]$InstallDir)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$lock = Get-Content -Raw -LiteralPath (Join-Path $repo 'cmake/qt-sdk.json') | ConvertFrom-Json -AsHashtable
$platform = if ($IsWindows) { 'windows-x64' } elseif ($IsMacOS) { 'macos-arm64' } elseif ($IsLinux) { 'linux-x64' }
$arch = if ($IsMacOS) { 'Arm64' } else { 'X64' }
if (!$platform -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne $arch) {
    throw 'The Qt release SDK requires a supported native runner'
}
$InstallDir = [IO.Path]::GetFullPath($InstallDir, (Get-Location).ProviderPath)
$sdk = $lock.platforms[$platform]
$downloads = Join-Path $InstallDir '.downloads'
$qtRoot = Join-Path $InstallDir $lock.version
New-Item -ItemType Directory -Force -Path $downloads, $qtRoot | Out-Null
foreach ($item in $sdk.archives) {
    $archive = Join-Path $downloads (Split-Path -Leaf $item.path)
    if (!(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive).Hash -ne $item.sha256) {
        # The lock uses the checksum of the bytes, not a mirror's transient metadata.
        & curl --fail --silent --show-error --location --proto '=https' --connect-timeout 15 --max-time 240 --retry 3 --output $archive "$($lock.repository)/$($item.path)"
        if ($LASTEXITCODE -ne 0 -or (Get-FileHash -LiteralPath $archive).Hash -ne $item.sha256) {
            throw "Could not download the locked Qt archive: $($item.path)"
        }
    }
    # ICU is stored flat; Qt's Linux libraries resolve it next to libQt6Core.
    $destination = if ($item.destination) { Join-Path $qtRoot $item.destination } else { $qtRoot }
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Push-Location -LiteralPath $destination
    try {
        & cmake -E tar xf $archive
        if ($LASTEXITCODE -ne 0) { throw "Could not extract $archive" }
    } finally {
        Pop-Location
    }
}
# Offline Installer payloads lack the installer-generated, relocatable qt.conf.
@('[Paths]', 'Prefix=..') | Set-Content -LiteralPath (Join-Path $qtRoot 'bin/qt.conf') -Encoding ascii
if (!(Test-Path -LiteralPath (Join-Path $qtRoot 'lib/cmake/Qt6/Qt6Config.cmake') -PathType Leaf)) {
    throw "Missing Qt CMake configuration in $qtRoot"
}
# lrelease loads QtQml on Unix; checking the tool catches incomplete SDKs before the build.
$extension = if ($IsWindows) { '.exe' } else { '' }
& (Join-Path $qtRoot "bin/lrelease$extension") -version | Out-Host
if ($LASTEXITCODE -ne 0) { throw "The Qt translation tool cannot run in $qtRoot" }
if ($env:GITHUB_ENV) { "MOSAICRAFT_QT_ROOT=$qtRoot" | Out-File -LiteralPath $env:GITHUB_ENV -Encoding utf8 -Append }
Write-Output $qtRoot
