# Downloads a full FFmpeg 8.1 build (GPL, all codecs: x264, x265, VP9, AV1, Opus, ...) into deps\ffmpeg,
# where CMake, Reno.sln and build_msvc.bat look for it first.
#   powershell -ExecutionPolicy Bypass -File tools\get_ffmpeg.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# A fixed build (BtbN keeps the end-of-month ones), checked against its SHA-256 before it is used.
$release = 'autobuild-2026-10-31-12-59'
$name = 'ffmpeg-n8.1.3-27-g8dca1586f4-win64-gpl-shared-8.1'
$sha256 = '3adc00b67118a8c4bdd6624a000e139df283b1eeb8b126fcec4849bd5b778dd4'
$url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/$release/$name.zip"
$deps = Join-Path $root 'deps'
$dest = Join-Path $deps 'ffmpeg'
New-Item -ItemType Directory -Force $deps | Out-Null
$zip = Join-Path $deps "$name.zip"
Write-Host "Downloading $url"
Invoke-WebRequest $url -OutFile $zip
$got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
if ($got -ne $sha256) {
    Remove-Item $zip
    throw "$name.zip is not the file expected (SHA-256 $got, expected $sha256)"
}
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Expand-Archive $zip -DestinationPath $deps
Move-Item (Join-Path $deps $name) $dest
Remove-Item $zip
Write-Host "FFmpeg is in $dest"
