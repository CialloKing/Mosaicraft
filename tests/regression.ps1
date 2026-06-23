# Mosaicraft Smoke Test (needs existing test.db)
param([string]$Exe = "build/Release/mosaicraft.exe", [string]$Db = "test.db")

$failed = 0; $passed = 0
$target = "target10.png"
if (-not (Test-Path $target)) { $target = "target8.jpg" }

function test($name, [ScriptBlock]$body) {
    Write-Host -NoNewline "  $name ... "
    try { & $body; Write-Host "OK" -F Green; $global:passed++ }
    catch { Write-Host "FAIL: $_" -F Red; $global:failed++ }
}

Write-Host "=== Mosaicraft Smoke Tests ===`n"

test "--version" { $r = & $Exe --version 2>&1; if ($r -notmatch "Mosaicraft") { throw "no version" } }
test "db-stats"  { $r = (& $Exe db-stats -d $Db 2>&1 | Out-String); if ($r -notmatch "Images") { throw "no Images" } }
test "db-health" { $r = (& $Exe db-health -d $Db 2>&1 | Out-String); if ($r -notmatch "Brightness") { throw "no Brightness" } }
test "db-usage"  { $r = (& $Exe db-usage -d $Db -n 3 2>&1 | Out-String); if ($LASTEXITCODE -ne 0) { throw "exit=$LASTEXITCODE" } }
test "mosaic"    { 
    $out = "tests/tmp_test.jpg"
    Remove-Item -Force $out -ErrorAction SilentlyContinue
    & $Exe mosaic -i $target -d $Db -o $out --no-color-adjust --benchmark 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "exit=$LASTEXITCODE" }
    if (-not (Test-Path $out)) { throw "no file" }
    Remove-Item -Force $out -ErrorAction SilentlyContinue
}
test "mosaic --analyze" {
    $out = "tests/tmp_analyze.jpg"
    Remove-Item -Force $out -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force "tests/tmp_analyze_analysis" -ErrorAction SilentlyContinue
    $r = (& $Exe mosaic -i $target -d $Db -o $out --no-color-adjust --analyze 2>&1 | Out-String)
    if ($r -notmatch "Score:") { throw "no Score" }
    Remove-Item -Force $out -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force "tests/tmp_analyze_analysis" -ErrorAction SilentlyContinue
}

Write-Host "`n=== $passed passed, $failed failed ==="
if ($failed -gt 0) { exit 1 } else { Write-Host "ALL TESTS PASSED" -F Green }
