# Verify the installed worker without Qt or GPU SDK paths on the runtime search path.
param([Parameter(Mandatory)][string]$Bin, [switch]$RequireGpu)

$ErrorActionPreference = 'Stop'
$exe = if ($IsWindows) { '.exe' } else { '' }
$cli = Join-Path $Bin "mosaicraft$exe"
$worker = Join-Path $Bin "mosaicraft_desktop_worker$exe"
function Get-Devices {
    $reply = & $cli devices --json | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or !$reply.ok) { throw 'Packaged CLI failed to list devices' }
    $reply.data.devices
}
function Assert-Preview([string]$Backend) {
    $request.backend = $Backend
    $reply = @{op='preview'; request=$request} | ConvertTo-Json -Compress | & $worker 2>$stderr | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or !$reply.ok -or !$reply.image) {
        Get-Content -LiteralPath $stderr | Out-Host
        throw "Packaged worker preview failed for ${Backend}: $($reply.message)"
    }
    Write-Host "Packaged worker preview passed: $Backend"
}
$envBackup = @{}
foreach ($key in @('CUDA_VISIBLE_DEVICES','VK_DRIVER_FILES','VK_ICD_FILENAMES')) {
    $envBackup[$key] = [Environment]::GetEnvironmentVariable($key)
}
$work = Join-Path (Get-Location) 'GPU smoke'
$inputDir = Join-Path $work 'input'
New-Item -ItemType Directory -Force -Path $inputDir | Out-Null
$target = Join-Path $inputDir 'target.png'
$library = Join-Path $work 'library'
$stderr = Join-Path $work 'worker.log'
# A 32 x 32 RGB fixture keeps startup and inference costs small on either backend.
[IO.File]::WriteAllBytes($target, [Convert]::FromBase64String('iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAIAAAD8GO2jAAAAkUlEQVR4nLXCkRqDAABG0QvBYBAEQRAEgyAIBsFgMBgMgkEQDAZBEPywh99T3PMdIO6iiJrTKWrO56gpy6ipqqip66hpmqhp26jpuqi5XKKm76NmGKJmHKPmeo2aaYqa2y1q7veoeTyi5vmMmtcrauY5at7vqFmWqFnXqPl8oub7jZpti5p9j5rjiJrI+P2i/gOaPdAu1J1S5wAAAABJRU5ErkJggg=='))
& $cli library build $inputDir -o $library --size 32x32 --backend cpu | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Packaged CLI failed to build a fixture library' }
$request = @{inputPath=$target; dbPath=$library; outputPath=(Join-Path $work 'output.png');
             outputMode='image'; targetSize='32x32'; tileW=16; tileH=16; seed=9; backend='auto'}
try {
    $devices = Get-Devices
    foreach ($backend in @('cuda','vulkan')) {
        $matches = @($devices | Where-Object backend -eq $backend)
        if (!$matches.Count -or @($matches | Where-Object reason -match 'not compiled').Count) {
            throw "$backend is not present in the packaged CLI"
        }
        if ($RequireGpu) {
            if (!($matches | Where-Object available)) { throw "A real $backend device is required" }
            Assert-Preview $backend
        }
    }
    # Verify auto works even when the NVIDIA driver or Vulkan ICD cannot be used.
    $env:CUDA_VISIBLE_DEVICES = '-1'
    $env:VK_ICD_FILENAMES = $env:VK_DRIVER_FILES = Join-Path $work 'no-driver.json'
    if (Get-Devices | Where-Object { $_.available -and $_.backend -in @('cuda','vulkan') }) {
        throw 'Expected both GPU backends to be unavailable for the fallback smoke test'
    }
    Assert-Preview auto
} finally {
    foreach ($key in $envBackup.Keys) { [Environment]::SetEnvironmentVariable($key, $envBackup[$key], 'Process') }
}
