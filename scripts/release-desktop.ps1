param(
    [Parameter(Mandatory)][string]$QtRoot,
    [string]$BuildDir = 'build/package-qt',
    [string]$OutputDir = 'build/archives-qt',
    [string]$VcpkgInstalledDir = $env:MOSAICRAFT_VCPKG_INSTALLED_DIR,
    [string]$SourceDir = (Split-Path $PSScriptRoot -Parent),
    [string]$ExpectedSourceCommit = $env:GITHUB_SHA,
    [switch]$CudaVulkan,
    [ValidateSet('Required','BuildOnly')][string]$GpuValidation = 'Required'
)

$ErrorActionPreference = 'Stop'
function Invoke-Checked($Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed ($LASTEXITCODE): $($Arguments -join ' ')" }
}

$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path
$BuildDir = [IO.Path]::GetFullPath($BuildDir, $SourceDir)
$OutputDir = [IO.Path]::GetFullPath($OutputDir, $SourceDir)
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
Push-Location -LiteralPath $SourceDir
try {
    if (!(Test-Path -LiteralPath "$QtRoot/lib/cmake/Qt6/Qt6Config.cmake" -PathType Leaf)) { throw "Invalid Qt SDK: $QtRoot" }
    $contract = Get-Content -Raw -LiteralPath 'contracts.json' | ConvertFrom-Json
    $qt = Get-Content -Raw -LiteralPath 'cmake/qt-sdk.json' | ConvertFrom-Json
    if (@(Invoke-Checked git @('status', '--porcelain', '--untracked-files=normal')).Count) {
        throw 'Package only a clean, committed workspace'
    }
    $commit = Invoke-Checked git @('rev-parse', 'HEAD')
    if ($ExpectedSourceCommit -and $commit -ne $ExpectedSourceCommit) { throw 'Checkout does not match the release commit' }
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
    $nativeOptions = @('-DMOSAICRAFT_CUDA=OFF', '-DMOSAICRAFT_VULKAN=OFF', '-DMOSAICRAFT_VULKAN_AUTO=OFF', '-DMOSAICRAFT_METAL=OFF')
    $runtime = 'cpu-only'
    $suffix = 'qt'
    if ($CudaVulkan) {
        if ($IsMacOS) { throw 'CUDA + Vulkan desktop archives require Windows or Linux x64' }
        if (!$env:CUDAToolkit_ROOT) { throw 'Set CUDAToolkit_ROOT to the locked CUDA SDK' }
        $preset = if ($IsWindows) { 'windows-cuda' } else { 'linux-cuda' }
        # Keep the tagged source's auto policy (CUDA -> CPU); Vulkan is selected explicitly.
        $nativeOptions = @('-DMOSAICRAFT_CUDA:BOOL=ON', '-DMOSAICRAFT_VULKAN:BOOL=ON',
            '-DMOSAICRAFT_VULKAN_AUTO:BOOL=OFF', '-DMOSAICRAFT_METAL:BOOL=OFF',
            '-DCUDA_ARCH=sm_75;sm_80;sm_86;sm_89;sm_90;sm_100;sm_120', "-DCUDAToolkit_ROOT=$env:CUDAToolkit_ROOT")
        $runtime = 'cuda-vulkan'
        $suffix = if ($IsWindows -and $GpuValidation -eq 'BuildOnly') { 'qt_cuda-vulkan_candidate' } else { 'qt_cuda-vulkan' }
    }
    $arch = if ($IsMacOS) { 'Arm64' } else { 'X64' }
    if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne $arch) { throw 'A native build is required' }
    if (!$VcpkgInstalledDir) { $VcpkgInstalledDir = 'third_party/vcpkg_installed' }
    if (!$env:VCPKG_ROOT -or !(Test-Path -LiteralPath "$VcpkgInstalledDir/$triplet" -PathType Container)) {
        throw "Prepare the native vcpkg toolchain and $triplet dependencies first"
    }
    $VcpkgInstalledDir = (Resolve-Path -LiteralPath $VcpkgInstalledDir).Path
    $stem = "Mosaicraft_v$($contract.softwareVersion)_${platform}_${suffix}"
    $staging = Join-Path $BuildDir ("stage-" + [guid]::NewGuid().ToString('N'))
    $package = Join-Path $staging $stem
    $native = Join-Path $BuildDir 'native'
    $gui = Join-Path $BuildDir 'gui'
    $toolchain = Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'
    $guiOptions = if ($IsWindows) { @('-G', 'Visual Studio 18 2026', '-A', 'x64') } else { @('-G', 'Ninja') }
    if ($IsMacOS) { $guiOptions += '-DCMAKE_OSX_ARCHITECTURES=arm64' }
    Invoke-Checked cmake (@('--preset', $preset, '-B', $native,
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DVCPKG_INSTALLED_DIR=$VcpkgInstalledDir",
        "-DVCPKG_TARGET_TRIPLET=$triplet", "-DVCPKG_HOST_TRIPLET=$triplet", '-DVCPKG_MANIFEST_INSTALL=OFF',
        '-DMOSAICRAFT_BUNDLE_RUNTIME=ON', '-DMOSAICRAFT_DESKTOP_BACKEND_INSTALL=ON') + $nativeOptions)
    $nativeTargets = @('mosaicraft', 'mosaicraft_desktop_worker')
    if ($CudaVulkan) { $nativeTargets += 'mosaicraft_tests' }
    Invoke-Checked cmake (@('--build', $native, '--config', 'Release', '--target') + $nativeTargets + '--parallel')
    $gpuInfo = @()
    if ($CudaVulkan) {
        $images = Get-Content -Raw -LiteralPath (Join-Path $native 'cuda-architectures.txt') | ConvertFrom-StringData -Delimiter ':'
        $cudaLock = Get-Content -Raw -LiteralPath 'cmake/cuda-13.5.json' | ConvertFrom-Json
        $cudaMode = if ($IsWindows) { 'hybrid' } else { 'shared' }
        if ($images.CudaToolkit -ne $cudaLock.components.cuda_nvcc.version -or $images.CudaRuntimeMode -ne $cudaMode) {
            throw 'Desktop GPU archive requires the locked CUDA toolkit/runtime'
        }
        $exe = if ($IsWindows) { '.exe' } else { '' }
        $cuobjdump = Join-Path (Split-Path $images.Nvcc) "cuobjdump$exe"
        foreach ($program in @('mosaicraft', 'mosaicraft_desktop_worker')) {
            & (Join-Path $PSScriptRoot 'Assert-CudaArchitectures.ps1') -BuildDir $native `
                -Executable (Join-Path $native "Release/bin/$program$exe") -Cuobjdump $cuobjdump -RequireUniversal
        }
        $state = 'not-run'
        if ($GpuValidation -eq 'Required') {
            $probe = Invoke-Checked (Join-Path $native "Release/bin/mosaicraft_tests$exe") @('--require-cuda')
            if ('GPU validation passed' -notin $probe) { throw 'CUDA kernel probe did not pass' }
            $probe | Out-Host
            $gpuInfo += @($probe | Where-Object { $_ -match '^GpuValidation\w+:' })
            $state = 'passed'
        }
        ctest --test-dir $native -C Release -R '^mosaicraft_tests$' --no-tests=error --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Desktop GPU core tests failed' }
        $gpuInfo += @("GpuValidation: $state", "PortableGpuValidation: $state", 'PortableAutoSelection: cuda-to-cpu',
            'CudaBackend: enabled', 'VulkanBackend: enabled', "CudaToolkit: $($images.CudaToolkit)",
            "CudaRuntimeMode: $cudaMode", "CudaArchitectures: $($images.CudaArchitectures)", "CudaPtx: $($images.CudaPtx)")
    }
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
    if ($CudaVulkan) {
        # The same notice template is used by the CPU payload in the tagged source.
        (Get-Content -Raw -LiteralPath $notice).Replace('Desktop (CPU)', 'Desktop (CUDA + Vulkan)') | Set-Content -LiteralPath $notice
        Add-Content -LiteralPath $notice -Value "`nAuto uses CUDA when available, otherwise CPU. Select Vulkan explicitly.`nInstall the host driver for the GPU backend you intend to use."
    }
    $qtNotices = Join-Path $package 'licenses/qt'
    New-Item -ItemType Directory -Force -Path $qtNotices | Out-Null
    Copy-Item -LiteralPath 'third_party/qt-notices/LGPL-3.0-only.txt' -Destination $qtNotices
    if ($IsLinux) {
        Copy-Item -LiteralPath 'third_party/qt-notices/ICU-73.2.txt' -Destination $qtNotices
    }
    # Keep the component/third-party inventory provided by the actual SDK archives.
    foreach ($module in @('qtbase', 'qtimageformats', 'qttranslations')) {
        Copy-Item -LiteralPath "$QtRoot/sbom/$module-$($qt.version).cdx.json" -Destination $qtNotices
    }
    @("Version: $($contract.softwareVersion)", "GitCommit: $commit", "Platform: $platform", "Runtime: $runtime", "QtVersion: $($qt.version)") + $gpuInfo |
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
    $verify = @{ ArchivePath = $archive; ExpectedCommit = $commit }
    if ($CudaVulkan) { $verify.RunSmoke = $true; $verify.RequireGpu = $GpuValidation -eq 'Required' }
    & (Join-Path $PSScriptRoot 'verify-desktop-asset.ps1') @verify
    $hash = (Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath "$archive.sha256" -Encoding ascii
    Write-Output "Packaged $archive"
} finally {
    Pop-Location
}
