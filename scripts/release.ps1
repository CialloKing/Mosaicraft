#Requires -Version 7.0
param(
    [string]$BuildDir = "",
    [ValidateSet('Release')][string]$Configuration = "Release",
    [string]$Version = "",
    [string]$ToolchainFile = "",
    [string]$PackagePlatform = "",
    [string]$PackageArch = "",
    [string]$PackageSuffix = "",
    [string]$OutputDir = "",
    [ValidateSet('auto', 'zip', 'tar.gz')][string]$ArchiveFormat = 'auto',
    [switch]$NoCuda,
    [switch]$Vulkan,
    [switch]$Metal,
    [ValidateSet('Required', 'BuildOnly')][string]$CudaValidation = 'Required',
    [switch]$AllowUnvalidatedCudaRelease,
    [switch]$AllowUntestedRelease,
    [switch]$SkipConfigure,
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$SkipSmoke,
    [switch]$SkipArchiveValidation,
    [switch]$InspectOnly,
    [switch]$KeepWorkspace
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot 'Archive.ps1')
. (Join-Path $PSScriptRoot 'ReleaseDocumentation.ps1')
. (Join-Path $PSScriptRoot 'Assert-CudaBuildInfo.ps1')
$cudaBuildInfo = @{}
$gpuProbeInfo = @{}
$script:stageTimings = [ordered]@{}

function Invoke-Native {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Invoke-NativeOutput
{
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    $output = & $FilePath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0)
    {
        $text = ($output | Out-String).Trim()
        throw "$FilePath failed with exit code $LASTEXITCODE. $text"
    }
    return $output
}

function Invoke-Step {
    param(
        [string]$Label,
        [scriptblock]$Action
    )

    Write-Host ""
    Write-Host "==> $Label"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & $Action
    $script:stageTimings[$Label] = $timer.Elapsed.TotalSeconds
    if ($env:GITHUB_STEP_SUMMARY)
    {
        "$Label : $([math]::Round($timer.Elapsed.TotalSeconds, 2)) s" | Add-Content $env:GITHUB_STEP_SUMMARY
    }
}

function Resolve-RepoPath {
    param([string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $RepoRoot $Path
}

function Get-ProjectVersion {
    $versions = Get-Content -LiteralPath (Join-Path $RepoRoot "contracts.json") -Raw | ConvertFrom-Json
    if ($Version -and $Version -ne $versions.softwareVersion)
    {
        throw "Version must match contracts.json: $($versions.softwareVersion)"
    }
    return $versions.softwareVersion
}

function Get-GitCommit
{
    if (-not (Test-CommandExists -Name "git"))
    {
        throw "Missing command: git"
    }

    Push-Location $RepoRoot
    try
    {
        return ((Invoke-NativeOutput -FilePath "git" -Arguments @("rev-parse", "HEAD") | Out-String).Trim())
    }
    finally
    {
        Pop-Location
    }
}

function Assert-CleanGitWorktree
{
    Push-Location $RepoRoot
    try
    {
        $status = ((Invoke-NativeOutput -FilePath "git" -Arguments @("status", "--porcelain") | Out-String).Trim())
        if ($status)
        {
            throw "Git worktree must be clean before release packaging. Commit or remove pending changes first."
        }
    }
    finally
    {
        Pop-Location
    }
}

function Copy-RequiredFile {
    param(
        [string]$Source,
        [string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Missing release file: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Test-IsWindows {
    return [System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT
}

function Get-ExecutableName {
    param([string]$BaseName)

    if (Test-IsWindows) {
        return "$BaseName.exe"
    }
    return $BaseName
}

function Get-DefaultPackagePlatform {
    try {
        if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)) {
            return "windows"
        }
        if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Linux)) {
            return "linux"
        }
        if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::OSX)) {
            return "macos"
        }
    }
    catch {
    }

    if (Test-IsWindows) {
        return "windows"
    }

    $platform = [System.Environment]::OSVersion.Platform.ToString().ToLowerInvariant()
    if ($platform -match "unix") {
        return "linux"
    }
    if ($platform -match "mac") {
        return "macos"
    }
    return $platform
}

