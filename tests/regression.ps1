#!/usr/bin/env pwsh
# Mosaicraft Regression Test Suite
# Usage: pwsh -File tests/regression.ps1

$ErrorActionPreference = "Stop"
$Exe = ".\build\Release\mosaicraft.exe"
$Db  = ".\test.db"
$Out = ".\tests\output"
$Targets = @("target2.jpg","target3.jpg","target5.png","target8.jpg")

$Tests = @(
    @{Name="JPG-GPU";     Args="-i {0} -d $Db -o $Out/jpg_gpu.jpg --out-w 320 --out-h 480 --tile-w 45 --tile-h 80"},
    @{Name="JPG-CPU";     Args="-i {0} -d $Db -o $Out/jpg_cpu.jpg --out-w 320 --out-h 480 --cpu"},
    @{Name="TIFF-GPU";    Args="-i {0} -d $Db -o $Out/tiff_gpu.tiff --out-w 320 --out-h 480 --format tiff"},
    @{Name="WebP-GPU";    Args="-i {0} -d $Db -o $Out/webp_gpu.webp --out-w 320 --out-h 480 --format webp"},
    @{Name="PNG-GPU";     Args="-i {0} -d $Db -o $Out/png_gpu.png --out-w 320 --out-h 480 --format png"},
    @{Name="DeepZoom";    Args="-i {0} -d $Db -o $Out/dz --tiled --deepzoom --out-w 320 --out-h 480"},
    @{Name="Analyze";     Args="-i {0} -d $Db -o $Out/ana.jpg --analyze"},
    @{Name="Upscale";     Args="-i {0} -d $Db -o $Out/up.jpg --upscale 2 --out-w 320 --out-h 240"},
    @{Name="Benchmark";   Args="-i {0} -d $Db -o $Out/bench.jpg --benchmark"},
    @{Name="Inspect";     Args="-i $Out/inspect_test.jpg"},
    @{Name="db-stats";    Args="-d $Db"}
)

$Passed = 0; $Failed = 0
Write-Host "=== Mosaicraft Regression Suite ===" -ForegroundColor Cyan
Write-Host ""

foreach ($t in $Targets) {
    $src = ".\$t"
    foreach ($test in $Tests) {
        if ($test.Name -eq "db-stats") { $args = $test.Args }
        elseif ($test.Name -eq "Inspect") { $args = $test.Args -f "$Out/inspect.jpg" }
        else { $args = $test.Args -f $src }
        
        Write-Host "[$($test.Name):$t] " -NoNewline
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        $result = & $Exe @($args -split ' ') 2>&1
        $watch.Stop()
        if ($LASTEXITCODE -eq 0 -and $watch.Elapsed.TotalSeconds -lt 300) {
            Write-Host "OK (${0}s)" -f [math]::Round($watch.Elapsed.TotalSeconds,1) -ForegroundColor Green
            $Passed++
        } else {
            Write-Host "FAIL (${0}s)" -f [math]::Round($watch.Elapsed.TotalSeconds,1) -ForegroundColor Red
            $Failed++
        }
    }
}

Write-Host ""
Write-Host "=== Results: $Passed passed, $Failed failed ===" -ForegroundColor $(if ($Failed -eq 0) {"Green"} else {"Red"})
exit $Failed
