# Downloads a full FFmpeg 8.1 build (GPL, all codecs: x264, x265, VP9, AV1, Opus, ...) into deps\ffmpeg,
# where CMake, Reno.sln and build_msvc.bat look for it first.
#   powershell -ExecutionPolicy Bypass -File tools\get_ffmpeg.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$name = 'ffmpeg-n8.1-latest-win64-gpl-shared-8.1'
$url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/$name.zip"
$deps = Join-Path $root 'deps'
$dest = Join-Path $deps 'ffmpeg'
New-Item -ItemType Directory -Force $deps | Out-Null
$zip = Join-Path $deps "$name.zip"
Write-Host "Downloading $url"
Invoke-WebRequest $url -OutFile $zip
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Expand-Archive $zip -DestinationPath $deps
Move-Item (Join-Path $deps $name) $dest
Remove-Item $zip
Write-Host "FFmpeg is in $dest"
