param(
    [string]$Repository = "CialloKing/Mosaicraft",
    [string]$Tag = "",
    [string]$AssetName = "",
    [Alias('ZipPath')][string]$ArchivePath = "",
    [string]$ExpectedVersion = "",
    [string]$ExpectedSha256 = "",
    [string]$ExpectedCommit = "",
    [ValidateSet('', 'cpu-only', 'cuda')][string]$ExpectedRuntime = '',
    [ValidateSet('', 'passed', 'not-run', 'not-applicable')][string]$ExpectedGpuValidation = '',
    [string]$DownloadDir = "",
    [int]$TimeoutSeconds = 90,
    [switch]$SkipSmoke,
    [switch]$RequireStandalone,
    [switch]$KeepWorkspace
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot 'Archive.ps1')
. (Join-Path $PSScriptRoot 'Assert-CudaBuildInfo.ps1')
. (Join-Path $PSScriptRoot 'Assert-PackageRuntime.ps1')
$originalPath = $env:PATH
$originalLdPath = $env:LD_LIBRARY_PATH
$originalDyldPath = $env:DYLD_LIBRARY_PATH
$downloadRoot = ""
$extractPrefix = if ($RequireStandalone) { 'mosaicraft 验收_' } else { 'mosaicraft_release_verify_extract_' }
$extractRoot = Join-Path ([System.IO.Path]::GetTempPath()) ($extractPrefix + [System.Guid]::NewGuid().ToString("N"))
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
    return (Get-Content -LiteralPath (Join-Path $RepoRoot "contracts.json") -Raw | ConvertFrom-Json).softwareVersion
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

function Read-GitHubTagCommitSha
{
    param(
        [string]$RepositoryName,
        [string]$ReleaseTag
    )

    Assert-CommandExists -Name "gh"
    $refJson = Invoke-NativeOutput -FilePath "gh" -Arguments @(
        "api",
        "repos/$RepositoryName/git/ref/tags/$ReleaseTag"
    )
    $ref = (($refJson | Out-String) | ConvertFrom-Json)
    $objectType = Get-ObjectPropertyText -Object $ref.object -Name "type"
    $objectSha = Get-ObjectPropertyText -Object $ref.object -Name "sha"

    if ($objectType -eq "commit")
    {
        return $objectSha
    }

    if ($objectType -ne "tag")
    {
        throw "Unsupported tag object type: $objectType"
    }

    $tagJson = Invoke-NativeOutput -FilePath "gh" -Arguments @(
        "api",
        "repos/$RepositoryName/git/tags/$objectSha"
    )
    $tag = (($tagJson | Out-String) | ConvertFrom-Json)
    $commitType = Get-ObjectPropertyText -Object $tag.object -Name "type"
    if ($commitType -ne "commit")
    {
        throw "Annotated tag does not point to a commit: $ReleaseTag"
    }
    return (Get-ObjectPropertyText -Object $tag.object -Name "sha")
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
        if ($name -like "*.zip" -or $name -like "*.tar.gz")
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
        throw "Release has no ZIP or tar.gz asset"
    }
    throw "Release has multiple archives; pass -AssetName"
}

