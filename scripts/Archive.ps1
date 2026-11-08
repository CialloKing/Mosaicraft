# Use native tar on Unix so archives retain executable bits and symlink chains.
function Compress-ReleaseArchive
{
    param([string]$Root, [string]$Destination)
    if ($Destination.EndsWith('.tar.gz'))
    {
        & tar -czf $Destination -C $Root .
        if ($LASTEXITCODE -ne 0) { throw 'tar compression failed' }
    }
    else
    {
        Compress-Archive -Path (Join-Path $Root '*') -DestinationPath $Destination -CompressionLevel Optimal
    }
}

function Expand-ReleaseArchive
{
    param([string]$Path, [string]$Destination)
    if ($Path.EndsWith('.tar.gz'))
    {
        & tar -xzf $Path -C $Destination
        if ($LASTEXITCODE -ne 0) { throw 'tar extraction failed' }
    }
    elseif ($Path.EndsWith('.zip'))
    {
        Expand-Archive -LiteralPath $Path -DestinationPath $Destination -Force
    }
    else
    {
        throw "Unsupported archive format: $Path"
    }
}
