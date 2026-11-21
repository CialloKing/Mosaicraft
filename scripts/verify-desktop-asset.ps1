# Static archive inspection only: no extraction or execution of packaged programs.
param([Parameter(Mandatory)][string]$ArchivePath, [string]$ExpectedSha256)

$ErrorActionPreference = 'Stop'
$archive = Get-Item -LiteralPath $ArchivePath
if ($ExpectedSha256 -and (Get-FileHash -LiteralPath $archive.FullName).Hash -ne $ExpectedSha256) {
    throw 'Desktop archive SHA-256 mismatch'
}
if ($archive.Name -notmatch '^(Mosaicraft_v[0-9]+\.[0-9]+\.[0-9]+_(windows-x64|linux-x64|macos-arm64)_qt)\.(zip|tar\.gz)$') {
    throw 'Unexpected desktop archive filename'
}
$stem, $platform, $extension = $Matches[1], $Matches[2], $Matches[3]
if (($platform -eq 'windows-x64') -ne ($extension -eq 'zip')) { throw 'Wrong archive format for the target platform' }
$entries = @(& cmake -E tar tf $archive.FullName)
if ($LASTEXITCODE -ne 0 -or !$entries.Count) { throw 'Cannot list desktop archive' }
$required = @('BUILD_INFO.txt', 'LICENSE', 'desktop-redistribution.txt', 'licenses/qt/LGPL-3.0-only.txt')
$modules = @('Core', 'Gui', 'Widgets', 'Sql', 'Network')
$plugins = @('sqldrivers/qsqlite', 'imageformats/qwebp', 'imageformats/qtiff')
switch ($platform) {
    'windows-x64' {
        $required += @('bin/MosaicraftDesktop.exe', 'bin/mosaicraft.exe', 'bin/mosaicraft_desktop_worker.exe', 'bin/qt.conf', 'bin/contracts.json', 'bin/vcruntime140.dll')
        $required += $modules | ForEach-Object { "bin/Qt6$_.dll" }
        $patterns = ($plugins + 'platforms/qwindows') | ForEach-Object { "*/$_.dll" }
    }
    'linux-x64' {
        $required += @('bin/MosaicraftDesktop', 'bin/mosaicraft', 'bin/mosaicraft_desktop_worker', 'bin/qt.conf', 'bin/contracts.json')
        $patterns = @($modules | ForEach-Object { "*/libQt6$_.so.6" })
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
