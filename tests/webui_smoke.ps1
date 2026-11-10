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

    $response = Invoke-ApiResponse -Method $Method -Path $Path -Body $Body
    if ($response.StatusCode -lt 200 -or $response.StatusCode -gt 299) {
        throw "HTTP $($response.StatusCode) from $Path`: $($response.Text)"
    }
    return $response.Json
}

function Read-ErrorResponseText {
    param($ErrorRecord)

    if ($ErrorRecord.ErrorDetails -and $ErrorRecord.ErrorDetails.Message) {
        return $ErrorRecord.ErrorDetails.Message
    }
    $response = $ErrorRecord.Exception.Response
    if ($null -eq $response) {
        return ""
    }
    $stream = $response.GetResponseStream()
    if ($null -eq $stream) {
        return ""
    }
    $reader = New-Object System.IO.StreamReader($stream)
    try {
        return $reader.ReadToEnd()
    }
    finally {
        $reader.Close()
    }
}

function Convert-ResponseJson {
    param([string]$Text)

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return $null
    }
    try {
        return $Text | ConvertFrom-Json
    }
    catch {
        return $null
    }
}

function Invoke-ApiResponse {
    param(
        [string]$Method,
        [string]$Path,
        $Body = $null,
        [switch]$RawBody
    )

    $uri = "$script:BaseUrl$Path"
    $request = @{
        Uri = $uri
        Method = $Method
        TimeoutSec = 20
    }
    if ($PSVersionTable.PSVersion.Major -lt 6) {
        $request.UseBasicParsing = $true
    }
    else {
        $request.SkipHttpErrorCheck = $true
    }
    if ($null -ne $Body) {
        $request.ContentType = "application/json; charset=utf-8"
        $json = if ($RawBody) { [string]$Body } else { $Body | ConvertTo-Json -Depth 20 }
        # Windows PowerShell 5.1 otherwise encodes non-ASCII request strings as ASCII.
        $request.Body = [System.Text.Encoding]::UTF8.GetBytes($json)
    }

    try {
        $response = Invoke-WebRequest @request
        $text = [string]$response.Content
        return [pscustomobject]@{
            StatusCode = [int]$response.StatusCode
            Json = Convert-ResponseJson -Text $text
            Text = $text
        }
    }
    catch {
        $response = $_.Exception.Response
        if ($null -eq $response) {
            throw
        }
        $text = Read-ErrorResponseText -ErrorRecord $_
        return [pscustomobject]@{
            StatusCode = [int]$response.StatusCode
            Json = Convert-ResponseJson -Text $text
            Text = $text
        }
    }
}

function Wait-Server {
    param([System.Diagnostics.Process]$Process)

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($Process.HasExited) {
            throw "MosaicraftWebUI exited ($($Process.ExitCode)) before smoke test could connect. stdout: $(Read-LogTail $script:StdoutPath) stderr: $(Read-LogTail $script:StderrPath)"
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
        [string]$Label,
        [string]$ExpectedType = "",
        [string]$ExpectedInputPath = "",
        [string]$ExpectedOutputPath = ""
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $jobResponse = Invoke-Api -Method "GET" -Path ("/api/jobs/{0}" -f [System.Uri]::EscapeDataString($JobId))
        Assert-True ($jobResponse.ok -eq $true) "$Label job status request failed"
        $job = $jobResponse.job
        if ($job.state -eq "succeeded") {
            Assert-JobSnapshot -Job $job -ExpectedType $ExpectedType -ExpectedInputPath $ExpectedInputPath -ExpectedOutputPath $ExpectedOutputPath
            Assert-True ([int64]$job.startedAt -gt 0) "$Label job startedAt should be set"
            Assert-True ([int64]$job.finishedAt -ge [int64]$job.startedAt) "$Label job finishedAt should be >= startedAt"
            return $job
        }
        if ($job.state -eq "failed" -or $job.state -eq "canceled") {
            throw "$Label job $($job.state): $($job.message)"
        }
        Start-Sleep -Milliseconds 250
    }
    throw "$Label job timed out: $JobId"
}

