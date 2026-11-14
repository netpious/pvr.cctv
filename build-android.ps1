# Builds the Android (arm64-v8a) PVR add-on with the Android NDK and packages
# it into a Kodi-installable zip.
#
# Prerequisites:
#   * Android NDK r26 or newer (r27c was used for the first build) - either set
#     ANDROID_NDK_HOME / ANDROID_NDK_ROOT, pass -Ndk, or install it under
#     %LOCALAPPDATA%\Android\Sdk\ndk\<version>.
#   * Kodi 21 (Omega) kodi-dev-kit headers - pass -KodiInclude or set
#     KODI_INCLUDE_DIR.
#   * Ninja (ships with Visual Studio under Common7\IDE\CommonExtensions\
#     Microsoft\CMake\Ninja\ninja.exe) - CMake needs it for the NDK toolchain.
#
# Example:
#   .\build-android.ps1 -KodiInclude "C:\kodi-dev-kit\include"

[CmdletBinding()]
param(
  [string]$Abi = "arm64-v8a",
  [string]$ApiLevel = "21",
  [string]$Ndk = "",
  [string]$KodiInclude = "",
  [string]$BuildDir = "build-android",
  [switch]$SkipPackage
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

function Find-Ndk {
  param([string]$Explicit)

  $candidates = @()
  if ($Explicit) { $candidates += $Explicit }
  if ($env:ANDROID_NDK_HOME) { $candidates += $env:ANDROID_NDK_HOME }
  if ($env:ANDROID_NDK_ROOT) { $candidates += $env:ANDROID_NDK_ROOT }
  if ($env:LOCALAPPDATA) {
    $ndkRoot = Join-Path $env:LOCALAPPDATA "Android\Sdk\ndk"
    if (Test-Path $ndkRoot) {
      $candidates += (Get-ChildItem $ndkRoot -Directory |
        Sort-Object Name -Descending | ForEach-Object { $_.FullName })
    }
  }

  foreach ($candidate in $candidates) {
    if ($candidate -and (Test-Path (Join-Path $candidate "build\cmake\android.toolchain.cmake"))) {
      return (Resolve-Path $candidate).Path
    }
  }
  return $null
}

function Find-Ninja {
  $ninja = Get-Command ninja -ErrorAction SilentlyContinue
  if ($ninja) { return $ninja.Source }

  $programFiles = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }
  $editions = @("BuildTools", "Community", "Professional", "Enterprise")
  foreach ($base in $programFiles) {
    foreach ($edition in $editions) {
      $candidate = Join-Path $base "Microsoft Visual Studio\2022\$edition\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
      if (Test-Path $candidate) { return $candidate }
    }
  }
  return $null
}

$ndk = Find-Ndk $Ndk
if (-not $ndk) {
  throw "Android NDK not found. Set ANDROID_NDK_HOME, pass -Ndk, or install it with sdkmanager ('ndk;27.2.12479018')."
}

$ninja = Find-Ninja
if (-not $ninja) {
  throw "Ninja not found. Install ninja or use the copy shipped with Visual Studio."
}

if (-not $KodiInclude) {
  $KodiInclude = $env:KODI_INCLUDE_DIR
}
if (-not $KodiInclude) {
  throw "Kodi headers not found. Pass -KodiInclude or set KODI_INCLUDE_DIR (the folder containing 'kodi/addon-instance/PVR.h')."
}

Write-Host "NDK:     $ndk"
Write-Host "Ninja:   $ninja"
Write-Host "Kodi:    $KodiInclude"
Write-Host "ABI:     $Abi (API $ApiLevel)"

Push-Location $root
try {
  # Build the argument list explicitly: PowerShell 5.1 mangles quoted
  # -D<name>=<value> arguments when they are written inline.
  $cmakeConfigure = @(
    "-S", ".",
    "-B", $BuildDir,
    "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake",
    "-DANDROID_NDK=$ndk",
    "-DANDROID_ABI=$Abi",
    "-DANDROID_PLATFORM=android-$ApiLevel",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DKODI_INCLUDE_DIR=$KodiInclude"
  )
  & cmake @cmakeConfigure
  if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

  & cmake --build $BuildDir
  if ($LASTEXITCODE -ne 0) { throw "Build failed" }

  $so = Join-Path $root "$BuildDir\addon\pvr.cctv.so"
  if (-not (Test-Path $so)) { throw "Build finished but $so is missing" }

  # The add-on is loaded by Kodi's process; debug symbols and DWARF sections
  # only waste space on the device.
  $strip = Join-Path $ndk "toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
  if (Test-Path $strip) {
    & $strip --strip-unneeded $so
    if ($LASTEXITCODE -ne 0) { throw "llvm-strip failed" }
  }

  Write-Host ("Add-on: {0} ({1:N0} bytes)" -f $so, (Get-Item $so).Length)

  if (-not $SkipPackage) {
    & (Join-Path $root "package.ps1") -BuildDir $BuildDir -Platform android -Abi $Abi
  }
}
finally {
  Pop-Location
}