function Get-DefaultPackageArch {
    $arch = ""
    try {
        $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    }
    catch {
        $arch = $env:PROCESSOR_ARCHITECTURE
    }

    switch -Regex ($arch) {
        "^(X64|AMD64)$" { return "x64" }
        "^X86$" { return "x86" }
        "^Arm64$" { return "arm64" }
        default { return $arch.ToLowerInvariant() }
    }
}

function Normalize-PackageToken {
    param(
        [string]$Value,
        [string]$Fallback,
        [string]$Label
    )

    $token = if ($Value) { $Value } else { $Fallback }
    $token = $token.Trim().ToLowerInvariant() -replace '[^a-z0-9_.-]', '-'
    $token = $token.Trim([char[]]"._-")
    if (-not $token) {
        throw "Invalid package $Label"
    }
    return $token
}

function Assert-FileExists {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing release file: $Path"
    }
}

function Test-CommandExists {
    param([string]$Name)

    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Write-InspectLine {
    param(
        [string]$Status,
        [string]$Message
    )

    Write-Host ("[{0}] {1}" -f $Status, $Message)
}

function Assert-InspectPath {
    param(
        [string]$Label,
        [string]$Path
    )

    if (Test-Path -LiteralPath $Path) {
        Write-InspectLine -Status "OK" -Message "$Label`: $Path"
        return
    }
    throw "Missing $Label`: $Path"
}

function Assert-InspectCommand {
    param([string]$Name)

    if (Test-CommandExists -Name $Name) {
        Write-InspectLine -Status "OK" -Message "command $Name"
        return
    }
    throw "Missing command: $Name"
}

function Write-BuildInfo
{
    param(
        [string]$Destination,
        [string]$VersionText,
        [string]$PackageName,
        [string]$Runtime,
        [string]$Platform,
        [string]$Arch,
        [string]$CommitHash,
        [string]$Configuration
    )

    $buildInfoPath = Join-Path $Destination "BUILD_INFO.txt"
    $versions = Get-Content -LiteralPath (Join-Path $RepoRoot "contracts.json") -Raw | ConvertFrom-Json
    $lines = @(
        "Name: Mosaicraft",
        "Version: $VersionText",
        "Package: $PackageName",
        "Platform: $Platform",
        "Arch: $Arch",
        "Runtime: $Runtime",
        "GpuValidation: $gpuValidation",
        "Configuration: $Configuration",
        "Commit: $CommitHash",
        "GeneratedAtUtc: $((Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"))"
    )
    foreach ($property in $versions.PSObject.Properties)
    {
        $lines += "Internal.$($property.Name): $($property.Value)"
    }
    Copy-Item -LiteralPath (Join-Path $RepoRoot "contracts.json") -Destination (Join-Path $Destination "contracts.json")
    if ($Vulkan -or $Metal)
    {
        $lines += "PortableGpuValidation: not-run"
        $lines += "PortableAutoSelection: disabled"
        $lines += "PortableKernelVersion: portable-v1"
        $lines += "ScoreModes: compatible;fast"
        $lines += "QualityDegradationLimit: 0.01"
        $lines += "FinalSelectionDifferenceLimit: 0.01"
    }
    if (-not $NoCuda)
    {
        foreach ($field in @('CudaArchitectures', 'CudaPtx', 'CudaToolkit', 'CudaRuntimeMode'))
        {
            $lines += "${field}: $($cudaBuildInfo[$field])"
        }
        foreach ($field in @('GpuValidationDevice', 'GpuValidationComputeCapability', 'GpuValidationDriver', 'GpuValidationRuntime'))
        {
            $value = if ($gpuValidation -eq 'passed') { $gpuProbeInfo[$field] } else { 'not-run' }
            $lines += "${field}: $value"
        }
    }
    Set-Content -LiteralPath $buildInfoPath -Value $lines -Encoding UTF8
    Assert-CudaBuildInfo (Read-BuildInfo $buildInfoPath)
    Assert-CudaPackageName (Read-BuildInfo $buildInfoPath) $PackageName -AllowUntestedRelease:$AllowUntestedRelease
}

function Read-BuildInfo
{
    param([string]$Path)

    $info = @{}
    foreach ($line in Get-Content -LiteralPath $Path)
    {
        $match = [regex]::Match($line, '^([^:]+):\s*(.*)$')
        if ($match.Success)
        {
            $info[$match.Groups[1].Value.Trim()] = $match.Groups[2].Value.Trim()
        }
    }
    return $info
}

