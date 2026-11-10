# Makes a built Reno run on Windows 7: replaces the Qt DLLs beside reno.exe (put there by windeployqt from the usual Qt
# 6.8) with the same DLLs patched to run on Windows 7 and 8 (github.com/crystalidea/qt6windows7; Qt keeps the DLLs of one
# 6.8 release usable by programs built with another). Only files Reno already has are replaced; the download is kept in
# deps\qt6win7.
#   powershell -ExecutionPolicy Bypass -File tools\use_qt_win7.ps1 build\out-win7
param([Parameter(Mandatory = $true)][string]$Out, [string]$Version = '6.8.4')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root 'deps'
$dest = Join-Path $deps "qt6win7-$Version"
if (-not (Test-Path $dest)) {
    New-Item -ItemType Directory -Force $dest | Out-Null
    $archive = Join-Path $deps "qt6win7-$Version.7z"
    $url = "https://github.com/crystalidea/qt6windows7/releases/download/v$Version/qt6_x64_to_run_on_windows7.7z"
    Write-Host "Downloading $url"
    Invoke-WebRequest $url -OutFile $archive
    $sevenZip = (Get-Command 7z -ErrorAction SilentlyContinue).Source
    if (-not $sevenZip) { $sevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe' }
    if (-not (Test-Path $sevenZip)) { throw '7-Zip is needed to unpack the Windows 7 Qt DLLs (7-zip.org)' }
    & $sevenZip x $archive "-o$dest" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "could not unpack $archive" }
    Remove-Item $archive
}
# the folder of the download holding Qt6Core.dll is the one its paths (platforms\qwindows.dll, ...) are relative to
$core = Get-ChildItem $dest -Recurse -Filter 'Qt6Core.dll' | Select-Object -First 1
if (-not $core) { throw "no Qt6Core.dll in $dest" }
$base = $core.DirectoryName
$outFull = (Resolve-Path $Out).Path
$replaced = 0
foreach ($f in Get-ChildItem $base -Recurse -Filter '*.dll') {
    $rel = $f.FullName.Substring($base.Length).TrimStart('\')
    $target = Join-Path $outFull $rel
    if (Test-Path $target) {
        Copy-Item $f.FullName $target -Force
        $replaced++
    }
}
if ($replaced -eq 0) { throw "none of Reno's Qt DLLs were in the download" }
Write-Host "Replaced $replaced Qt DLLs in $outFull with the Windows 7 ones (Qt $Version)"
