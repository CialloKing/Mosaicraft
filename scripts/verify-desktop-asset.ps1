param(
    [Parameter(Mandatory)][string]$ArchivePath,
    [string]$ExpectedSha256,
    [string]$ExpectedCommit,
    [switch]$RunSmoke,
    [switch]$RequireGpu
)

$ErrorActionPreference = 'Stop'
$archive = Get-Item -LiteralPath $ArchivePath
if ($ExpectedSha256 -and (Get-FileHash -LiteralPath $archive.FullName).Hash -ne $ExpectedSha256) {
    throw 'Desktop archive SHA-256 mismatch'
}
if ($archive.Name -notmatch '^(?<stem>Mosaicraft_v(?<version>[0-9]+\.[0-9]+\.[0-9]+)_(?<platform>windows-x64|linux-x64|macos-arm64)_qt(?<gpu>_cuda-vulkan(?<candidate>_candidate)?)?)\.(?<ext>zip|tar\.gz)$') {
    throw 'Unexpected desktop archive filename'
}
$stem, $platform, $extension, $version = $Matches.stem, $Matches.platform, $Matches.ext, $Matches.version
$gpu = [bool]$Matches.gpu
$candidate = [bool]$Matches.candidate
if (($gpu -and $platform -eq 'macos-arm64') -or ($candidate -and $platform -ne 'windows-x64') -or
    ($RequireGpu -and (!$RunSmoke -or !$gpu -or $candidate))) { throw 'Unsupported desktop archive validation mode' }
if (($platform -eq 'windows-x64') -ne ($extension -eq 'zip')) { throw 'Wrong archive format for the target platform' }
$entries = @(& cmake -E tar tf $archive.FullName)
if ($LASTEXITCODE -ne 0 -or !$entries.Count) { throw 'Cannot list desktop archive' }
$required = @('BUILD_INFO.txt', 'LICENSE', 'desktop-redistribution.txt', 'licenses/qt/LGPL-3.0-only.txt')
$modules = @('Core', 'Gui', 'Widgets', 'Network')
$plugins = @('imageformats/qwebp', 'imageformats/qtiff')
switch ($platform) {
    'windows-x64' {
        $required += @('bin/MosaicraftDesktop.exe', 'bin/mosaicraft.exe', 'bin/mosaicraft_desktop_worker.exe', 'bin/qt.conf', 'bin/contracts.json', 'bin/vcruntime140.dll')
        $required += $modules | ForEach-Object { "bin/Qt6$_.dll" }
        $patterns = ($plugins + 'platforms/qwindows') | ForEach-Object { "*/$_.dll" }
    }
    'linux-x64' {
        $required += @('bin/MosaicraftDesktop', 'bin/mosaicraft', 'bin/mosaicraft_desktop_worker', 'bin/qt.conf', 'bin/contracts.json')
        $required += 'licenses/qt/ICU-73.2.txt'
        $patterns = @($modules | ForEach-Object { "*/libQt6$_.so.6" })
        $patterns += '*/libicudata.so.73', '*/libicui18n.so.73', '*/libicuuc.so.73'
        $patterns += ($plugins + 'platforms/qxcb' + 'platforms/qwayland') | ForEach-Object { '*/' + ($_ -replace '/q', '/libq') + '.so' }
    }
    'macos-arm64' {
        $app = 'MosaicraftDesktop.app/Contents'
        $required += @("$app/Info.plist", "$app/MacOS/MosaicraftDesktop", "$app/MacOS/mosaicraft", "$app/MacOS/mosaicraft_desktop_worker", "$app/MacOS/contracts.json", "$app/_CodeSignature/CodeResources")
        $patterns = @($modules | ForEach-Object { "$app/Frameworks/Qt$_.framework/Versions/*/Qt$_" })
        $patterns += ($plugins + 'platforms/qcocoa') | ForEach-Object { "$app/PlugIns/" + ($_ -replace '/q', '/libq') + '.dylib' }
    }
}
foreach ($name in $required) {
    if ("$stem/$name" -cnotin $entries) { throw "Desktop archive is missing $name" }
}
foreach ($pattern in $patterns) {
    if (!($entries -clike "$stem/$pattern")) { throw "Desktop archive is missing $pattern" }
}
Write-Output "Verified desktop archive layout: $($archive.Name)"