function Assert-BuildInfo
{
    param(
        [string]$ExtractRoot,
        [string]$VersionText,
        [string]$PackageName,
        [string]$CommitHash
    )

    $buildInfoPath = Join-Path $ExtractRoot "BUILD_INFO.txt"
    Assert-FileExists -Path $buildInfoPath
    $info = Read-BuildInfo -Path $buildInfoPath

    if ($info["Version"] -ne $VersionText)
    {
        throw "BUILD_INFO version mismatch: $($info["Version"])"
    }
    if ($info["Package"] -ne $PackageName)
    {
        throw "BUILD_INFO package mismatch: $($info["Package"])"
    }
    if ($info["Commit"] -ne $CommitHash)
    {
        throw "BUILD_INFO commit mismatch: $($info["Commit"])"
    }
}

function Invoke-ReleaseInspection {
    param(
        [string]$VersionText,
        [string]$BuildPath,
        [string]$Configuration,
        [string]$Toolchain,
        [string]$PackagePath,
        [switch]$NoCuda,
        [switch]$SkipBuild
    )

    Invoke-Step "Inspect release environment" {
        Write-InspectLine -Status "INFO" -Message "repo: $RepoRoot"
        Write-InspectLine -Status "INFO" -Message "version: $VersionText"
        Write-InspectLine -Status "INFO" -Message "build: $BuildPath"
        Write-InspectLine -Status "INFO" -Message "configuration: $Configuration"
        Write-InspectLine -Status "INFO" -Message "package: $PackagePath"
        Write-InspectLine -Status "INFO" -Message ("cuda: " + ($(if ($NoCuda) { "disabled" } else { "enabled" })))

        Assert-InspectCommand -Name "cmake"
        Assert-InspectCommand -Name "ctest"
        Assert-InspectCommand -Name "git"
        if (Test-CommandExists -Name "pwsh") {
            Write-InspectLine -Status "OK" -Message "command pwsh"
        } else {
            Assert-InspectCommand -Name "powershell"
        }

        if ($Toolchain) {
            Assert-InspectPath -Label "vcpkg toolchain" -Path $Toolchain
        } else {
            Write-InspectLine -Status "WARN" -Message "vcpkg toolchain not set; configure must find dependencies another way"
        }

        foreach ($required in @(
            "CMakeLists.txt",
            "README.md",
            "docs\README.md",
            "scripts\ReleaseDocumentation.ps1",
            "docs\reference\api.md",
            "docs\development\portable-gpu.md",
            "docs\performance\portable-gpu-20261113.json",
            "docs\releases.md",
            "LICENSE",
            "third_party_versions.txt",
            "tests\webui_smoke.ps1"
        )) {
            Assert-InspectPath -Label $required -Path (Join-Path $RepoRoot $required)
        }
        Assert-ReleaseVersionSummary -RepoRoot $RepoRoot -VersionText $VersionText
        Write-InspectLine -Status "OK" -Message "version summary: v$VersionText"

        if (-not $NoCuda -and -not $env:CUDAToolkit_ROOT) {
            Write-InspectLine -Status "WARN" -Message "Set CUDAToolkit_ROOT explicitly to CUDA 13.5 before configuring"
        } elseif (-not $NoCuda) {
            Assert-InspectPath -Label 'explicit CUDA SDK' -Path $env:CUDAToolkit_ROOT
        }

        $binDir = Join-Path $BuildPath "$Configuration\bin"
        if (Test-Path -LiteralPath $binDir) {
            Write-InspectLine -Status "OK" -Message "build output directory: $binDir"
            $cliName = Get-ExecutableName -BaseName "mosaicraft"
            $webUiName = Get-ExecutableName -BaseName "MosaicraftWebUI"
            Assert-InspectPath -Label $cliName -Path (Join-Path $binDir $cliName)
            Assert-InspectPath -Label $webUiName -Path (Join-Path $binDir $webUiName)
            Assert-InspectPath -Label "index.html" -Path (Join-Path $binDir "index.html")
        } elseif ($SkipBuild) {
            throw "Build output directory is required when -SkipBuild is set: $binDir"
        } else {
            Write-InspectLine -Status "WARN" -Message "build output directory not found yet: $binDir"
        }
    }
}

