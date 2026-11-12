function Assert-ReleaseVersionSummary
{
    param([string]$RepoRoot, [string]$VersionText)

    $summary = Join-Path $RepoRoot 'docs/releases.md'
    $content = Get-Content -LiteralPath $summary -Raw -Encoding utf8
    $escaped = [regex]::Escape($VersionText)
    if ($content -notmatch "(?m)^## v$escaped\r?$")
    {
        throw "Missing version heading in docs/releases.md: v$VersionText"
    }
}

function Copy-ReleaseDocumentation
{
    param([string]$RepoRoot, [string]$Destination)

    # Preserve relative links without accidentally packaging private, untracked reports.
    $documents = @(& git -C $RepoRoot ls-files -- docs)
    if ($LASTEXITCODE -ne 0 -or $documents.Count -eq 0)
    {
        throw 'Cannot enumerate tracked release documentation'
    }
    foreach ($document in $documents)
    {
        $target = Join-Path $Destination $document
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath (Join-Path $RepoRoot $document) -Destination $target
    }
}

function Assert-ReleaseDocumentation
{
    param([string]$Root, [switch]$AllowLegacy, [string]$SourceRoot = '')

    $required = @('README.md', 'BUILD_INFO.txt', 'contracts.json', 'LICENSE', 'third_party_versions.txt')
    if (Test-Path -LiteralPath (Join-Path $Root 'docs/README.md'))
    {
        $required += @(
            'docs/releases.md',
            'docs/guides/quick-start.md',
            'docs/reference/api.md',
            'docs/development/portable-gpu.md',
            'docs/archive/README.md',
            'docs/performance/portable-gpu-20261113.json'
        )
    }
    elseif ($AllowLegacy)
    {
        # Historical release assets retain their original root-level document layout.
        $required += @('API.md', 'ENCYCLOPEDIA.md')
    }
    else
    {
        throw 'Missing release documentation index: docs/README.md'
    }
    foreach ($document in $required)
    {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $document) -PathType Leaf))
        {
            throw "Missing release document: $document"
        }
    }

    $blocked = Get-ChildItem -LiteralPath $Root -Recurse -File |
        Where-Object { $_.Name -eq 'CHANGELOG.md' -or $_.Name -like 'RELEASE_NOTES_*.md' }
    if ($blocked)
    {
        throw "Unexpected standalone release documentation: $(($blocked.Name | Sort-Object -Unique) -join ', ')"
    }

    if ($SourceRoot)
    {
        $documents = @(& git -C $SourceRoot ls-files -- docs)
        if ($LASTEXITCODE -ne 0 -or $documents.Count -eq 0)
        {
            throw 'Cannot enumerate source documentation for archive validation'
        }
        foreach ($document in $documents)
        {
            $source = Join-Path $SourceRoot $document
            $target = Join-Path $Root $document
            if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or
                (Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $target).Hash)
            {
                throw "Archived documentation differs from source: $document"
            }
        }
    }
}