if ($ExpectedCommit -or $gpu) {
    $info = & tar -xOf $archive.FullName "$stem/BUILD_INFO.txt" | Out-String | ConvertFrom-StringData -Delimiter ':'
    if ($LASTEXITCODE -ne 0 -or $info.Platform -ne $platform -or $info.Version -ne $version -or
        ($ExpectedCommit -and $info.GitCommit -ne $ExpectedCommit)) { throw 'Desktop archive metadata does not match' }
    if ($gpu) {
        $states = if ($candidate) { 'not-run' } elseif ($RequireGpu -or $platform -eq 'windows-x64') { 'passed' } else { 'not-run', 'passed' }
        if ($info.Runtime -ne 'cuda-vulkan' -or $info.CudaBackend -ne 'enabled' -or
            $info.VulkanBackend -ne 'enabled' -or $info.GpuValidation -notin $states -or
            $info.PortableGpuValidation -ne $info.GpuValidation -or $info.PortableAutoSelection -ne 'cuda-to-cpu') {
            throw 'Incorrect desktop GPU metadata'
        }
        if (!($entries -clike "$stem/bin/licenses/cuda/*")) { throw 'Missing CUDA notices' }
        if ($platform -eq 'linux-x64' -and !($entries -clike "$stem/bin/lib/libcudart.so.*")) { throw 'Missing CUDA runtime' }
    } elseif ($info.Runtime -ne 'cpu-only') { throw 'Incorrect desktop CPU metadata' }
}
if (!$RunSmoke) { return }
if (($IsWindows -and $platform -ne 'windows-x64') -or ($IsLinux -and $platform -ne 'linux-x64') -or $IsMacOS) {
    throw 'Run this desktop smoke on the target Windows/Linux platform'
}
$root = (New-Item -ItemType Directory -Path (Join-Path $archive.DirectoryName "smoke-$([guid]::NewGuid())")).FullName
$envBackup = @{}
foreach ($key in @('PATH','LD_LIBRARY_PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QT_DEBUG_PLUGINS','MOSAICRAFT_DESKTOP_SMOKE_CLI')) {
    $envBackup[$key] = [Environment]::GetEnvironmentVariable($key)
}
Push-Location -LiteralPath $root
try {
    & cmake -E tar xf $archive.FullName
    if ($LASTEXITCODE -ne 0) { throw 'Desktop archive extraction failed' }
    foreach ($key in @('LD_LIBRARY_PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH')) {
        [Environment]::SetEnvironmentVariable($key, $null, 'Process')
    }
    $bin = Join-Path $root "$stem/bin"
    if ($IsWindows) {
        $env:PATH = "$env:WINDIR/System32;$env:WINDIR"
        $env:MOSAICRAFT_DESKTOP_SMOKE_CLI = Join-Path $bin 'mosaicraft.exe'
        $gui = Start-Process -FilePath (Join-Path $bin 'MosaicraftDesktop.exe') -ArgumentList '--smoke-test','-platform','windows' -WindowStyle Hidden -PassThru
        if (!$gui.WaitForExit(180000)) { $gui.Kill($true); throw 'Packaged desktop smoke timed out' }
        if ($gui.ExitCode -ne 0) { throw "Packaged desktop smoke failed ($($gui.ExitCode))" }
    } else {
        $env:PATH = '/usr/bin:/bin'
        $env:MOSAICRAFT_DESKTOP_SMOKE_CLI = Join-Path $bin 'mosaicraft'
        $env:QT_DEBUG_PLUGINS = '1'  # Report the real load error rather than Qt's generic XCB hint.
        & timeout 180 xvfb-run -a (Join-Path $bin 'MosaicraftDesktop') --smoke-test -platform xcb 2>(Join-Path $root 'gui.log')
        if ($LASTEXITCODE -ne 0) {
            Get-Content -LiteralPath (Join-Path $root 'gui.log') -Tail 60 | Out-Host
            throw 'Packaged desktop smoke failed'
        }
    }
    Write-Output 'Packaged desktop smoke passed'
    if ($gpu) { & (Join-Path $PSScriptRoot 'test-desktop-gpu.ps1') -Bin $bin -RequireGpu:$RequireGpu }
} finally {
    Pop-Location
    foreach ($key in $envBackup.Keys) { [Environment]::SetEnvironmentVariable($key, $envBackup[$key], 'Process') }
}
