# Use native tar on Unix so archives retain executable bits and symlink chains.
function Compress-ReleaseArchive
{
    param([string]$Root, [string]$Destination)
    if ($Destination.EndsWith('.tar.gz'))
    {
        & tar -czf $Destination -C $Root .
        if ($LASTEXITCODE -ne 0) { throw 'tar compression failed' }
    }
    elseif ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT)
    {
        Push-Location $Root
        try
        {
            & zip -q -y -r $Destination .
            if ($LASTEXITCODE -ne 0) { throw 'zip compression failed' }
        }
        finally { Pop-Location }
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
        if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT)
        {
            Expand-Archive -LiteralPath $Path -DestinationPath $Destination -Force
        }
        else
        {
            & unzip -q $Path -d $Destination
            if ($LASTEXITCODE -ne 0) { throw 'zip extraction failed' }
        }
    }
    else
    {
        throw "Unsupported archive format: $Path"
    }
}