function Assert-JobSnapshot
{
    param(
        $Job,
        [string]$ExpectedType = "",
        [string]$ExpectedInputPath = "",
        [string]$ExpectedOutputPath = ""
    )

    # Job snapshots drive the Web UI task card, so smoke tests lock down these fields.
    Assert-True ($null -ne $Job) "job snapshot is missing"
    Assert-True (-not [string]::IsNullOrWhiteSpace([string]$Job.id)) "job id is missing"
    Assert-True (-not [string]::IsNullOrWhiteSpace([string]$Job.state)) "job state is missing"
    Assert-True ([int64]$Job.createdAt -gt 0) "job createdAt should be set"
    Assert-True ($null -ne $Job.ok) "job ok flag is missing"
    Assert-True ($null -ne $Job.exitCode) "job exitCode is missing"

    if ($ExpectedType)
    {
        Assert-True ([string]$Job.type -eq $ExpectedType) "job type should be $ExpectedType"
    }
    if ($ExpectedInputPath)
    {
        Assert-True ([string]$Job.inputPath -eq $ExpectedInputPath) "job inputPath should round-trip"
    }
    if ($ExpectedOutputPath)
    {
        Assert-True ([string]$Job.outputPath -eq $ExpectedOutputPath) "job outputPath should round-trip"
    }
}

