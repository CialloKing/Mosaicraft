param(
    [string]$Repository = "CialloKing/Mosaicraft",
    [string]$Tag = "",
    [string]$AssetName = "",
    [string]$ZipPath = "",
    [string]$ExpectedVersion = "",
    [string]$ExpectedSha256 = "",
    [string]$DownloadDir = "",
    [int]$TimeoutSeconds = 90,
    [switch]$SkipSmoke,
    [switch]$KeepWorkspace
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$downloadRoot = ""
$extractRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("mosaicraft_release_verify_extract_" + [System.Guid]::NewGuid().ToString("N"))
$createdDownloadRoot = $false

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

function Invoke-Step
{
    param(
        [string]$Label,
        [scriptblock]$Action
    )

    Write-Host ""
    Write-Host "==> $Label"
    & $Action
}

function Test-CommandExists
{
    param([string]$Name)

    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Assert-CommandExists
{
    param([string]$Name)

    if (-not (Test-CommandExists -Name $Name))
    {
        throw "Missing command: $Name"
    }
}

function Resolve-InputPath
{
    param([string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path))
    {
        return (Resolve-Path -LiteralPath $Path).Path
    }
    return (Resolve-Path -LiteralPath (Join-Path (Get-Location) $Path)).Path
}

function Resolve-OutputPath
{
    param([string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path))
    {
        return $Path
    }
    return (Join-Path (Get-Location) $Path)
}

function Get-ProjectVersion
{
    $versionHeader = Join-Path $RepoRoot "core\Version.h"
    $content = Get-Content -LiteralPath $versionHeader -Raw
    $match = [regex]::Match($content, 'kVersion\s*=\s*"([^"]+)"')
    if (-not $match.Success)
    {
        throw "Unable to read version from $versionHeader"
    }
    return $match.Groups[1].Value
}

function Get-VersionFromTag
{
    param([string]$Value)

    if (-not $Value)
    {
        return ""
    }
    return $Value.TrimStart([char]"v")
}

function Normalize-Sha256
{
    param([string]$Value)

    $text = $Value.Trim()
    if ($text.StartsWith("sha256:", [System.StringComparison]::OrdinalIgnoreCase))
    {
        $text = $text.Substring(7)
    }
    $text = $text.Trim().ToUpperInvariant()
    if ($text -and $text -notmatch '^[0-9A-F]{64}$')
    {
        throw "Invalid SHA256 value: $Value"
    }
    return $text
}

function Get-ObjectPropertyText
{
    param(
        $Object,
        [string]$Name
    )

    if ($null -eq $Object)
    {
        return ""
    }

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value)
    {
        return ""
    }
    return [string]$property.Value
}

function Read-GitHubRelease
{
    param(
        [string]$RepositoryName,
        [string]$ReleaseTag
    )

    Assert-CommandExists -Name "gh"
    $json = Invoke-NativeOutput -FilePath "gh" -Arguments @(
        "release",
        "view",
        $ReleaseTag,
        "--repo",
        $RepositoryName,
        "--json",
        "tagName,name,assets,body,url"
    )
    return (($json | Out-String) | ConvertFrom-Json)
}

function Select-ReleaseAsset
{
    param(
        $Release,
        [string]$RequestedName
    )

    $assets = @($Release.assets)
    if ($RequestedName)
    {
        foreach ($asset in $assets)
        {
            if ((Get-ObjectPropertyText -Object $asset -Name "name") -eq $RequestedName)
            {
                return $asset
            }
        }
        throw "Release asset not found: $RequestedName"
    }

    $zipAssets = @()
    foreach ($asset in $assets)
    {
        $name = Get-ObjectPropertyText -Object $asset -Name "name"
        if ($name -like "*.zip")
        {
            $zipAssets += $asset
        }
    }

    if ($zipAssets.Count -eq 1)
    {
        return $zipAssets[0]
    }
    if ($zipAssets.Count -eq 0)
    {
        throw "Release has no zip asset"
    }
    throw "Release has multiple zip assets; pass -AssetName"
}

function Get-ReleaseBodySha256
{
    param([string]$Body)

    if (-not $Body)
    {
        return ""
    }

    $matches = [regex]::Matches($Body, '(?i)SHA256:\s*`?([0-9a-f]{64})`?')
    if ($matches.Count -eq 0)
    {
        return ""
    }
    return Normalize-Sha256 -Value $matches[$matches.Count - 1].Groups[1].Value
}

function Download-ReleaseAsset
{
    param(
        [string]$RepositoryName,
        [string]$ReleaseTag,
        [string]$Name,
        [string]$DownloadUrl,
        [string]$DestinationDir
    )

    New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null
    $destination = Join-Path $DestinationDir $Name
    if (Test-Path -LiteralPath $destination)
    {
        Remove-Item -LiteralPath $destination -Force
    }

    if (-not $DownloadUrl)
    {
        throw "Release asset download URL is missing for $RepositoryName $ReleaseTag $Name"
    }

    # Use the asset URL directly so the verifier does not depend on gh release download behavior.
    if ($PSVersionTable.PSVersion.Major -lt 6)
    {
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $destination -TimeoutSec 120 -UseBasicParsing | Out-Null
    }
    else
    {
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $destination -TimeoutSec 120 | Out-Null
    }

    if (-not (Test-Path -LiteralPath $destination))
    {
        throw "Downloaded asset was not found: $destination"
    }
    return (Resolve-Path -LiteralPath $destination).Path
}

function Assert-FileExists
{
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path))
    {
        throw "Missing file: $Path"
    }
}

