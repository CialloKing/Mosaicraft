param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$Version = "",
    [string]$ToolchainFile = "",
    [string]$PackagePlatform = "",
    [string]$PackageArch = "",
    [string]$PackageSuffix = "",
    [string]$OutputDir = "",
    [switch]$NoCuda,
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

function Invoke-Step {
    param(
        [string]$Label,
        [scriptblock]$Action
    )

    Write-Host ""
    Write-Host "==> $Label"
    & $Action
}

function Resolve-RepoPath {
    param([string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $RepoRoot $Path
}

function Get-ProjectVersion {
    if ($Version) {
        return $Version
    }

    $versionHeader = Join-Path $RepoRoot "core\Version.h"
    $content = Get-Content -LiteralPath $versionHeader -Raw
    $match = [regex]::Match($content, 'kVersion\s*=\s*"([^"]+)"')
    if (-not $match.Success) {
        throw "Unable to read version from $versionHeader"
    }
    return $match.Groups[1].Value
}

function Find-PresetToolchainFile {
    $presetPath = Join-Path $RepoRoot "CMakePresets.json"
    if (-not (Test-Path -LiteralPath $presetPath)) {
        return ""
    }

    try {
        $presets = Get-Content -LiteralPath $presetPath -Raw | ConvertFrom-Json
        foreach ($preset in @($presets.configurePresets)) {
            if ($preset.name -ne "default") {
                continue
            }
            $toolchain = $preset.cacheVariables.CMAKE_TOOLCHAIN_FILE
            if ($toolchain) {
                return [string]$toolchain
            }
        }
    }
    catch {
        Write-Warning "Unable to read CMakePresets.json: $($_.Exception.Message)"
    }
    return ""
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

function Assert-EncyclopediaVersionEntry {
    param([string]$VersionText)

    $encyclopedia = Join-Path $RepoRoot "docs\ENCYCLOPEDIA.md"
    $content = Get-Content -LiteralPath $encyclopedia -Raw
    $escapedVersion = [regex]::Escape($VersionText)
    if ($content -match "(^|[^0-9])v?$escapedVersion([^0-9]|$)") {
        Write-InspectLine -Status "OK" -Message "encyclopedia version entry: v$VersionText"
        return
    }
    throw "Missing encyclopedia version entry: v$VersionText"
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
            "docs\API.md",
            "docs\ENCYCLOPEDIA.md",
            "LICENSE",
            "third_party_versions.txt",
            "tests\webui_smoke.ps1"
        )) {
            Assert-InspectPath -Label $required -Path (Join-Path $RepoRoot $required)
        }
        Assert-EncyclopediaVersionEntry -VersionText $VersionText

        if (-not $NoCuda -and -not $env:CUDA_PATH) {
            Write-InspectLine -Status "WARN" -Message "CUDA_PATH is not set; CUDA configure may still work if CMake can locate the toolkit"
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
$buildPath = Resolve-RepoPath $BuildDir
$outputPath = if ($OutputDir) { Resolve-RepoPath $OutputDir } else { $RepoRoot }
$packagePlatform = Normalize-PackageToken -Value $PackagePlatform -Fallback (Get-DefaultPackagePlatform) -Label "platform"
$packageArch = Normalize-PackageToken -Value $PackageArch -Fallback (Get-DefaultPackageArch) -Label "arch"
$packageRuntime = if ($NoCuda) { "cpu-only" } else { "cuda" }

if (-not $ToolchainFile -and $env:VCPKG_ROOT) {
    $candidateToolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"
    if (Test-Path -LiteralPath $candidateToolchain) {
        $ToolchainFile = $candidateToolchain
    }
}
if (-not $ToolchainFile) {
    $presetToolchain = Find-PresetToolchainFile
    if ($presetToolchain -and (Test-Path -LiteralPath $presetToolchain)) {
        $ToolchainFile = $presetToolchain
    }
}

$suffix = ""
if ($PackageSuffix) {
    $suffix = "_" + ($PackageSuffix -replace '[^A-Za-z0-9_.-]', '-')
}
$packageName = "Mosaicraft_v${versionText}_${packagePlatform}-${packageArch}_${packageRuntime}${suffix}"
$zipPath = Join-Path $outputPath "$packageName.zip"
$tempRoot = [System.IO.Path]::GetTempPath()
$packageRoot = Join-Path $tempRoot ("${packageName}_pkg_" + [System.Guid]::NewGuid().ToString("N"))
$extractRoot = Join-Path $tempRoot ("${packageName}_extract_" + [System.Guid]::NewGuid().ToString("N"))

if ($InspectOnly) {
    Invoke-ReleaseInspection `
        -VersionText $versionText `
        -BuildPath $buildPath `
        -Configuration $Configuration `
        -Toolchain $ToolchainFile `
        -PackagePath $zipPath `
        -NoCuda:$NoCuda `
        -SkipBuild:$SkipBuild
    return
}

New-Item -ItemType Directory -Force -Path $outputPath | Out-Null

try {
    if (-not $SkipConfigure) {
        Invoke-Step "Configure" {
            $configureArgs = @("-S", $RepoRoot, "-B", $buildPath)
            if ($ToolchainFile) {
                $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
            }
            if ($NoCuda) {
                $configureArgs += "-DMOSAICRAFT_CUDA=OFF"
            }
            Invoke-Native -FilePath "cmake" -Arguments $configureArgs
        }
    }

    if (-not $SkipBuild) {
        Invoke-Step "Build release targets" {
            Invoke-Native -FilePath "cmake" -Arguments @(
                "--build", $buildPath,
                "--config", $Configuration,
                "--target", "mosaicraft", "MosaicraftWebUI", "mosaicraft_tests", "mosaicraft_regression_tests"
            )
        }
    }

    if (-not $SkipTests) {
        Invoke-Step "Run CTest" {
            Invoke-Native -FilePath "ctest" -Arguments @(
                "--test-dir", $buildPath,
                "-C", $Configuration,
                "--output-on-failure"
            )
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

    Invoke-Step "Package $packageName.zip" {
        $binDir = Join-Path $buildPath "$Configuration\bin"
        if (-not (Test-Path -LiteralPath $binDir)) {
            throw "Build output directory not found: $binDir"
        }

        New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null

        $cliName = Get-ExecutableName -BaseName "mosaicraft"
        $webUiName = Get-ExecutableName -BaseName "MosaicraftWebUI"
        Copy-RequiredFile -Source (Join-Path $binDir $cliName) -Destination $packageRoot
        Copy-RequiredFile -Source (Join-Path $binDir $webUiName) -Destination $packageRoot
        Copy-RequiredFile -Source (Join-Path $binDir "index.html") -Destination $packageRoot

        Get-ChildItem -LiteralPath $binDir -File |
            Where-Object { $_.Name -match '\.(dll|so|dylib)(\..*)?$' } |
            ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $packageRoot -Force }

        if (Test-IsWindows) {
            foreach ($runtime in @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll")) {
                $runtimePath = Join-Path $env:WINDIR "System32\$runtime"
                if (Test-Path -LiteralPath $runtimePath) {
                    Copy-Item -LiteralPath $runtimePath -Destination $packageRoot -Force
                } else {
                    Write-Warning "VC runtime not found: $runtimePath"
                }
            }
        }

        Copy-RequiredFile -Source (Join-Path $RepoRoot "README.md") -Destination $packageRoot
        Copy-RequiredFile -Source (Join-Path $RepoRoot "docs\API.md") -Destination (Join-Path $packageRoot "API.md")
        Copy-RequiredFile -Source (Join-Path $RepoRoot "docs\ENCYCLOPEDIA.md") -Destination (Join-Path $packageRoot "ENCYCLOPEDIA.md")
        Copy-RequiredFile -Source (Join-Path $RepoRoot "LICENSE") -Destination $packageRoot
        Copy-RequiredFile -Source (Join-Path $RepoRoot "third_party_versions.txt") -Destination $packageRoot

        if (Test-Path -LiteralPath $zipPath) {
            Remove-Item -LiteralPath $zipPath -Force
        }
        Compress-Archive -Path (Join-Path $packageRoot "*") -DestinationPath $zipPath -CompressionLevel Optimal
    }

    if (-not $SkipArchiveValidation) {
        Invoke-Step "Validate archive" {
            New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
            Expand-Archive -LiteralPath $zipPath -DestinationPath $extractRoot -Force

            $cliName = Get-ExecutableName -BaseName "mosaicraft"
            $webUiName = Get-ExecutableName -BaseName "MosaicraftWebUI"
            $cliPath = Join-Path $extractRoot $cliName
            $webUiPath = Join-Path $extractRoot $webUiName
            Assert-FileExists -Path $cliPath
            Assert-FileExists -Path $webUiPath

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
        $item = Get-Item -LiteralPath $zipPath
        $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath
        Write-Host "Package: $($item.FullName)"
        Write-Host "Size: $($item.Length)"
        Write-Host "SHA256: $($hash.Hash)"
    }
}
finally {
    if (-not $KeepWorkspace) {
        if (Test-Path -LiteralPath $packageRoot) {
            Remove-Item -LiteralPath $packageRoot -Recurse -Force
        }
        if (Test-Path -LiteralPath $extractRoot) {
            Remove-Item -LiteralPath $extractRoot -Recurse -Force
        }
    } else {
        Write-Host "Package workspace: $packageRoot"
        Write-Host "Extract workspace: $extractRoot"
    }
}
