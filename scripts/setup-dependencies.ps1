param(
    [ValidateSet("Prepare", "Install")][string]$Mode = "Install",
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [ValidateSet("x64-windows-release", "x64-linux-release", "arm64-osx-release", "x64-osx-release")]
    [string]$Triplet = "x64-windows-release",
    [string]$CacheNamespace = "deps-v1"
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$baseline = (Get-Content (Join-Path $repo "vcpkg.json") -Raw | ConvertFrom-Json).'builtin-baseline'
if (-not $VcpkgRoot) { throw "Pass -VcpkgRoot or set VCPKG_ROOT to the pinned vcpkg checkout." }
$VcpkgRoot = (Resolve-Path $VcpkgRoot).Path
$revision = (& git -C $VcpkgRoot rev-parse HEAD | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $revision -ne $baseline) { throw "vcpkg must be at $baseline; the shared checkout will not be changed." }
if ($CacheNamespace -notmatch '^[a-zA-Z0-9_.-]+$') { throw "Invalid cache namespace." }
$isWindowsHost = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
$isMac = [Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([Runtime.InteropServices.OSPlatform]::OSX)
$platform = if ($isWindowsHost) { "windows" } elseif ($isMac) { "macos" } else { "linux" }
$generator = if ($isWindowsHost) { "Visual Studio 18 2026" } else { "Ninja" }
$architecture = $Triplet.Split("-")[0]
$work = Join-Path $repo "build/dependency-state/$Triplet"
$installed = Join-Path $repo "build/dependencies"
$cache = if ($env:RUNNER_TEMP) { Join-Path $env:RUNNER_TEMP "mosaicraft-vcpkg-cache" } else { Join-Path $repo "build/dependency-cache" }
New-Item -ItemType Directory -Force -Path $work, $cache, $installed | Out-Null

function Invoke-Checked([string]$Program, [string[]]$Arguments)
{
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
function Export-Variable([string]$Name, [string]$Value)
{
    [Environment]::SetEnvironmentVariable($Name, $Value, "Process")
    if ($env:GITHUB_ENV) { "$Name=$Value" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8 }
}
$trusted = ($env:GITHUB_EVENT_NAME -ne "pull_request") -and
    (($env:GITHUB_REF -eq "refs/heads/main") -or ($env:GITHUB_REF -like "refs/tags/v*"))
$access = if (-not $env:GITHUB_ACTIONS -or $trusted) { "readwrite" } else { "read" }
Export-Variable "VCPKG_ROOT" $VcpkgRoot
Export-Variable "VCPKG_OVERLAY_TRIPLETS" (Join-Path $repo "cmake/triplets")
Export-Variable "VCPKG_DEFAULT_TRIPLET" $Triplet
Export-Variable "VCPKG_DEFAULT_HOST_TRIPLET" $Triplet
Export-Variable "VCPKG_BINARY_SOURCES" "clear;files,$cache,$access"
Export-Variable "VCPKG_DOWNLOADS" (Join-Path $repo "build/vcpkg-downloads")
New-Item -ItemType Directory -Force -Path $env:VCPKG_DOWNLOADS | Out-Null
Export-Variable "VCPKG_MAX_CONCURRENCY" $(if ($isMac -and $architecture -eq "arm64") { "2" } else { "4" })
Export-Variable "MOSAICRAFT_VCPKG_INSTALLED_DIR" $installed
Export-Variable "MOSAICRAFT_CMAKE_GENERATOR" $generator
Export-Variable "CMAKE_BUILD_PARALLEL_LEVEL" $env:VCPKG_MAX_CONCURRENCY
if ($isMac)
{
    Export-Variable "MACOSX_DEPLOYMENT_TARGET" "13.0"
    # Remove inherited Homebrew search flags; the Intel triplet also pins
    # OpenCV's quoted codec includes ahead of Clang's system directories.
    foreach ($name in @('CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'OBJC_INCLUDE_PATH',
        'LIBRARY_PATH', 'CPPFLAGS', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS'))
    {
        Export-Variable $name ''
    }
}
$vcpkg = Join-Path $VcpkgRoot $(if ($isWindowsHost) { "vcpkg.exe" } else { "vcpkg" })

if ($Mode -eq "Prepare")
{
    if (-not (Test-Path $vcpkg))
    {
        $bootstrap = Join-Path $VcpkgRoot $(if ($isWindowsHost) { "bootstrap-vcpkg.bat" } else { "bootstrap-vcpkg.sh" })
        for ($attempt = 1; $attempt -le 3; $attempt++)
        {
            & $bootstrap -disableMetrics
            if ($LASTEXITCODE -eq 0) { break }
            if ($attempt -eq 3) { throw 'vcpkg bootstrap failed after three attempts' }
            Start-Sleep -Seconds 5
        }
    }
    if (-not $isWindowsHost -and -not (Get-Command ninja -ErrorAction SilentlyContinue))
    {
        Invoke-Checked "python3" @("-m", "pip", "install", "--user", "ninja")
    }
    $probe = Join-Path $work "probe"
    New-Item -ItemType Directory -Force -Path $probe | Out-Null
    @'
cmake_minimum_required(VERSION 3.20)
project(MosaicraftToolchainProbe LANGUAGES CXX)
file(WRITE "${CMAKE_BINARY_DIR}/compiler.txt" "${CMAKE_CXX_COMPILER_ID}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_CXX_COMPILER}|${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}|${CMAKE_OSX_SYSROOT}")
'@ | Set-Content (Join-Path $probe "CMakeLists.txt") -Encoding utf8
    $arguments = @("-S", $probe, "-B", (Join-Path $probe "build"), "-G", $generator)
    if ($isWindowsHost) { $arguments += @("-A", "x64") }
    if ($isMac) { $arguments += "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0" }
    Invoke-Checked "cmake" $arguments
    $toolchain = Get-Content (Join-Path $probe "build/compiler.txt") -Raw
    $toolchain += (& cmake --version | Select-Object -First 1)
    $toolchain += (& $vcpkg version | Out-String)
    if ($isMac) { $toolchain += (& xcodebuild -version | Out-String); $toolchain += (& xcrun --show-sdk-version | Out-String) }
    $sha = [Security.Cryptography.SHA256]::Create()
    try
    {
        $fingerprint = [Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($toolchain))).ToLowerInvariant()
        $files = @((Join-Path $repo "vcpkg.json"), (Join-Path $repo "cmake/triplets/$Triplet.cmake"))
        $configuration = ($files | ForEach-Object { Get-Content -LiteralPath $_ -Raw }) -join [Environment]::NewLine
        $dependencyHash = [Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($configuration))).ToLowerInvariant()
    }
    finally { $sha.Dispose() }
    $prefix = "$CacheNamespace-$platform-$architecture-$fingerprint-$baseline-"
    $key = "$prefix$dependencyHash"
    @{ cacheKey=$key; cachePrefix=$prefix; triplet=$Triplet; toolchain=$toolchain; baseline=$baseline } |
        ConvertTo-Json | Set-Content (Join-Path $work "toolchain.json") -Encoding utf8
    if ($env:GITHUB_OUTPUT)
    {
        "cache-key=$key" | Add-Content $env:GITHUB_OUTPUT
        "cache-prefix=$prefix" | Add-Content $env:GITHUB_OUTPUT
        "cache-path=$cache" | Add-Content $env:GITHUB_OUTPUT
        "cache-write=$($trusted.ToString().ToLowerInvariant())" | Add-Content $env:GITHUB_OUTPUT
    }
    Write-Host "Dependency cache key: $key"
    return
}
if (-not (Test-Path $vcpkg)) { throw "Run Prepare first to bootstrap vcpkg." }
$timer = [Diagnostics.Stopwatch]::StartNew()
$log = Join-Path $work "install.log"
Set-Content -LiteralPath $log -Value ''
$arguments = @("install", "--x-manifest-root=$repo", "--triplet=$Triplet", "--host-triplet=$Triplet",
    "--x-install-root=$installed", "--overlay-triplets=$env:VCPKG_OVERLAY_TRIPLETS",
    "--x-buildtrees-root=$(Join-Path $repo 'build/vcpkg-buildtrees')",
    "--x-packages-root=$(Join-Path $repo 'build/vcpkg-packages')", "--disable-metrics")
for ($attempt = 1; $attempt -le 3; $attempt++)
{
    $attemptLog = Join-Path $work "install-attempt-$attempt.log"
    & $vcpkg @arguments 2>&1 | Tee-Object -FilePath $attemptLog
    $installExitCode = $LASTEXITCODE
    Get-Content $attemptLog | Add-Content $log
    if ($installExitCode -eq 0) { break }
    $failure = Get-Content $attemptLog -Raw
    # Retry transient source-download failures, never conceal a compiler failure.
    if ($attempt -eq 3 -or $failure -notmatch 'response code (429|50[0234])|timed out|Could not resolve host')
    {
        if ($isMac)
        {
            # Keep actual header provenance in the job log when a native codec
            # ABI fails to link; CMake's reported include directory is insufficient.
            $includeDir = Join-Path $installed "$Triplet/include"
            $jpegLibrary = Join-Path $installed "$Triplet/lib/libjpeg.dylib"
            Write-Host 'JPEG ABI diagnostics: installed configuration'
            if (Test-Path "$includeDir/jconfig.h") { Get-Content "$includeDir/jconfig.h" | Select-String 'JPEG_LIB_VERSION|LIBJPEG_TURBO_VERSION' }
            if (Test-Path $jpegLibrary) { & nm -gU $jpegLibrary | Select-String 'jpeg_default_qtables|jpeg_CreateCompress' }
            $opencvBuild = Join-Path $repo "build/vcpkg-buildtrees/opencv4/$Triplet-rel"
            if (Test-Path "$opencvBuild/build.ninja")
            {
                & ninja -C $opencvBuild -t deps 'modules/imgcodecs/CMakeFiles/opencv_imgcodecs.dir/src/grfmt_jpeg.cpp.o' |
                    Select-String 'jpeglib.h|jconfig.h'
            }
        }
        throw "vcpkg install failed: $installExitCode"
    }
    Start-Sleep -Seconds 5
}
$timer.Stop()
$text = Get-Content $log -Raw
if ($text -match 'Installing [0-9]+/[0-9]+ (protobuf|abseil|flatbuffers)' -or $text -match 'Building [^ ]+-dbg')
{
    throw "Unexpected heavyweight or Debug dependency in the minimal Release configuration."
}
$result = @{ seconds=$timer.Elapsed.TotalSeconds; triplet=$Triplet; baseline=$baseline;
    restoredPackages=([regex]::Match($text, 'Restored ([0-9]+) package')).Groups[1].Value }
$result | ConvertTo-Json | Set-Content (Join-Path $work "install-summary.json") -Encoding utf8
if ($env:GITHUB_STEP_SUMMARY)
{
    "Dependencies $Triplet : $([math]::Round($timer.Elapsed.TotalSeconds, 1)) s; restored packages: $($result.restoredPackages)" |
        Add-Content $env:GITHUB_STEP_SUMMARY
}
