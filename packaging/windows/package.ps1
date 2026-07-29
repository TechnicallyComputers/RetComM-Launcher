# Package RetComM Windows zip from a CMake install prefix + vcpkg bin dir.
#
# Usage:
#   packaging/windows/package.ps1 -Prefix out -Version 0.1.0 -VcpkgBin C:\vcpkg\installed\x64-windows\bin
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

Copy-Item (Join-Path $Prefix "bin\retcomm.exe") $Stage
Copy-Item (Join-Path $Prefix "bin\retcomm-hub.exe") $Stage

$CatalogSrc = Join-Path $Prefix "catalog"
if (-not (Test-Path $CatalogSrc)) {
    $CatalogSrc = Join-Path $Prefix "share\retcomm\catalog"
}
Copy-Item -Recurse $CatalogSrc (Join-Path $Stage "catalog")

if ($VcpkgBin -and (Test-Path $VcpkgBin)) {
    $Dlls = @(
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
        "fmt.dll"
    )
    foreach ($pat in $Dlls) {
        Get-ChildItem -Path $VcpkgBin -Filter $pat -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item $_.FullName $Stage -Force
        }
    }
}

if (Test-Path (Join-Path $Root "assets\retcomm.png")) {
    Copy-Item (Join-Path $Root "assets\retcomm.png") (Join-Path $Stage "retcomm.png")
}

$ZipPath = Join-Path $OutDir $ZipName
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipPath
Write-Host "Zip: $ZipPath"
