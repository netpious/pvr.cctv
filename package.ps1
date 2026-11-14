[CmdletBinding()]
param(
  [string]$BuildDir = "build",
  [string]$OutDir = "dist",
  [ValidateSet("auto", "windows", "linux", "android", "macos")]
  [string]$Platform = "auto",
  [string]$Abi = "arm64-v8a"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$out = Join-Path $root $OutDir
$addonDir = Join-Path $out "pvr.cctv"

$binary = Get-ChildItem (Join-Path $root $BuildDir) -Recurse -File -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -in @("pvr.cctv.dll", "pvr.cctv.so", "pvr.cctv.dylib") } |
  Select-Object -First 1

if (-not $binary) {
  throw "No add-on binary found under '$BuildDir'. Run build.ps1 first."
}

if ($Platform -eq "auto") {
  switch ($binary.Extension) {
    ".dll" { $Platform = "windows" }
    ".dylib" { $Platform = "macos" }
    default {
      # A .so can be Linux or Android; the build directory is the only hint
      # we have without inspecting the ELF headers.
      if ((Resolve-Path $binary.DirectoryName).Path -match "android") { $Platform = "android" }
      else { $Platform = "linux" }
    }
  }
}

if ($Platform -eq "android" -and $binary.Extension -ne ".so") {
  throw "Platform 'android' needs a .so, found '$($binary.Name)'."
}

if (Test-Path $addonDir) {
  Remove-Item $addonDir -Recurse -Force
}
New-Item -ItemType Directory -Path $addonDir -Force | Out-Null

Copy-Item (Join-Path $root "addon.xml") $addonDir
if (Test-Path (Join-Path $root "icon.png")) {
  Copy-Item (Join-Path $root "icon.png") $addonDir
}
Copy-Item (Join-Path $root "resources") $addonDir -Recurse
Copy-Item $binary.FullName $addonDir

$archiveName = switch ($Platform) {
  "windows" { "pvr.cctv-win64.zip" }
  "linux" { "pvr.cctv-linux-x86_64.zip" }
  "android" { "pvr.cctv-android-$Abi.zip" }
  "macos" { "pvr.cctv-macos.zip" }
}
$archive = Join-Path $out $archiveName

# Kodi 只认 zip 内用 "/" 分隔的路径。Windows PowerShell 5.1 的 Compress-Archive
# 会把条目写成 "pvr.cctv\addon.xml"（反斜杠），Kodi 里面找不到顶层目录，
# 于是报「无法从 zip 安装加载项」。这里用 .NET ZipArchive 手工写条目，强制 "/"。
# （用 Add-Type 写 C# 而不是在 PowerShell 里 new ZipArchive，避免 PS 5.1 解析
#   ZipArchiveMode 枚举时的 "找不到类型" 问题。）
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipRefs = @(
  [System.IO.Compression.ZipArchive].Assembly.Location,
  [System.IO.Compression.ZipFile].Assembly.Location
)

Add-Type -ReferencedAssemblies $zipRefs -TypeDefinition @"
using System;
using System.IO;
using System.IO.Compression;

public static class KodiZipPack
{
  public static int Create(string sourceDir, string archivePath, string rootName)
  {
    if (File.Exists(archivePath))
      File.Delete(archivePath);

    string[] files = Directory.GetFiles(sourceDir, "*", SearchOption.AllDirectories);
    Array.Sort(files, StringComparer.Ordinal);
    int count = 0;

    using (FileStream fs = File.Open(archivePath, FileMode.CreateNew))
    using (ZipArchive zip = new ZipArchive(fs, ZipArchiveMode.Create))
    {
      foreach (string file in files)
      {
        string rel = file.Substring(sourceDir.Length + 1).Replace('\\', '/');
        ZipArchiveEntry entry = zip.CreateEntry(rootName + "/" + rel, CompressionLevel.Optimal);
        entry.LastWriteTime = File.GetLastWriteTime(file);
        using (Stream es = entry.Open())
        using (FileStream src = File.OpenRead(file))
          src.CopyTo(es);
        count++;
      }
    }
    return count;
  }
}
"@

$entryCount = [KodiZipPack]::Create($addonDir, $archive, "pvr.cctv")

# 打包后自检：不允许出现反斜杠条目，且必须存在 pvr.cctv/addon.xml
Add-Type -AssemblyName System.IO.Compression.FileSystem
$verify = [System.IO.Compression.ZipFile]::OpenRead($archive)
try {
  $names = @($verify.Entries | ForEach-Object { $_.FullName })
}
finally { $verify.Dispose() }

$bad = @($names | Where-Object { $_ -like '*\*' })
if ($bad.Count -gt 0) {
  throw "Zip entries use backslashes: $($bad -join ', ')"
}
if ($names -notcontains "pvr.cctv/addon.xml") {
  throw "Zip is missing pvr.cctv/addon.xml"
}

Write-Host "Packaged: $archive ($entryCount entries)"