function Assert-ArchiveDocumentationPolicy
{
    param([string]$Root)

    foreach ($required in @(
        "README.md",
        "API.md",
        "ENCYCLOPEDIA.md",
        "LICENSE",
        "third_party_versions.txt"
    ))
    {
        Assert-FileExists -Path (Join-Path $Root $required)
    }

    $blockedNames = @()
    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File)
    {
        if ($file.Name -eq "CHANGELOG.md" -or $file.Name -like "RELEASE_NOTES_*.md")
        {
            $blockedNames += $file.Name
        }
    }

    if ($blockedNames.Count -gt 0)
    {
        $names = ($blockedNames | Sort-Object -Unique) -join ", "
        throw "Unexpected standalone release documentation in archive: $names"
    }
}

function Get-PowerShellCommand
{
    if (Test-CommandExists -Name "pwsh")
    {
        return "pwsh"
    }
    return "powershell"
}

function Invoke-ExecutableText
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
    return (($output | Out-String).Trim())
}

function Assert-PackageCommands
{
    param(
        [string]$CliPath,
        [string]$WebUiPath,
        [string]$VersionText
    )

    $versionOutput = Invoke-ExecutableText -FilePath $CliPath -Arguments @("--version")
    if ($versionOutput -ne "Mosaicraft $VersionText")
    {
        throw "Unexpected CLI version output: $versionOutput"
    }

    $cliHelp = Invoke-ExecutableText -FilePath $CliPath -Arguments @("--help")
    if ($cliHelp -notmatch "mosaic" -or $cliHelp -notmatch "build")
    {
        throw "CLI help output does not contain expected commands"
    }

    $webUiHelp = Invoke-ExecutableText -FilePath $WebUiPath -Arguments @("--help")
    if ($webUiHelp -notmatch "Usage: MosaicraftWebUI")
    {
        throw "Web UI help output is unexpected"
    }
}

function Invoke-PackageSmoke
{
    param(
        [string]$WebUiPath,
        [int]$Timeout
    )

    $powershellCommand = Get-PowerShellCommand
    $arguments = @("-NoProfile")
    if ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT)
    {
        $arguments += @("-ExecutionPolicy", "Bypass")
    }
    $arguments += @(
        "-File",
        (Join-Path $RepoRoot "tests\webui_smoke.ps1"),
        "-WebUiExe",
        $WebUiPath,
        "-TimeoutSeconds",
        [string]$Timeout
    )

    # The smoke test must use the extracted Web UI executable so it proves the downloaded package works.
    Invoke-NativeOutput -FilePath $powershellCommand -Arguments $arguments | Out-Host
}