$versionText = Get-ProjectVersion
Assert-ReleaseVersionSummary -RepoRoot $RepoRoot -VersionText $versionText
$outputPath = if ($OutputDir) { Resolve-RepoPath $OutputDir } else { $RepoRoot }
$packagePlatform = Normalize-PackageToken -Value $PackagePlatform -Fallback (Get-DefaultPackagePlatform) -Label "platform"
$packageArch = Normalize-PackageToken -Value $PackageArch -Fallback (Get-DefaultPackageArch) -Label "arch"
$packageRuntime = if ($Metal) { 'metal' } elseif ($Vulkan) { if ($NoCuda) { 'vulkan' } else { 'cuda-vulkan' } } elseif ($NoCuda) { 'cpu-only' } else { 'cuda' }
if ($Vulkan -and $Metal) { throw 'Choose Vulkan or Metal for one platform package' }
$hostPlatform = Get-DefaultPackagePlatform
$hostArch = Get-DefaultPackageArch
if ($Metal -and ($hostPlatform -ne 'macos' -or -not $NoCuda)) { throw 'Metal requires macOS and -NoCuda' }
if ($Vulkan -and $hostPlatform -eq 'macos') { throw 'Use native Metal for macOS candidate packages' }
if ($hostPlatform -eq 'macos' -and ($hostArch -ne 'arm64' -or -not $NoCuda))
{
    throw 'macOS packaging supports ARM64 CPU-only builds'
}
if ($hostPlatform -notin @('windows', 'linux', 'macos') -or ($hostPlatform -ne 'macos' -and $hostArch -ne 'x64'))
{
    throw 'Packaging requires a supported native x64 Windows/Linux or ARM64 macOS host'
}
$configurePreset = if ($hostPlatform -eq 'macos') { 'macos-arm64-cpu' } else { "$hostPlatform-$(if ($NoCuda) { 'cpu' } else { 'cuda' })" }
if ($Metal) { $configurePreset = 'macos-metal' }
elseif ($Vulkan) { $configurePreset = "$hostPlatform-$(if ($NoCuda) { 'vulkan' } else { 'cuda-vulkan' })" }
if (-not $BuildDir) { $BuildDir = "build/$configurePreset" }
$buildPath = Resolve-RepoPath $BuildDir
$gpuValidation = if ($NoCuda) { 'not-applicable' } elseif ($CudaValidation -eq 'BuildOnly') { 'not-run' } else { 'passed' }
if ($NoCuda -and ($Vulkan -or $Metal)) { $gpuValidation = 'not-run' }
if (-not $NoCuda -and $CudaValidation -eq 'Required' -and ($SkipTests -or $SkipSmoke -or $SkipArchiveValidation))
{
    throw 'CUDA Required packaging cannot skip tests, smoke or archive validation'
}
if ($AllowUntestedRelease)
{
    if ($NoCuda -or $CudaValidation -ne 'BuildOnly' -or
        -not ($SkipTests -and $SkipSmoke -and $SkipArchiveValidation) -or
        $packagePlatform -notin @('windows', 'linux') -or $packageArch -ne 'x64' -or $PackageSuffix)
    {
        throw 'AllowUntestedRelease requires a Windows/Linux x64 CUDA BuildOnly package without suffix and all tests, smoke and archive validation skipped'
    }
}
if ($AllowUnvalidatedCudaRelease)
{
    if ($NoCuda -or $CudaValidation -ne 'BuildOnly' -or $packagePlatform -ne 'linux' -or
        $packageArch -ne 'x64' -or (Get-DefaultPackagePlatform) -ne 'linux' -or
        $ArchiveFormat -notin @('auto', 'tar.gz') -or $PackageSuffix)
    {
        throw 'AllowUnvalidatedCudaRelease requires a native Linux x64 CUDA BuildOnly tar.gz package without suffix'
    }
    if (($SkipTests -or $SkipSmoke -or $SkipArchiveValidation) -and -not $AllowUntestedRelease)
    {
        throw 'Linux CUDA release packaging cannot skip tests, smoke or archive validation'
    }
}
$gitCommit = Get-GitCommit

if (-not $ToolchainFile -and $env:VCPKG_ROOT) {
    $candidateToolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"
    if (Test-Path -LiteralPath $candidateToolchain) {
        $ToolchainFile = $candidateToolchain
    }
}

