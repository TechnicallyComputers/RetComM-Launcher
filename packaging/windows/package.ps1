# Package RetComM Windows zip from a CMake install prefix (+ optional DLL dirs).
#
# Usage:
#   packaging/windows/package.ps1 -Prefix out -Version 0.1.1 [-VcpkgBin path] [-Arch x64]
param(
    [Parameter(Mandatory = $true)][string]$Prefix,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$VcpkgBin = "",
    [string]$Arch = "x64"
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$OutDir = Join-Path $Root "dist"
$Stage = Join-Path $OutDir "windows-stage"
$ZipName = "RetComM-Launcher-$Version-windows-$Arch.zip"

Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$PrefixBin = Join-Path $Prefix "bin"
Copy-Item (Join-Path $PrefixBin "retcomm.exe") $Stage
Copy-Item (Join-Path $PrefixBin "retcomm-hub.exe") $Stage

# Prefer DLLs already installed beside the exes (CMake TARGET_RUNTIME_DLLS).
Get-ChildItem -Path $PrefixBin -Filter "*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName $Stage -Force
}

# Title catalog is fetched on-device; not bundled in the zip.

function Copy-RuntimeDlls([string]$Dir) {
    if (-not $Dir -or -not (Test-Path $Dir)) { return 0 }
    $count = 0
    $patterns = @(
        "SDL3.dll",
        "libcurl.dll",
        "libcurl-d.dll",
        "zlib1.dll",
        "zlibd1.dll",
        "libssl*.dll",
        "libcrypto*.dll",
        "nghttp2.dll",
        "libssh2.dll",
        "brotlicommon.dll",
        "brotlidec.dll",
        "fmt.dll",
        "legacy.dll"
    )
    foreach ($pat in $patterns) {
        Get-ChildItem -Path $Dir -Filter $pat -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item $_.FullName $Stage -Force
            $count++
        }
    }
    # Fallback: copy every DLL from this directory (build/Release applocal tree).
    if ($count -eq 0) {
        Get-ChildItem -Path $Dir -Filter "*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item $_.FullName $Stage -Force
            $count++
        }
    }
    return $count
}

$copied = 0
if ($VcpkgBin) { $copied += Copy-RuntimeDlls $VcpkgBin }

# Auto-discover common locations when -VcpkgBin was wrong/empty.
if ($copied -eq 0 -or -not (Test-Path (Join-Path $Stage "SDL3.dll"))) {
    $searchRoots = @(
        (Join-Path $Root "build\Release"),
        (Join-Path $Root "build\vcpkg_installed\x64-windows\bin"),
        (Join-Path $Root "vcpkg_installed\x64-windows\bin"),
        (Join-Path $Root "retcomm-vcpkg\installed\x64-windows\bin")
    )
    foreach ($dir in $searchRoots) {
        $copied += Copy-RuntimeDlls $dir
    }
    if (-not (Test-Path (Join-Path $Stage "SDL3.dll"))) {
        $hit = Get-ChildItem -Path (Join-Path $Root "build") -Recurse -Filter "SDL3.dll" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($hit) {
            Write-Host "Discovered SDL3.dll at $($hit.DirectoryName)"
            Copy-RuntimeDlls $hit.DirectoryName | Out-Null
        }
    }
}

if (-not (Test-Path (Join-Path $Stage "SDL3.dll"))) {
    Write-Warning "SDL3.dll not bundled — retcomm-hub may fail to start on clean machines"
}
if (-not (Test-Path (Join-Path $Stage "libcurl.dll")) -and
    -not (Get-ChildItem $Stage -Filter "libcurl*.dll" -ErrorAction SilentlyContinue)) {
    Write-Warning "libcurl DLL not bundled — network features may fail on clean machines"
}

if (Test-Path (Join-Path $Root "assets\retcomm.png")) {
    Copy-Item (Join-Path $Root "assets\retcomm.png") (Join-Path $Stage "retcomm.png")
}

$ZipPath = Join-Path $OutDir $ZipName
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipPath
Write-Host "Zip: $ZipPath"
Get-ChildItem $Stage | ForEach-Object { Write-Host ("  staged: " + $_.Name) }