$webUiPath = Find-WebUiExe -Requested $WebUiExe
if ($Port -le 0) {
    $Port = Get-Random -Minimum 18080 -Maximum 24000
}
$script:BaseUrl = "http://localhost:$Port"

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("mosaicraft 中文 smoke_" + [System.Guid]::NewGuid().ToString("N"))
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
        WorkingDirectory = $tempRoot
        PassThru = $true
        RedirectStandardOutput = $stdoutPath
        RedirectStandardError = $stderrPath
    }
    if ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT) {
        $startArgs.WindowStyle = "Hidden"
    }
    $serverProcess = Start-Process @startArgs
    Wait-Server -Process $serverProcess
    $page = Invoke-WebRequest -Uri "$script:BaseUrl/" -TimeoutSec $TimeoutSeconds
    Assert-True ($page.Content -match '(?i)<!doctype html>') 'Web UI should serve its HTML document'

    $info = Invoke-Api -Method "GET" -Path "/api/info"
    Assert-True ($info.ok -eq $true) "/api/info did not return ok=true"
    Assert-True ($info.info.api.structured -eq $true) "structured API is not enabled"
    Assert-True ([int]$info.info.api.contractMajorVersion -eq 2) "unexpected API contract major version"
    Assert-True ($info.info.api.contractVersion -eq '2.2') "unexpected API contract version"
    Assert-True ($info.info.features -contains 'output-format-policy') "output policy capability missing"

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
    foreach ($operation in @("mosaic", "submitMosaicJob")) {
        $overlayEndpoint = @($endpoints.endpoints | Where-Object { $_.operation -eq $operation })[0]
        Assert-True ($overlayEndpoint.requestFields -contains "overlayStrength") "overlayStrength missing from endpoint discovery"
    }
    $uiHtml = (Invoke-WebRequest -Uri "$script:BaseUrl/" -UseBasicParsing).Content
    Assert-True ($uiHtml -match 'id="m-overlaystr"[^>]+value="0"[^>]+step="0.05"') "overlay control defaults missing from Web UI"

    $badJson = Invoke-ApiResponse -Method "POST" -Path "/api/jobs/mosaic" -Body "{" -RawBody
    Assert-True ($badJson.StatusCode -eq 400) "malformed JSON should return HTTP 400"
    Assert-True ($badJson.Json.ok -eq $false) "malformed JSON should return ok=false"
    Assert-True ([string]$badJson.Json.message -like "invalid JSON body:*") "malformed JSON message should be explicit"

    $badTile = Invoke-ApiResponse -Method "POST" -Path "/api/jobs/mosaic" -Body @{
        inputPath = $targetPath
        tileW = 3
    }
    Assert-True ($badTile.StatusCode -eq 400) "invalid tileW should return HTTP 400"
    Assert-True ($badTile.Json.ok -eq $false) "invalid tileW should return ok=false"
    Assert-True ($badTile.Json.message -eq "tileW must be at least 4") "invalid tileW message should be explicit"

    foreach ($strength in @(-0.01, 0.51)) {
        $badStrength = Invoke-ApiResponse -Method "POST" -Path "/api/jobs/mosaic" -Body @{
            inputPath = $targetPath
            colorAdjust = $true
            colorStrength = $strength
        }
        Assert-True ($badStrength.StatusCode -eq 400) "invalid colorStrength should return HTTP 400"
        Assert-True ($badStrength.Json.message -eq "colorStrength must be between 0.000000 and 0.500000") "colorStrength bounds should be explicit"
    }

    $badUsage = Invoke-ApiResponse -Method "GET" -Path "/api/db/usage?db=missing.db&limit=0"
    foreach ($strength in @(-0.01, 1.01, "NaN", "Infinity")) {
        $badOverlay = Invoke-ApiResponse -Method "POST" -Path "/api/jobs/mosaic" -Body @{
            inputPath = $targetPath
            overlayStrength = $strength
        }
        Assert-True ($badOverlay.StatusCode -eq 400) "invalid overlayStrength should return HTTP 400"
        Assert-True ($badOverlay.Json.ok -eq $false) "invalid overlayStrength should return ok=false"
    }
    Assert-True ($badUsage.StatusCode -eq 400) "invalid usage limit should return HTTP 400"
    Assert-True ($badUsage.Json.ok -eq $false) "invalid usage limit should return ok=false"
    Assert-True ($badUsage.Json.message -eq "limit must be at least 1") "invalid usage limit message should be explicit"

    $missingJob = Invoke-ApiResponse -Method "GET" -Path "/api/jobs/not-found"
    Assert-True ($missingJob.StatusCode -eq 404) "missing job should return HTTP 404"
    Assert-True ($missingJob.Json.ok -eq $false) "missing job should return ok=false"

    $legacyRun = Invoke-ApiResponse -Method "POST" -Path "/api/run" -Body "mosaicraft db-stats" -RawBody
    Assert-True ($legacyRun.StatusCode -eq 404) "disabled legacy run should return HTTP 404"
    Assert-True ($legacyRun.Json.ok -eq $false) "disabled legacy run should return ok=false"

    $build = Invoke-Api -Method "POST" -Path "/api/jobs/build" -Body @{
        inputDir = $inputDir
        outputDir = $libraryDir
        dbPath = $dbPath
        threads = 1
        forceMode = $true
        normalizeSize = "36x64"
    }
    Assert-True ($build.ok -eq $true -and $null -ne $build.job) "build job was not accepted"
    Assert-JobSnapshot -Job $build.job -ExpectedType "build" -ExpectedInputPath $inputDir -ExpectedOutputPath $libraryDir
    Wait-JobSucceeded -JobId $build.job.id -Label "build" -ExpectedType "build" -ExpectedInputPath $inputDir -ExpectedOutputPath $libraryDir | Out-Null

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

    $mosaicBody = @{
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
        topNRandom = 1
        pngLevel = 1
    }
    $mosaic = Invoke-Api -Method "POST" -Path "/api/jobs/mosaic" -Body $mosaicBody
    Assert-True ($mosaic.ok -eq $true -and $null -ne $mosaic.job) "mosaic job was not accepted"
    Assert-JobSnapshot -Job $mosaic.job -ExpectedType "mosaic" -ExpectedInputPath $targetPath -ExpectedOutputPath $mosaicPath
    Wait-JobSucceeded -JobId $mosaic.job.id -Label "mosaic" -ExpectedType "mosaic" -ExpectedInputPath $targetPath -ExpectedOutputPath $mosaicPath | Out-Null
    Assert-True (Test-Path -LiteralPath $mosaicPath) "mosaic output was not created"

    foreach ($case in @(
        @{ Name = "zero"; Strength = 0.0 },
        @{ Name = "default"; Strength = 0.04 },
        @{ Name = "maximum"; Strength = 0.5 }
    )) {
        $adjustedBody = $mosaicBody.Clone()
        $adjustedBody.outputPath = Join-Path $outputDir ("adjusted-" + $case.Name + ".png")
        $adjustedBody.colorAdjust = $true
        $adjustedBody.colorStrength = $case.Strength
        $adjustedBody.useGpu = $true
        $adjusted = Invoke-Api -Method "POST" -Path "/api/jobs/mosaic" -Body $adjustedBody
        Assert-True ($adjusted.ok -eq $true) "brightness job was not accepted"
        Wait-JobSucceeded -JobId $adjusted.job.id -Label ("brightness-" + $case.Name) -ExpectedType "mosaic" -ExpectedInputPath $targetPath -ExpectedOutputPath $adjustedBody.outputPath | Out-Null
        $adjustedInspect = Invoke-Api -Method "POST" -Path "/api/inspect" -Body @{ imagePath = $adjustedBody.outputPath }
        Assert-True ($adjustedInspect.ok -eq $true) "adjusted PNG should decode"
        Assert-True ([int]$adjustedInspect.inspect.width -eq 36 -and [int]$adjustedInspect.inspect.height -eq 64) "adjusted PNG dimensions should match"
    }

    foreach ($strength in @(0.0, 0.25, 1.0)) {
        $overlayBody = $mosaicBody.Clone()
        $overlayBody.outputPath = Join-Path $outputDir ("overlay-" + $strength + ".png")
        $overlayBody.overlayStrength = $strength
        if ($strength -eq 0.25) {
            $overlayResult = Invoke-Api -Method "POST" -Path "/api/mosaic" -Body $overlayBody
            Assert-True ($overlayResult.ok -eq $true) "synchronous overlay failed"
        }
        else {
            $overlayResult = Invoke-Api -Method "POST" -Path "/api/jobs/mosaic" -Body $overlayBody
            Assert-True ($overlayResult.ok -eq $true) "overlay job was not accepted"
            Wait-JobSucceeded -JobId $overlayResult.job.id -Label "overlay" -ExpectedType "mosaic" -ExpectedInputPath $targetPath -ExpectedOutputPath $overlayBody.outputPath | Out-Null
        }
        $overlayInspect = Invoke-Api -Method "POST" -Path "/api/inspect" -Body @{ imagePath = $overlayBody.outputPath }
        Assert-True ($overlayInspect.ok -eq $true) "overlay PNG should decode"
        Assert-True ([int]$overlayInspect.inspect.width -eq 36 -and [int]$overlayInspect.inspect.height -eq 64) "overlay PNG dimensions should match"
    }

    # 窄条覆盖真实编码及同步/异步结果，不需要写入巨型测试图片。
    foreach ($case in @(
        @{ Name='auto-jpg'; Format='jpg'; Explicit=$false; Tile=33000; Expected='png'; Width=66000 },
        @{ Name='explicit-jpg'; Format='jpg'; Explicit=$true; Tile=33000; Expected='jpg'; Width=65500 },
        @{ Name='auto-webp'; Format='webp'; Explicit=$false; Tile=8200; Expected='png'; Width=16400 },
        @{ Name='explicit-webp'; Format='webp'; Explicit=$true; Tile=8200; Expected='webp'; Width=16382 }
    )) {
        $policyBody = $mosaicBody.Clone()
        $policyBody.Remove('format')
        $policyBody.outputPath = Join-Path $outputDir ($case.Name + '.jpg')
        $policyBody.outW = 18
        $policyBody.outH = 16
        $policyBody.nativeTileW = $case.Tile
        $policyBody.nativeTileH = 1
        $policyBody.lRange = 255
        if ($case.Explicit) { $policyBody.format = $case.Format }
        else { $policyBody.outputPath = Join-Path $outputDir ($case.Name + '.' + $case.Format) }
        if ($case.Explicit) {
            $policy = Invoke-Api -Method 'POST' -Path '/api/mosaic' -Body $policyBody
        }
        else {
            $submitted = Invoke-Api -Method 'POST' -Path '/api/jobs/mosaic' -Body $policyBody
            $policy = Wait-JobSucceeded -JobId $submitted.job.id -Label $case.Name -ExpectedType 'mosaic' -ExpectedInputPath $targetPath -ExpectedOutputPath $policyBody.outputPath
            Assert-True (-not ($policy.request.PSObject.Properties.Name -contains 'format')) 'inferred format became explicit restoration data'
        }
        $plan = $policy.runtime.outputPlan
        Assert-True ($plan.format -eq $case.Expected -and [int]$plan.width -eq $case.Width) 'output plan dimensions/format incorrect'
        Assert-True ([int]$policy.runtime.width -eq $case.Width -and $policy.runtime.format -eq $case.Expected) 'saved output disagrees with plan'
        Assert-True ($plan.scaled -eq $case.Explicit -and $plan.formatChanged -ne $case.Explicit) 'wrong output adjustment policy'
        $saved = @($policy.artifacts | Where-Object id -eq 'main')[0]
        Assert-True ($saved.path.EndsWith('.' + $case.Expected)) 'actual suffix incorrect'
        Assert-True (Test-Path -LiteralPath $saved.path) 'planned output was not saved'
        $decoded = Invoke-Api -Method 'POST' -Path '/api/inspect' -Body @{imagePath=$saved.path}
        Assert-True ([int]$decoded.inspect.width -eq $case.Width -and [int]$decoded.inspect.height -eq 1) 'encoded image dimensions incorrect'
        if ($case.Name -eq 'explicit-jpg') { $jpgForInference = $saved.path }
    }
    foreach ($source in @(
        @{ Path=$targetPath; Format='png'; Source='fallback' },
        @{ Path=$jpgForInference; Format='jpg'; Source='input_extension' }
    )) {
        $defaultOutput = $mosaicBody.Clone()
        $defaultOutput.Remove('outputPath')
        $defaultOutput.Remove('format')
        $defaultOutput.inputPath = $source.Path
        $defaultOutput.lRange = 255
        $defaultResult = Invoke-Api -Method 'POST' -Path '/api/mosaic' -Body $defaultOutput
        Assert-True ($defaultResult.runtime.outputPlan.formatSource -eq $source.Source) 'default filename prevented input format selection'
        Assert-True ($defaultResult.runtime.format -eq $source.Format) 'input suffix/fallback selected wrong format'
        $defaultMain = @($defaultResult.artifacts | Where-Object id -eq 'main')[0]
        $expectedDefault = Join-Path $outputDir ('output.' + $source.Format)
        Assert-True (Test-Path -LiteralPath $expectedDefault) 'default filename did not append actual suffix'
        Assert-True ((Get-FileHash -LiteralPath $defaultMain.path).Hash -eq (Get-FileHash -LiteralPath $expectedDefault).Hash) 'default artifact differs from expected file'
    }
    $invalidSuffix = $mosaicBody.Clone()
    $invalidSuffix.Remove('format')
    $invalidSuffix.outputPath = Join-Path $outputDir 'unsupported.bmp'
    $invalidResult = Invoke-ApiResponse -Method 'POST' -Path '/api/jobs/mosaic' -Body $invalidSuffix
    Assert-True ($invalidResult.StatusCode -eq 400 -and $invalidResult.Json.diagnostic.field -eq 'outputPath') 'unsupported output suffix must identify outputPath'
    $invalidSuffix.format = 'png'
    $override = Invoke-Api -Method 'POST' -Path '/api/mosaic' -Body $invalidSuffix
    Assert-True ($override.ok -and $override.runtime.format -eq 'png') 'explicit format should override an unsupported suffix'

    $usage = Invoke-Api -Method "POST" -Path "/api/db/usage" -Body @{
        dbPath = $dbPath
        limit = 10
        showUnused = $true
    }
    Assert-True ($usage.ok -eq $true) "database usage failed"

    # Real file responses and cursored diagnostics, independent of HTML text assertions.
    Assert-True (-not [string]::IsNullOrWhiteSpace($info.info.runtime.sessionId)) "session id missing"
    Assert-True (-not [string]::IsNullOrWhiteSpace($info.info.runtime.workingDirectory)) "working directory missing"
    $previewBody = $mosaicBody.Clone()
    $previewBody.outputPath = Join-Path $outputDir "preview result.jpg"
    $previewBody.generatePreview = $true
    $previewJob = Invoke-Api -Method "POST" -Path "/api/jobs/mosaic" -Body $previewBody
    $completed = Wait-JobSucceeded -JobId $previewJob.job.id -Label "preview" -ExpectedType "mosaic" -ExpectedInputPath $targetPath -ExpectedOutputPath $previewBody.outputPath
    Assert-True ($completed.progress.stage -eq "finalizing") "final progress stage missing"
    Assert-True ($completed.canCancel -eq $false) "finished job must not be cancelable"
    Assert-True ($completed.request.generatePreview -eq $true) "request restoration data missing"
    $main = @($completed.artifacts | Where-Object { $_.id -eq 'main' })[0]
    $preview = @($completed.artifacts | Where-Object { $_.id -eq 'preview' })[0]
    Assert-True ($main.path.EndsWith('.png')) "artifact must use actual output extension"
    $events = Invoke-Api -Method "GET" -Path ("/api/jobs/{0}/events?after=0&limit=100" -f $completed.id)
    Assert-True ($events.events.Count -gt 0) "runtime events missing"
    $emptyEvents = Invoke-Api -Method "GET" -Path ("/api/jobs/{0}/events?after={1}" -f $completed.id, $events.nextCursor)
    Assert-True ($emptyEvents.events.Count -eq 0) "event cursor repeated old events"
    $download = Join-Path $outputDir 'download.bin'
    $assetUrl = "$script:BaseUrl/api/jobs/$($completed.id)/artifacts/main"
    Invoke-WebRequest -Uri $assetUrl -OutFile $download | Out-Null
    Assert-True ((Get-FileHash -LiteralPath $download).Hash -eq (Get-FileHash -LiteralPath $main.path).Hash) "download differs from saved output"
    $rangeFile = Join-Path $outputDir 'range.bin'
    $rangeRequest = [System.Net.HttpWebRequest]::Create($assetUrl)
    $rangeRequest.AddRange(0, 31)
    $rangeResponse = $rangeRequest.GetResponse()
    $rangeOutput = [IO.File]::Create($rangeFile)
    try {
        Assert-True ([int]$rangeResponse.StatusCode -eq 206) "range response should use HTTP 206"
        $rangeResponse.GetResponseStream().CopyTo($rangeOutput)
    }
    finally {
        $rangeOutput.Dispose()
        $rangeResponse.Close()
    }
    Assert-True ((Get-Item -LiteralPath $rangeFile).Length -eq 32) "range response should use requested length"
    [IO.File]::AppendAllText($main.path, 'changed')
    $changed = Invoke-ApiResponse -Method "GET" -Path ("/api/jobs/{0}/artifacts/main" -f $completed.id)
    Assert-True ($changed.StatusCode -eq 409) "overwritten artifact should return 409"
    Remove-Item -LiteralPath $preview.path
    $missingArtifact = Invoke-ApiResponse -Method "GET" -Path ("/api/jobs/{0}/artifacts/preview" -f $completed.id)
    Assert-True ($missingArtifact.StatusCode -eq 404) "deleted artifact should return 404"

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
