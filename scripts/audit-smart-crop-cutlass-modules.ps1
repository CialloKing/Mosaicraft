param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$InputDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][ValidateSet('smart-real', 'smart-anime')][string]$Crop
)

$ErrorActionPreference = 'Stop'
$cli = (Resolve-Path -LiteralPath $Exe).Path
$source = (Resolve-Path -LiteralPath $InputDirectory).Path
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "Output must be a new directory: $output" }
if ($output.StartsWith($source.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Output must not be under the input directory'
}
[void][IO.Directory]::CreateDirectory($output)

$start = [Diagnostics.ProcessStartInfo]::new($cli)
$start.UseShellExecute = $false
$start.WorkingDirectory = $output
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
# Do not find third-party DLLs on the developer's PATH or in the starting directory.
$start.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
foreach ($key in @($start.Environment.Keys)) {
    if ($key -like 'CUDA_PATH*' -or $key -eq 'MOSAICRAFT_CUDA_SDK' -or $key -eq 'MOSAICRAFT_CUDNN_SDK') {
        [void]$start.Environment.Remove($key)
    }
}
foreach ($arg in @('library', 'normalize', $source, '--recursive', '-o', (Join-Path $output 'images'),
                   '--size', '180x320', '--threads', '3', '--crop', $Crop, '--crop-device', 'cuda', '--json')) {
    $start.ArgumentList.Add($arg)
}
$process = [Diagnostics.Process]::Start($start)
$stdout = $process.StandardOutput.ReadToEndAsync()
$stderr = $process.StandardError.ReadToEndAsync()
$modules = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
try {
    while (-not $process.HasExited) {
        $process.Refresh()
        try {
            foreach ($module in $process.Modules) { [void]$modules.Add($module.FileName) }
        }
        catch [ComponentModel.Win32Exception] {
            if (-not $process.HasExited) { throw }
        }
        Start-Sleep -Milliseconds 40
    }
    $exitCode = $process.ExitCode
}
finally {
    if (-not $process.HasExited) { $process.Kill($true) }
    $process.Dispose()
}
$stdout.Result | Set-Content (Join-Path $output 'runtime.json') -Encoding utf8
$stderr.Result | Set-Content (Join-Path $output 'runtime.log') -Encoding utf8
$modules | Sort-Object | Set-Content (Join-Path $output 'modules.txt') -Encoding utf8
if ($exitCode -ne 0) { throw "Strict CUDA crop failed (exit $exitCode)" }
$runtime = ($stdout.Result | ConvertFrom-Json).runtime
if ($runtime.device -ne 'cuda' -or $runtime.metrics.smartCropCudaImages -le 0 -or
    $runtime.metrics.smartCropCpuImages -ne 0 -or $runtime.metrics.smartCropCudaFallbacks -ne 0 -or
    $runtime.metrics.smartCropCudaRecoveries -ne 0 -or
    @($modules | Where-Object { (Split-Path $_ -Leaf) -ieq 'nvcuda.dll' }).Count -eq 0) {
    throw 'Missing evidence of real CUDA inference or driver module load'
}
if ($modules | Where-Object { (Split-Path $_ -Leaf) -match '^(cublas|cudnn).*\.dll$' }) {
    throw 'cuBLAS or cuDNN was loaded'
}
if ($modules | Where-Object { $_ -match '\\NVIDIA GPU Computing Toolkit\\' }) {
    throw 'A CUDA Toolkit module was loaded'
}
