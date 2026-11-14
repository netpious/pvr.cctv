[CmdletBinding()]
param(
  [string]$BuildDir = "build",
  [string]$Config = "Release",
  [string]$KodiInclude = $env:KODI_INCLUDE_DIR
)

$ErrorActionPreference = "Stop"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  throw "cmake was not found in PATH. Install CMake 3.16 or newer first."
}

if (-not $KodiInclude) {
  $candidates = @(
    (Join-Path $PSScriptRoot "kodi-dev-kit/include"),
    (Join-Path (Split-Path $PSScriptRoot -Parent) "kodi-dev-kit/include")
  )
  foreach ($candidate in $candidates) {
    if (Test-Path (Join-Path $candidate "kodi/addon-instance/PVR.h")) {
      $KodiInclude = $candidate
      break
    }
  }
}

if (-not $KodiInclude -or -not (Test-Path (Join-Path $KodiInclude "kodi/addon-instance/PVR.h"))) {
  throw @"
Kodi development headers were not found.

Download the Kodi 21 (Omega) source and use its:
  xbmc/addons/kodi-dev-kit/include

Then run:
  .\build.ps1 -KodiInclude "C:\path\to\kodi-dev-kit\include"
"@
}

$configureArgs = @(
  "-S", $PSScriptRoot,
  "-B", (Join-Path $PSScriptRoot $BuildDir),
  "-DKODI_INCLUDE_DIR=$KodiInclude"
)

if ($IsWindows -or $env:OS -eq "Windows_NT") {
  $configureArgs += @("-A", "x64")
}

& cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
  throw "CMake configure failed."
}

& cmake --build (Join-Path $PSScriptRoot $BuildDir) --config $Config
if ($LASTEXITCODE -ne 0) {
  throw "CMake build failed."
}

$binary = Get-ChildItem (Join-Path $PSScriptRoot $BuildDir) -Recurse -File |
  Where-Object { $_.Name -in @("pvr.cctv.dll", "pvr.cctv.so", "pvr.cctv.dylib") } |
  Select-Object -First 1

if ($binary) {
  Write-Host ""
  Write-Host "Built: $($binary.FullName)"
} else {
  throw "Build finished but no add-on binary was found."
}
