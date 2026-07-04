param(
    [string]$WebUiExe = "",
    [int]$Port = 0,
    [int]$TimeoutSeconds = 60,
    [switch]$KeepWorkspace
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Find-WebUiExe {
    param([string]$Requested)

    if ($Requested) {
        $path = $Requested
        if (-not [System.IO.Path]::IsPathRooted($path)) {
            $path = Join-Path (Get-Location) $path
        }
        if (Test-Path -LiteralPath $path) {
            return (Resolve-Path -LiteralPath $path).Path
        }
        throw "MosaicraftWebUI executable not found: $Requested"
    }

    $candidates = @(
        (Join-Path $RepoRoot "build_nocuda\Release\bin\MosaicraftWebUI.exe"),
        (Join-Path $RepoRoot "build\Release\bin\MosaicraftWebUI.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "MosaicraftWebUI executable not found. Build MosaicraftWebUI first or pass -WebUiExe."
}

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

function Clamp-Byte {
    param([int]$Value)
    if ($Value -lt 0) { return 0 }
    if ($Value -gt 255) { return 255 }
    return $Value
}

function Write-Bmp24 {
    param(
        [string]$Path,
        [int]$Width,
        [int]$Height,
        [scriptblock]$Pixel
    )

    $rowBytes = $Width * 3
    $padding = (4 - ($rowBytes % 4)) % 4
    $imageSize = ($rowBytes + $padding) * $Height
    $fileSize = 54 + $imageSize

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    $writer = New-Object System.IO.BinaryWriter($stream)
    try {
        $writer.Write([byte]0x42)
        $writer.Write([byte]0x4D)
        $writer.Write([int]$fileSize)
        $writer.Write([int16]0)
        $writer.Write([int16]0)
        $writer.Write([int]54)
        $writer.Write([int]40)
        $writer.Write([int]$Width)
        $writer.Write([int]$Height)
        $writer.Write([int16]1)
        $writer.Write([int16]24)
        $writer.Write([int]0)
        $writer.Write([int]$imageSize)
        $writer.Write([int]2835)
        $writer.Write([int]2835)
        $writer.Write([int]0)
        $writer.Write([int]0)

        for ($y = $Height - 1; $y -ge 0; $y--) {
            for ($x = 0; $x -lt $Width; $x++) {
                $bgr = & $Pixel $x $y
                $writer.Write([byte](Clamp-Byte $bgr[0]))
                $writer.Write([byte](Clamp-Byte $bgr[1]))
                $writer.Write([byte](Clamp-Byte $bgr[2]))
            }
            for ($p = 0; $p -lt $padding; $p++) {
                $writer.Write([byte]0)
            }
        }
    }
    finally {
        $writer.Close()
        $stream.Close()
    }
}

function Write-Fixtures {
    param(
        [string]$InputDir,
        [string]$TargetPath
    )

    $colors = @(
        @(20, 20, 20),
        @(230, 230, 230),
        @(40, 40, 220),
        @(40, 190, 40),
        @(220, 70, 40),
        @(30, 210, 210),
        @(210, 40, 210),
        @(210, 160, 30),
        @(120, 60, 200),
        @(80, 180, 150)
    )

    for ($i = 0; $i -lt $colors.Count; $i++) {
        $base = $colors[$i]
        $path = Join-Path $InputDir ("tile_{0:D2}.bmp" -f $i)
        Write-Bmp24 -Path $path -Width 72 -Height 128 -Pixel {
            param($x, $y)
            $stripe = if ((($x + $y + $i) % 19) -lt 7) { 36 } else { 0 }
            @(
                (($base[0] + $x * 2 + $i * 11 + $stripe) % 256),
                (($base[1] + $y * 2 + $i * 17) % 256),
                (($base[2] + ($x + $y) + $i * 23) % 256)
            )
        }
    }

    Write-Bmp24 -Path $TargetPath -Width 36 -Height 64 -Pixel {
        param($x, $y)
        if ($x -lt 18 -and $y -lt 32) { return @(40, 40, 220) }
        if ($x -ge 18 -and $y -lt 32) { return @(40, 190, 40) }
        if ($x -lt 18 -and $y -ge 32) { return @(220, 70, 40) }
        return @(220, 220, 220)
    }
}

function Invoke-Api {
    param(
        [string]$Method,
        [string]$Path,
        $Body = $null
    )

    $uri = "$script:BaseUrl$Path"
    if ($null -eq $Body) {
        return Invoke-RestMethod -Uri $uri -Method $Method -TimeoutSec 20
    }

    $json = $Body | ConvertTo-Json -Depth 20
    return Invoke-RestMethod -Uri $uri -Method $Method -ContentType "application/json" -Body $json -TimeoutSec 20
}

function Wait-Server {
    param([System.Diagnostics.Process]$Process)

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($Process.HasExited) {
            throw "MosaicraftWebUI exited before smoke test could connect."
        }
        try {
            $pong = Invoke-Api -Method "GET" -Path "/api/ping"
            if ($pong.ok -eq $true) {
                return
            }
        }
        catch {
        }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for MosaicraftWebUI at $script:BaseUrl. stdout: $(Read-LogTail $script:StdoutPath) stderr: $(Read-LogTail $script:StderrPath)"
}

function Read-LogTail {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        return "<missing>"
    }
    $content = Get-Content -LiteralPath $Path -Raw
    if ($content.Length -gt 800) {
        return $content.Substring($content.Length - 800)
    }
    return $content
}

function Wait-JobSucceeded {
    param(
        [string]$JobId,
        [string]$Label
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $jobResponse = Invoke-Api -Method "GET" -Path ("/api/jobs/{0}" -f [System.Uri]::EscapeDataString($JobId))
        Assert-True ($jobResponse.ok -eq $true) "$Label job status request failed"
        $job = $jobResponse.job
        if ($job.state -eq "succeeded") {
            return $job
        }
        if ($job.state -eq "failed" -or $job.state -eq "canceled") {
            throw "$Label job $($job.state): $($job.message)"
        }
        Start-Sleep -Milliseconds 250
    }
    throw "$Label job timed out: $JobId"
}

$webUiPath = Find-WebUiExe -Requested $WebUiExe
if ($Port -le 0) {
    $Port = Get-Random -Minimum 18080 -Maximum 24000
}
$script:BaseUrl = "http://localhost:$Port"

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("mosaicraft_webui_smoke_" + [System.Guid]::NewGuid().ToString("N"))
$inputDir = Join-Path $tempRoot "input"
$libraryDir = Join-Path $tempRoot "library"
$outputDir = Join-Path $tempRoot "output"
$targetPath = Join-Path $tempRoot "target.bmp"
$dbPath = Join-Path $libraryDir "mosaicraft.db"
$mosaicPath = Join-Path $outputDir "mosaic.png"
$stdoutPath = Join-Path $tempRoot "webui.stdout.log"
$stderrPath = Join-Path $tempRoot "webui.stderr.log"
$script:StdoutPath = $stdoutPath
$script:StderrPath = $stderrPath

$serverProcess = $null
try {
    New-Item -ItemType Directory -Force -Path $inputDir, $libraryDir, $outputDir | Out-Null
    Write-Fixtures -InputDir $inputDir -TargetPath $targetPath

    $startArgs = @{
        FilePath = $webUiPath
        ArgumentList = @("$Port", "--no-open")
        PassThru = $true
        RedirectStandardOutput = $stdoutPath
        RedirectStandardError = $stderrPath
    }
    if ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT) {
        $startArgs.WindowStyle = "Hidden"
    }
    $serverProcess = Start-Process @startArgs
    Wait-Server -Process $serverProcess

    $info = Invoke-Api -Method "GET" -Path "/api/info"
    Assert-True ($info.ok -eq $true) "/api/info did not return ok=true"
    Assert-True ($info.info.api.structured -eq $true) "structured API is not enabled"
    Assert-True ([int]$info.info.api.contractMajorVersion -eq 1) "unexpected API contract major version"

    $endpoints = Invoke-Api -Method "GET" -Path "/api/endpoints"
    Assert-True ($endpoints.ok -eq $true) "/api/endpoints did not return ok=true"
    $operations = @($endpoints.endpoints | ForEach-Object { $_.operation })
    $required = @(
        "info", "endpoints", "submitMosaicJob", "submitBuildJob",
        "getJob", "cancelJob", "clearFinishedJobs", "databaseStats",
        "databaseHealth", "databaseUsage", "databaseUsageExport",
        "databasePurge", "inspect"
    )
    foreach ($operation in $required) {
        Assert-True ($operations -contains $operation) "missing API operation: $operation"
    }

    $build = Invoke-Api -Method "POST" -Path "/api/jobs/build" -Body @{
        inputDir = $inputDir
        outputDir = $libraryDir
        dbPath = $dbPath
        threads = 1
        forceMode = $true
        normalizeSize = "36x64"
    }
    Assert-True ($build.ok -eq $true -and $null -ne $build.job) "build job was not accepted"
    Wait-JobSucceeded -JobId $build.job.id -Label "build" | Out-Null

    $stats = Invoke-Api -Method "POST" -Path "/api/db/stats" -Body @{ dbPath = $dbPath }
    Assert-True ($stats.ok -eq $true) "database stats failed"
    Assert-True ([int]$stats.stats.total -eq 10) "database stats total should be 10"
    Assert-True ([string]$stats.stats.featureWidth -eq "36") "database feature width should be 36"
    Assert-True ([string]$stats.stats.featureHeight -eq "64") "database feature height should be 64"

    $inspect = Invoke-Api -Method "POST" -Path "/api/inspect" -Body @{
        imagePath = $targetPath
        dbPath = $dbPath
    }
    Assert-True ($inspect.ok -eq $true) "inspect failed"
    Assert-True ([int]$inspect.inspect.width -eq 36) "inspect width should be 36"
    Assert-True ([int]$inspect.inspect.height -eq 64) "inspect height should be 64"
    Assert-True ($inspect.inspect.databaseAvailable -eq $true) "inspect should see the database"

    $mosaic = Invoke-Api -Method "POST" -Path "/api/jobs/mosaic" -Body @{
        inputPath = $targetPath
        dbPath = $dbPath
        outputPath = $mosaicPath
        format = "png"
        useGpu = $false
        tileW = 9
        tileH = 16
        outW = 18
        outH = 32
        nativeTileW = 18
        nativeTileH = 32
        candidates = 10
        topNrandom = 1
        pngLevel = 1
    }
    Assert-True ($mosaic.ok -eq $true -and $null -ne $mosaic.job) "mosaic job was not accepted"
    Wait-JobSucceeded -JobId $mosaic.job.id -Label "mosaic" | Out-Null
    Assert-True (Test-Path -LiteralPath $mosaicPath) "mosaic output was not created"

    $usage = Invoke-Api -Method "POST" -Path "/api/db/usage" -Body @{
        dbPath = $dbPath
        limit = 10
        showUnused = $true
    }
    Assert-True ($usage.ok -eq $true) "database usage failed"

    Invoke-Api -Method "DELETE" -Path "/api/jobs" | Out-Null
    Write-Host "Web UI/API smoke passed: $script:BaseUrl"
}
finally {
    if ($null -ne $serverProcess -and -not $serverProcess.HasExited) {
        Stop-Process -Id $serverProcess.Id -Force
        $serverProcess.WaitForExit(5000) | Out-Null
    }
    if (-not $KeepWorkspace -and (Test-Path -LiteralPath $tempRoot)) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
    elseif ($KeepWorkspace) {
        Write-Host "Smoke workspace kept: $tempRoot"
    }
}