try
{
    if (-not $ExpectedVersion)
    {
        if ($Tag)
        {
            $ExpectedVersion = Get-VersionFromTag -Value $Tag
        }
        else
        {
            $ExpectedVersion = Get-ProjectVersion
        }
    }

    if (-not $Tag -and -not $ZipPath)
    {
        $Tag = "v$(Get-ProjectVersion)"
    }

    $release = $null
    $releaseUrl = ""
    $asset = $null
    $releaseBodyHash = ""
    $assetDigestHash = ""

    if ($Tag)
    {
        Invoke-Step "Read GitHub release metadata" {
            $script:release = Read-GitHubRelease -RepositoryName $Repository -ReleaseTag $Tag
            $script:releaseUrl = Get-ObjectPropertyText -Object $script:release -Name "url"
            $script:asset = Select-ReleaseAsset -Release $script:release -RequestedName $AssetName
            $script:releaseBodyHash = Get-ReleaseBodySha256 -Body (Get-ObjectPropertyText -Object $script:release -Name "body")
            $script:assetDigestHash = Normalize-Sha256 -Value (Get-ObjectPropertyText -Object $script:asset -Name "digest")
            Write-Host "Release: $script:releaseUrl"
            Write-Host "Asset: $(Get-ObjectPropertyText -Object $script:asset -Name "name")"
        }
    }

    $zip = ""
    if ($ZipPath)
    {
        $zip = Resolve-InputPath -Path $ZipPath
    }
    else
    {
        $assetNameToDownload = Get-ObjectPropertyText -Object $asset -Name "name"
        if ($DownloadDir)
        {
            $downloadRoot = Resolve-OutputPath -Path $DownloadDir
        }
        else
        {
            $downloadRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("mosaicraft_release_verify_download_" + [System.Guid]::NewGuid().ToString("N"))
            $createdDownloadRoot = $true
        }

        Invoke-Step "Download release asset" {
            $assetDownloadUrl = Get-ObjectPropertyText -Object $asset -Name "url"
            $script:zip = Download-ReleaseAsset -RepositoryName $Repository -ReleaseTag $Tag -Name $assetNameToDownload -DownloadUrl $assetDownloadUrl -DestinationDir $downloadRoot
            Write-Host "Downloaded: $script:zip"
        }
        $zip = $script:zip
    }

    Invoke-Step "Verify SHA256" {
        $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToUpperInvariant()
        $expectedHash = Normalize-Sha256 -Value $ExpectedSha256
        if (-not $expectedHash)
        {
            $expectedHash = $assetDigestHash
        }

        if ($expectedHash -and $actualHash -ne $expectedHash)
        {
            throw "SHA256 mismatch. expected=$expectedHash actual=$actualHash"
        }
        if ($releaseBodyHash -and $actualHash -ne $releaseBodyHash)
        {
            throw "Release page SHA256 does not match asset. page=$releaseBodyHash actual=$actualHash"
        }

        Write-Host "SHA256: $actualHash"
    }

    Invoke-Step "Extract and inspect package" {
        New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
        Expand-Archive -LiteralPath $zip -DestinationPath $extractRoot -Force

        $cliPath = Join-Path $extractRoot "mosaicraft.exe"
        $webUiPath = Join-Path $extractRoot "MosaicraftWebUI.exe"
        Assert-FileExists -Path $cliPath
        Assert-FileExists -Path $webUiPath
        Assert-FileExists -Path (Join-Path $extractRoot "index.html")

        # Single-version release notes should live in ENCYCLOPEDIA.md to keep every zip compact and predictable.
        Assert-ArchiveDocumentationPolicy -Root $extractRoot
        Assert-PackageCommands -CliPath $cliPath -WebUiPath $webUiPath -VersionText $ExpectedVersion

        if (-not $SkipSmoke)
        {
            Invoke-PackageSmoke -WebUiPath $webUiPath -Timeout $TimeoutSeconds
        }
    }

    Invoke-Step "Release asset verification summary" {
        Write-Host "Verified zip: $zip"
        Write-Host "Version: $ExpectedVersion"
        if ($releaseUrl)
        {
            Write-Host "Release: $releaseUrl"
        }
        if ($KeepWorkspace)
        {
            Write-Host "Extract workspace: $extractRoot"
            if ($downloadRoot)
            {
                Write-Host "Download workspace: $downloadRoot"
            }
        }
    }
}
finally
{
    if (-not $KeepWorkspace)
    {
        if (Test-Path -LiteralPath $extractRoot)
        {
            Remove-Item -LiteralPath $extractRoot -Recurse -Force
        }
        if ($createdDownloadRoot -and (Test-Path -LiteralPath $downloadRoot))
        {
            Remove-Item -LiteralPath $downloadRoot -Recurse -Force
        }
    }
}
