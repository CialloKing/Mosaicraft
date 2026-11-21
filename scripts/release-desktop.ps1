param(
    [Parameter(Mandatory)][string]$QtRoot,
    [string]$BuildDir = 'build/package-qt',
    [string]$OutputDir = 'build/archives-qt',
    [string]$VcpkgInstalledDir = $env:MOSAICRAFT_VCPKG_INSTALLED_DIR
)

$ErrorActionPreference = 'Stop'
function Invoke-Checked($Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed ($LASTEXITCODE): $($Arguments -join ' ')" }
}

Push-Location -LiteralPath (Split-Path $PSScriptRoot -Parent)
try {
    if (!(Test-Path -LiteralPath "$QtRoot/lib/cmake/Qt6/Qt6Config.cmake" -PathType Leaf)) { throw "Invalid Qt SDK: $QtRoot" }
    $QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
    $contract = Get-Content -Raw -LiteralPath 'contracts.json' | ConvertFrom-Json
    $qt = Get-Content -Raw -LiteralPath 'cmake/qt-sdk.json' | ConvertFrom-Json
    if (@(Invoke-Checked git @('status', '--porcelain', '--untracked-files=normal')).Count) {
        throw 'Package only a clean, committed workspace'
    }
    $commit = Invoke-Checked git @('rev-parse', 'HEAD')
    if ($env:GITHUB_SHA -and $commit -ne $env:GITHUB_SHA) { throw 'Checkout does not match the release commit' }
    if ($env:GITHUB_REF_TYPE -eq 'tag' -and $env:GITHUB_REF_NAME -ne "v$($contract.softwareVersion)") {
        throw 'Release tag does not match contracts.json'
    }
    $platform, $preset, $triplet, $format, $extension = if ($IsWindows) {
        'windows-x64', 'windows-cpu', 'x64-windows-release', 'zip', 'zip'
    } elseif ($IsMacOS) {
        'macos-arm64', 'macos-arm64-cpu', 'arm64-osx-release', 'gnutar', 'tar.gz'
    } elseif ($IsLinux) {
        'linux-x64', 'linux-cpu', 'x64-linux-release', 'gnutar', 'tar.gz'
    } else { throw 'Unsupported desktop release host' }
    $arch = if ($IsMacOS) { 'Arm64' } else { 'X64' }
    if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne $arch) { throw 'A native CPU build is required' }
    if (!$VcpkgInstalledDir) { $VcpkgInstalledDir = 'third_party/vcpkg_installed' }
    if (!$env:VCPKG_ROOT -or !(Test-Path -LiteralPath "$VcpkgInstalledDir/$triplet" -PathType Container)) {
        throw "Prepare the native vcpkg toolchain and $triplet dependencies first"
    }
    $VcpkgInstalledDir = (Resolve-Path -LiteralPath $VcpkgInstalledDir).Path
    $BuildDir = [IO.Path]::GetFullPath($BuildDir, (Get-Location).ProviderPath)
    $OutputDir = [IO.Path]::GetFullPath($OutputDir, (Get-Location).ProviderPath)
    $stem = "Mosaicraft_v$($contract.softwareVersion)_${platform}_qt"
    $staging = Join-Path $BuildDir ("stage-" + [guid]::NewGuid().ToString('N'))
    $package = Join-Path $staging $stem
    $native = Join-Path $BuildDir 'native'
    $gui = Join-Path $BuildDir 'gui'
    $toolchain = Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'
    $guiOptions = if ($IsWindows) { @('-G', 'Visual Studio 18 2026', '-A', 'x64') } else { @('-G', 'Ninja') }
    if ($IsMacOS) { $guiOptions += '-DCMAKE_OSX_ARCHITECTURES=arm64' }
    Invoke-Checked cmake @('--preset', $preset, '-B', $native,
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DVCPKG_INSTALLED_DIR=$VcpkgInstalledDir",
        "-DVCPKG_TARGET_TRIPLET=$triplet", "-DVCPKG_HOST_TRIPLET=$triplet", '-DVCPKG_MANIFEST_INSTALL=OFF',
        '-DMOSAICRAFT_BUNDLE_RUNTIME=ON', '-DMOSAICRAFT_DESKTOP_BACKEND_INSTALL=ON',
        '-DMOSAICRAFT_CUDA=OFF', '-DMOSAICRAFT_VULKAN=OFF', '-DMOSAICRAFT_METAL=OFF')
    Invoke-Checked cmake @('--build', $native, '--config', 'Release', '--target', 'mosaicraft', 'mosaicraft_desktop_worker', '--parallel')
    Invoke-Checked cmake (@('-S', 'desktop', '-B', $gui, '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$QtRoot",
        '-DMOSAICRAFT_DEPLOY_QT=ON', '-DCMAKE_INSTALL_BINDIR=bin', '-DCMAKE_INSTALL_LIBDIR=lib', "-DMOSAICRAFT_QT_RELEASE_VERSION=$($qt.version)") + $guiOptions)
    Invoke-Checked cmake @('--build', $gui, '--config', 'Release', '--parallel')
    Invoke-Checked cmake @('--install', $gui, '--config', 'Release', '--prefix', $package)

    # Keep the backend next to the GUI so both programs are found without PATH or preferences.
    $app = Join-Path $package 'MosaicraftDesktop.app'
    $bin = if ($IsMacOS) { Join-Path $app 'Contents/MacOS' } else { Join-Path $package 'bin' }
    Invoke-Checked cmake @('--install', $native, '--config', 'Release', '--prefix', $bin)
    if ($IsMacOS) {
        # Adding the already relocated backend changes the bundle seal.
        Invoke-Checked codesign @('--force', '--deep', '--sign', '-', $app)
        Invoke-Checked codesign @('--verify', '--deep', '--strict', $app)
    }
    Copy-Item -LiteralPath 'LICENSE','docs/development/desktop-redistribution.txt' -Destination $package
    $notice = Join-Path $package 'desktop-redistribution.txt'
    (Get-Content -Raw -LiteralPath $notice).Replace('@QT_VERSION@', $qt.version) | Set-Content -LiteralPath $notice
    $qtNotices = Join-Path $package 'licenses/qt'
    New-Item -ItemType Directory -Force -Path $qtNotices | Out-Null
    Copy-Item -LiteralPath 'third_party/qt-notices/LGPL-3.0-only.txt' -Destination $qtNotices
    # Keep the component/third-party inventory provided by the actual SDK archives.
    foreach ($module in @('qtbase', 'qtimageformats', 'qttranslations')) {
        Copy-Item -LiteralPath "$QtRoot/sbom/$module-$($qt.version).cdx.json" -Destination $qtNotices
    }
    @("Version: $($contract.softwareVersion)", "GitCommit: $commit", "Platform: $platform", 'Runtime: cpu-only', "QtVersion: $($qt.version)") |
        Set-Content -LiteralPath (Join-Path $package 'BUILD_INFO.txt')

    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
    $archive = Join-Path $OutputDir "$stem.$extension"
    Push-Location -LiteralPath $staging
    try {
        # Preserve executable bits and framework links in the Unix tarballs.
        $mode = if ($IsWindows) { 'cf' } else { 'czf' }
        Invoke-Checked cmake @('-E', 'tar', $mode, $archive, "--format=$format", '--', $stem)
    } finally {
        Pop-Location
    }
    & (Join-Path $PSScriptRoot 'verify-desktop-asset.ps1') -ArchivePath $archive
    $hash = (Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath "$archive.sha256" -Encoding ascii
    Write-Output "Packaged $archive"
} finally {
    Pop-Location
}
