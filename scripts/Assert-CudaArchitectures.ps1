param(
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][string]$Executable,
    [string]$Cuobjdump = 'cuobjdump',
    [switch]$RequireUniversal
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$info = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $BuildDir 'cuda-architectures.txt'))
{
    if ($line -match '^([^:]+):\s*(.*)$') { $info[$Matches[1]] = $Matches[2] }
}
$targets = @($info.CudaArchitectures -split ';')
$ptx = @($info.CudaPtx -split ';' | ForEach-Object { $_ -replace '^compute_', 'sm_' })
$universal = 'sm_75;sm_80;sm_86;sm_89;sm_90;sm_100;sm_120'
if ($RequireUniversal -and (($targets -join ';') -ne $universal -or ($ptx -join ';') -ne $universal))
{
    throw 'Universal CUDA package requires all seven native targets and matching PTX; reconfigure CUDA_ARCH or use a fresh build directory.'
}
function Assert-Images([string]$Path, [int]$Modules)
{
    foreach ($kind in @('elf', 'ptx'))
    {
        $listing = & $Cuobjdump "--list-$kind" $Path 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "cuobjdump failed for $Path : $listing" }
        $images = @([regex]::Matches($listing, '\.sm_(\d+)\.(?:cubin|ptx)') | ForEach-Object { "sm_$($_.Groups[1].Value)" })
        $expected = @(if ($kind -eq 'elf') { $targets } else { $ptx })
        if ($images.Count -ne $expected.Count * $Modules) { throw "Unexpected $kind images in $Path : $listing" }
        foreach ($target in $expected)
        {
            if (@($images | Where-Object { $_ -eq $target }).Count -ne $Modules)
            {
                throw "Missing $target $kind image in $Path"
            }
        }
    }
}
foreach ($module in @('CudaBackend', 'FeatureExtractorCuda', 'CompatibleFeatures', 'CompatibleScoring'))
{
    $object = Join-Path $BuildDir "$module.obj"
    if (-not (Test-Path -LiteralPath $object)) { $object = Join-Path $BuildDir "$module.o" }
    Assert-Images $object 1
}
Assert-Images $Executable 4
Write-Host "CUDA images verified: $($targets -join ', '); PTX $($info.CudaPtx)"