$suffix = ""
if ($PackageSuffix) {
    $suffix = "_" + ($PackageSuffix -replace '[^A-Za-z0-9_.-]', '-')
}
$packageName = "Mosaicraft_v${versionText}_${packagePlatform}-${packageArch}_${packageRuntime}${suffix}"
if (-not $NoCuda -and $CudaValidation -eq 'BuildOnly' -and -not $AllowUnvalidatedCudaRelease -and -not $AllowUntestedRelease) { $packageName += '_candidate' }
if (($Vulkan -or $Metal) -and -not $packageName.EndsWith('_candidate')) { $packageName += '_candidate' }
if ($ArchiveFormat -eq 'auto') { $ArchiveFormat = if (Test-IsWindows) { 'zip' } else { 'tar.gz' } }
$archiveName = "$packageName.$ArchiveFormat"
$archivePath = Join-Path $outputPath $archiveName
$tempRoot = [System.IO.Path]::GetTempPath()
$packageRoot = Join-Path $tempRoot ("${packageName}_pkg_" + [System.Guid]::NewGuid().ToString("N"))
$extractRoot = Join-Path $tempRoot ("${packageName}_extract_" + [System.Guid]::NewGuid().ToString("N"))

if ($InspectOnly) {
    Invoke-ReleaseInspection `
        -VersionText $versionText `
        -BuildPath $buildPath `
        -Configuration $Configuration `
        -Toolchain $ToolchainFile `
        -PackagePath $archivePath `
        -NoCuda:$NoCuda `
        -SkipBuild:$SkipBuild
    return
}

New-Item -ItemType Directory -Force -Path $outputPath | Out-Null

try {
    Assert-CleanGitWorktree

    if (-not $SkipConfigure) {
        Invoke-Step "Configure" {
            # Source and release builds share presets; explicit overrides take precedence.
            $configureArgs = @('--preset', $configurePreset, '-S', $RepoRoot, '-B', $buildPath, '-DMOSAICRAFT_BUNDLE_RUNTIME=ON')
            if ($ToolchainFile) {
                $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
                if ($env:VCPKG_OVERLAY_TRIPLETS) {
                    $configureArgs += "-DVCPKG_OVERLAY_TRIPLETS=$env:VCPKG_OVERLAY_TRIPLETS"
                }
            }
            if (-not $NoCuda -and $env:CUDAToolkit_ROOT) { $configureArgs += "-DCUDAToolkit_ROOT=$env:CUDAToolkit_ROOT" }
            if ($env:VCPKG_DEFAULT_TRIPLET)
            {
                $configureArgs += "-DVCPKG_TARGET_TRIPLET=$env:VCPKG_DEFAULT_TRIPLET"
                $configureArgs += "-DVCPKG_HOST_TRIPLET=$env:VCPKG_DEFAULT_HOST_TRIPLET"
            }
            if ($env:MOSAICRAFT_VCPKG_INSTALLED_DIR)
            {
                $configureArgs += "-DVCPKG_INSTALLED_DIR=$env:MOSAICRAFT_VCPKG_INSTALLED_DIR"
                $configureArgs += '-DVCPKG_MANIFEST_INSTALL=OFF'
            }
            Invoke-Native -FilePath "cmake" -Arguments $configureArgs
        }
    }

    if (-not $SkipBuild) {
        Invoke-Step "Build release targets" {
            $buildTargets = @("mosaicraft", "MosaicraftWebUI")
            if (-not $SkipTests)
            {
                $buildTargets += @("mosaicraft_tests", "mosaicraft_regression_tests")
            }
            Invoke-Native -FilePath "cmake" -Arguments (@(
                "--build", $buildPath,
                "--config", $Configuration,
                "--target"
            ) + $buildTargets + "--parallel")
        }
    }

    $cache = Get-Content (Join-Path $buildPath 'CMakeCache.txt') -Raw
    $expectedCuda = if ($NoCuda) { 'OFF' } else { 'ON' }
    if ($cache -notmatch "(?m)^MOSAICRAFT_CUDA:BOOL=$expectedCuda`r?$")
    {
        throw 'Build directory CUDA configuration does not match the requested package'
    }
    foreach ($backend in @('VULKAN', 'METAL'))
    {
        $enabled = if (($backend -eq 'VULKAN' -and $Vulkan) -or ($backend -eq 'METAL' -and $Metal)) { 'ON' } else { 'OFF' }
        if ($cache -notmatch "(?m)^MOSAICRAFT_${backend}:BOOL=$enabled`r?$")
        {
            throw "Build directory $backend configuration does not match the requested package"
        }
    }
    if (-not $NoCuda)
    {
        $cudaBuildInfo = Read-BuildInfo (Join-Path $buildPath 'cuda-architectures.txt')
        $cudaLock = Get-Content (Join-Path $RepoRoot 'cmake/cuda-13.5.json') -Raw | ConvertFrom-Json
        $expectedRuntimeMode = if (Test-IsWindows) { 'hybrid' } else { 'shared' }
        if ($cudaBuildInfo.CudaToolkit -ne $cudaLock.components.cuda_nvcc.version -or
            $cudaBuildInfo.CudaRuntimeMode -ne $expectedRuntimeMode)
        {
            throw 'Build directory must use the locked CUDA 13.5 toolkit/runtime; configure a fresh directory'
        }
        $inspector = Join-Path (Split-Path $cudaBuildInfo.Nvcc) (Get-ExecutableName 'cuobjdump')
        Invoke-Step 'Verify universal CUDA images' {
            foreach ($program in @('mosaicraft', 'MosaicraftWebUI'))
            {
                & (Join-Path $PSScriptRoot 'Assert-CudaArchitectures.ps1') -BuildDir $buildPath `
                    -Executable (Join-Path $buildPath "$Configuration/bin/$(Get-ExecutableName $program)") `
                    -Cuobjdump $inspector -RequireUniversal
            }
        }
    }
    if (-not $NoCuda -and $CudaValidation -eq 'Required')
    {
        $probePath = Join-Path $buildPath "$Configuration/bin/$(Get-ExecutableName 'mosaicraft_tests')"
        $probe = Invoke-NativeOutput -FilePath $probePath -Arguments @('--require-cuda') | Out-String
        if ($probe -notmatch '(?m)^GPU validation passed\r?$') { throw 'GPU validation probe did not confirm usable CUDA kernels' }
        foreach ($line in $probe -split "`n")
        {
            if ($line -match '^(GpuValidation[^:]+):\s*(.*)$') { $gpuProbeInfo[$Matches[1]] = $Matches[2].Trim() }
        }
        foreach ($field in @('GpuValidationDevice', 'GpuValidationComputeCapability', 'GpuValidationDriver', 'GpuValidationRuntime'))
        {
            if (-not $gpuProbeInfo[$field]) { throw "GPU probe omitted $field" }
        }
    }

    if (-not $SkipTests) {
        Invoke-Step "Run CTest" {
            $previousRequirement = $env:MOSAICRAFT_REQUIRE_CUDA
            $env:MOSAICRAFT_REQUIRE_CUDA = if (-not $NoCuda -and $CudaValidation -eq 'Required') { '1' } else { '0' }
            try
            {
                Invoke-Native -FilePath "ctest" -Arguments @(
                    "--test-dir", $buildPath,
                    "-C", $Configuration,
                    "--output-on-failure"
                )
            }
            finally { $env:MOSAICRAFT_REQUIRE_CUDA = $previousRequirement }
        }
    }

    if (-not $SkipSmoke) {
        Invoke-Step "Run Web UI/API smoke" {
            Invoke-Native -FilePath "cmake" -Arguments @(
                "--build", $buildPath,
                "--config", $Configuration,
                "--target", "mosaicraft_webui_smoke"
            )
        }
    }

    Invoke-Step "Package $archiveName" {
        $binDir = Join-Path $buildPath "$Configuration\bin"
        if (-not (Test-Path -LiteralPath $binDir)) {
            throw "Build output directory not found: $binDir"
        }

        New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null

        Invoke-Native -FilePath 'cmake' -Arguments @('--install', $buildPath,
            '--config', $Configuration, '--prefix', $packageRoot)

        Copy-RequiredFile -Source (Join-Path $RepoRoot "README.md") -Destination $packageRoot
        Copy-RequiredFile -Source (Join-Path $RepoRoot "CMakeUserPresets.example.json") -Destination $packageRoot
        Copy-ReleaseDocumentation -RepoRoot $RepoRoot -Destination $packageRoot
        Copy-RequiredFile -Source (Join-Path $RepoRoot "LICENSE") -Destination $packageRoot
        Copy-RequiredFile -Source (Join-Path $RepoRoot "third_party_versions.txt") -Destination $packageRoot
        foreach ($dependency in @('miniz','picosha2')) {
            $licenseTarget = Join-Path $packageRoot "licenses/$dependency"
            New-Item -ItemType Directory -Force -Path $licenseTarget | Out-Null
            Copy-RequiredFile -Source (Join-Path $RepoRoot "third_party/$dependency/LICENSE") -Destination $licenseTarget
            Copy-RequiredFile -Source (Join-Path $RepoRoot "third_party/$dependency/SOURCE.md") -Destination $licenseTarget
        }
        Write-BuildInfo `
            -Destination $packageRoot `
            -VersionText $versionText `
            -PackageName $archiveName `
            -Runtime $packageRuntime `
            -Platform $packagePlatform `
            -Arch $packageArch `
            -CommitHash $gitCommit `
            -Configuration $Configuration

        if (Test-Path -LiteralPath $archivePath) {
            Remove-Item -LiteralPath $archivePath -Force
        }
        Compress-ReleaseArchive -Root $packageRoot -Destination $archivePath
    }

    if (-not $SkipArchiveValidation) {
        Invoke-Step "Validate archive" {
            New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
            Expand-ReleaseArchive -Path $archivePath -Destination $extractRoot

            $cliName = Get-ExecutableName -BaseName "mosaicraft"
            $webUiName = Get-ExecutableName -BaseName "MosaicraftWebUI"
            $cliPath = Join-Path $extractRoot $cliName
            $webUiPath = Join-Path $extractRoot $webUiName
            Assert-FileExists -Path $cliPath
            Assert-FileExists -Path $webUiPath
            Assert-ReleaseDocumentation -Root $extractRoot -SourceRoot $RepoRoot
            Assert-BuildInfo -ExtractRoot $extractRoot -VersionText $versionText -PackageName $archiveName -CommitHash $gitCommit
            $versionOutput = (& $cliPath --version).Trim()
            if ($versionOutput -ne "Mosaicraft $versionText") {
                throw "Unexpected CLI version output: $versionOutput"
            }

            if (-not $SkipSmoke) {
                $powershellCommand = if (Get-Command pwsh -ErrorAction SilentlyContinue) { "pwsh" } else { "powershell" }
                $smokeArgs = @("-NoProfile")
                if (Test-IsWindows) {
                    $smokeArgs += @("-ExecutionPolicy", "Bypass")
                }
                $smokeArgs += @(
                    "-File", (Join-Path $RepoRoot "tests\webui_smoke.ps1"),
                    "-WebUiExe", $webUiPath,
                    "-TimeoutSeconds", "90"
                )
                Invoke-Native -FilePath $powershellCommand -Arguments $smokeArgs
            }
        }
    }

    Invoke-Step "Release package summary" {
        $item = Get-Item -LiteralPath $archivePath
        $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath
        Set-Content -LiteralPath "$archivePath.sha256" -Value "$($hash.Hash)  $archiveName" -Encoding ascii
        Write-Host "Package: $($item.FullName)"
        Write-Host "Size: $($item.Length)"
        Write-Host "SHA256: $($hash.Hash)"
        if ($env:GITHUB_OUTPUT) { "archive-path=$archivePath" | Add-Content $env:GITHUB_OUTPUT }
    }
    $script:stageTimings | ConvertTo-Json | Set-Content (Join-Path $buildPath 'release-timings.json')
}
finally {
    if (-not $KeepWorkspace) {
        $allowedRoot = ([System.IO.Path]::GetFullPath($tempRoot)).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
        foreach ($workspacePath in @($packageRoot, $extractRoot)) {
            if (Test-Path -LiteralPath $workspacePath) {
                $resolved = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $workspacePath).Path)
                if (-not $resolved.StartsWith($allowedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
                    throw "Refusing to remove a release workspace outside the temporary directory: $resolved"
                }
                Remove-Item -LiteralPath $resolved -Recurse -Force
            }
        }
    } else {
        Write-Host "Package workspace: $packageRoot"
        Write-Host "Extract workspace: $extractRoot"
    }
}