function Get-ReleaseBodySha256
{
    param([string]$Body, [string]$Name, [bool]$SingleArchive)

    if (-not $Body)
    {
        return ""
    }

    foreach ($line in $Body -split "`n")
    {
        if ($line.Contains($Name) -and $line -match '(?i)\b([0-9a-f]{64})\b')
        {
            return Normalize-Sha256 -Value $Matches[1]
        }
    }
    if (-not $SingleArchive) { return '' }
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
        "BUILD_INFO.txt",
        "contracts.json",
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
        [string]$Root,
        [string]$VersionText,
        [string]$PackageName,
        [string]$TagCommitHash
    )

    $versionsPath = Join-Path $Root "contracts.json"
    Assert-FileExists -Path $versionsPath
    $versions = Get-Content -LiteralPath $versionsPath -Raw | ConvertFrom-Json
    $expected = Get-Content -LiteralPath (Join-Path $RepoRoot "contracts.json") -Raw | ConvertFrom-Json
    foreach ($property in $expected.PSObject.Properties)
    {
        if ($versions.($property.Name) -ne $property.Value)
        {
            throw "Package internal version mismatch: $($property.Name)"
        }
    }
    $buildInfoPath = Join-Path $Root "BUILD_INFO.txt"
    Assert-FileExists -Path $buildInfoPath
    $info = Read-BuildInfo -Path $buildInfoPath
    Assert-CudaBuildInfo $info
    $commit = [string]$info["Commit"]

    if ($info["Version"] -ne $VersionText)
    {
        throw "BUILD_INFO version mismatch: $($info["Version"])"
    }
    if ($PackageName -and $info["Package"] -ne $PackageName)
    {
        throw "BUILD_INFO package mismatch: $($info["Package"])"
    }
    if ($commit -notmatch '^[0-9a-fA-F]{40}$')
    {
        throw "BUILD_INFO commit is missing or invalid: $commit"
    }
    if ($TagCommitHash -and $commit.ToLowerInvariant() -ne $TagCommitHash.ToLowerInvariant())
    {
        throw "BUILD_INFO commit does not match release tag. package=$commit tag=$TagCommitHash"
    }
    if ($ExpectedCommit -and $commit -ne $ExpectedCommit)
    {
        throw "BUILD_INFO commit does not match expected commit: $commit"
    }
    if ($ExpectedRuntime -and $info['Runtime'] -ne $ExpectedRuntime)
    {
        throw "BUILD_INFO runtime does not match expected runtime"
    }
    # Missing GPU metadata is accepted for ordinary verification of historical packages.
    if ($ExpectedGpuValidation -and $info['GpuValidation'] -ne $ExpectedGpuValidation)
    {
        throw 'BUILD_INFO GPU validation does not match the required state'
    }
    if ($info.ContainsKey('GpuValidation'))
    {
        if ($info['GpuValidation'] -notin @('passed', 'not-run', 'not-applicable')) { throw 'Invalid GPU validation state' }
        if ($info['Runtime'] -eq 'cpu-only' -and $info['GpuValidation'] -ne 'not-applicable') { throw 'CPU package has CUDA validation metadata' }
        Assert-CudaPackageName $info $PackageName
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

    if (-not $Tag -and -not $ArchivePath)
    {
        $Tag = "v$(Get-ProjectVersion)"
    }

    $release = $null
    $releaseUrl = ""
    $asset = $null
    $releaseBodyHash = ""
    $assetDigestHash = ""
    $tagCommitHash = ""

    if ($Tag)
    {
        Invoke-Step "Read GitHub release metadata" {
            $script:release = Read-GitHubRelease -RepositoryName $Repository -ReleaseTag $Tag
            $script:releaseUrl = Get-ObjectPropertyText -Object $script:release -Name "url"
            $script:asset = Select-ReleaseAsset -Release $script:release -RequestedName $AssetName
            $archiveCount = @($script:release.assets | Where-Object { $_.name -match '\.(zip|tar\.gz)$' }).Count
            $script:releaseBodyHash = Get-ReleaseBodySha256 -Body (Get-ObjectPropertyText -Object $script:release -Name "body") -Name $script:asset.name -SingleArchive ($archiveCount -eq 1)
            $script:assetDigestHash = Normalize-Sha256 -Value (Get-ObjectPropertyText -Object $script:asset -Name "digest")
            $script:tagCommitHash = Read-GitHubTagCommitSha -RepositoryName $Repository -ReleaseTag $Tag
            Write-Host "Release: $script:releaseUrl"
            Write-Host "Asset: $(Get-ObjectPropertyText -Object $script:asset -Name "name")"
            Write-Host "Tag commit: $script:tagCommitHash"
        }
    }

    $zip = ""
    if ($ArchivePath)
    {
        $zip = Resolve-InputPath -Path $ArchivePath
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
        Expand-ReleaseArchive -Path $zip -Destination $extractRoot

        $extension = if ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT) { '.exe' } else { '' }
        $cliPath = Join-Path $extractRoot "mosaicraft$extension"
        $webUiPath = Join-Path $extractRoot "MosaicraftWebUI$extension"
        Assert-FileExists -Path $cliPath
        Assert-FileExists -Path $webUiPath
        Assert-FileExists -Path (Join-Path $extractRoot "index.html")

        # Single-version release notes should live in ENCYCLOPEDIA.md to keep every zip compact and predictable.
        Assert-ArchiveDocumentationPolicy -Root $extractRoot
        Assert-BuildInfo -Root $extractRoot -VersionText $ExpectedVersion -PackageName (Split-Path -Leaf $zip) -TagCommitHash $tagCommitHash
        if ($RequireStandalone)
        {
            $env:LD_LIBRARY_PATH = ''
            $env:DYLD_LIBRARY_PATH = ''
            if ($extension)
            {
                $env:PATH = "$PSHOME;$env:WINDIR/System32;$env:WINDIR"
            }
            Assert-PackageRuntime -Root $extractRoot
        }
        # A package must find its own HTML, without a source-checkout fallback.
        Push-Location $extractRoot
        try
        {
            Assert-PackageCommands -CliPath $cliPath -WebUiPath $webUiPath -VersionText $ExpectedVersion
            if (-not $SkipSmoke)
            {
                Invoke-PackageSmoke -WebUiPath $webUiPath -Timeout $TimeoutSeconds
            }
        }
        finally { Pop-Location }
    }

    Invoke-Step "Release asset verification summary" {
        Write-Host "Verified archive: $zip"
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
    $env:PATH = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdPath
    $env:DYLD_LIBRARY_PATH = $originalDyldPath
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
