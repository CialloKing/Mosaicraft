function Assert-PackageRuntime
{
    param([string]$Root)
    $windows = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
    $mac = [Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([Runtime.InteropServices.OSPlatform]::OSX)
    if ($windows) { return } # Windows verification also starts both programs with a restricted PATH.
    $binaries = @((Join-Path $Root 'mosaicraft'), (Join-Path $Root 'MosaicraftWebUI'))
    $binaries += @(Get-ChildItem (Join-Path $Root 'lib') -File | ForEach-Object FullName)
    foreach ($binary in $binaries)
    {
        if ($binary -in @((Join-Path $Root 'mosaicraft'), (Join-Path $Root 'MosaicraftWebUI')))
        {
            & test -x $binary
            if ($LASTEXITCODE -ne 0) { throw "Archive lost executable permission: $binary" }
        }
        if ($mac)
        {
            & codesign --verify $binary
            if ($LASTEXITCODE -ne 0) { throw "Invalid ad-hoc signature: $binary" }
            $loads = & otool -L $binary
            if ($LASTEXITCODE -ne 0) { throw "Cannot inspect $binary" }
            foreach ($line in $loads | Select-Object -Skip 1)
            {
                $load = ($line.Trim() -split ' ')[0]
                if ($load.StartsWith('/usr/lib/') -or $load.StartsWith('/System/Library/')) { continue }
                # IDs are @rpath; all dependency references must remain inside this archive.
                if ($load -notmatch '^@(loader_path|rpath)/') { throw "Non-relative dependency: $load" }
                $path = $load.Replace('@loader_path', (Split-Path $binary)).Replace('@rpath', (Join-Path $Root 'lib'))
                if (-not (Test-Path -LiteralPath $path)) { throw "Missing packaged dependency: $load" }
            }
        }
        else
        {
            $loads = & ldd $binary
            if ($LASTEXITCODE -ne 0 -or $loads -match 'not found') { throw "Unresolved dependencies: $binary`n$loads" }
            foreach ($line in $loads)
            {
                if ($line -match '=>\s+(/.+)\s+\(0x')
                {
                    $path = $Matches[1].Trim()
                    if ($path.StartsWith("$Root/")) { continue }
                    $name = Split-Path $path -Leaf
                    if ($name -notmatch '^lib(c|m|dl|pthread|rt|gcc_s|stdc\+\+)\.so')
                    {
                        throw "Dependency escaped the archive: $path"
                    }
                }
            }
        }
    }
}
