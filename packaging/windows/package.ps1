# Package RetComM Windows zip, portable single-exe, and Inno Setup installer.
#
# Usage:
#   packaging/windows/package.ps1 -Prefix out -Version 0.1.1 [-VcpkgBin path] [-Arch x64] [-PortableStub path]
param(
    [Parameter(Mandatory = $true)][string]$Prefix,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$VcpkgBin = "",
    [string]$Arch = "x64",
    [string]$PortableStub = "",
    [string]$InnoSetup = ""
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$OutDir = Join-Path $Root "dist"
$Stage = Join-Path $OutDir "windows-stage"
$ZipName = "RetComM-Launcher-$Version-windows-$Arch.zip"
$PortableName = "RetComM-Launcher-$Version-windows-portable.exe"

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

if ($copied -eq 0 -or -not (Test-Path (Join-Path $Stage "SDL3.dll"))) {
    $searchRoots = @(
        (Join-Path $Root "build\Release"),
        (Join-Path $Root "vcpkg_installed\x64-windows\bin"),
        (Join-Path $Root "retcomm-vcpkg\installed\x64-windows\bin"),
        (Join-Path $Root "build\vcpkg_installed\x64-windows\bin")
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

# Installer / zip channel marker (self-update picks the windows-x64.zip asset).
@'
{
  "schema_version": 1,
  "channel": "installer"
}
'@ | Set-Content -Path (Join-Path $Stage "channel.json") -Encoding utf8

$IcoSrc = Join-Path $Root "assets\retcomm.ico"
if (Test-Path $IcoSrc) {
    Copy-Item $IcoSrc (Join-Path $Stage "retcomm.ico") -Force
}
if (Test-Path (Join-Path $Root "assets\retcomm.png")) {
    Copy-Item (Join-Path $Root "assets\retcomm.png") (Join-Path $Stage "retcomm.png") -Force
}

# --- Zip (installer update payload + legacy portable folder) ---
$ZipPath = Join-Path $OutDir $ZipName
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipPath
Write-Host "Zip: $ZipPath"

# --- Portable single-exe (stub + appended zip trailer) ---
if (-not $PortableStub) {
    $candidates = @(
        (Join-Path $PrefixBin "retcomm-portable.exe"),
        (Join-Path $Root "build\Release\retcomm-portable.exe"),
        (Join-Path $Root "build\retcomm-portable.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $PortableStub = $c; break }
    }
}
if (-not $PortableStub -or -not (Test-Path $PortableStub)) {
    Write-Warning "retcomm-portable.exe not found — skipping windows-portable.exe"
} else {
    $PayloadZip = Join-Path $OutDir "windows-portable-payload.zip"
    if (Test-Path $PayloadZip) { Remove-Item $PayloadZip -Force }
    # Payload should not include the installer channel marker as authoritative;
    # the stub writes channel.json on extract. Still fine if present.
    Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $PayloadZip

    $OutPortable = Join-Path $OutDir $PortableName
    if (Test-Path $OutPortable) { Remove-Item $OutPortable -Force }

    $stubBytes = [System.IO.File]::ReadAllBytes($PortableStub)
    $zipBytes = [System.IO.File]::ReadAllBytes($PayloadZip)
    $fs = [System.IO.File]::Open($OutPortable, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    try {
        $fs.Write($stubBytes, 0, $stubBytes.Length)
        $fs.Write($zipBytes, 0, $zipBytes.Length)
        $len = [BitConverter]::GetBytes([uint64]$zipBytes.Length)
        $fs.Write($len, 0, 8)
        $magic = [Text.Encoding]::ASCII.GetBytes("RCM1")
        $fs.Write($magic, 0, 4)
    } finally {
        $fs.Close()
    }
    Remove-Item $PayloadZip -Force -ErrorAction SilentlyContinue
    Write-Host "Portable: $OutPortable (stub $($stubBytes.Length) + payload $($zipBytes.Length))"
}

# --- Inno Setup installer ---
function Find-ISCC([string]$Hint) {
    if ($Hint -and (Test-Path $Hint)) { return (Resolve-Path $Hint).Path }
    $cmd = Get-Command iscc -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $paths = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "${env:LocalAppData}\Programs\Inno Setup 6\ISCC.exe"
    )
    foreach ($p in $paths) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

$iscc = Find-ISCC $InnoSetup
if (-not $iscc) {
    Write-Warning "Inno Setup (ISCC.exe) not found — skipping windows setup.exe"
} elseif (-not (Test-Path (Join-Path $Stage "retcomm.ico"))) {
    Write-Warning "assets/retcomm.ico missing from stage — skipping windows setup.exe"
} else {
    $iss = Join-Path $PSScriptRoot "setup.iss"
    $stageAbs = (Resolve-Path $Stage).Path
    $outAbs = (Resolve-Path $OutDir).Path
    & $iscc `
        "/DMyAppVersion=$Version" `
        "/DStageDir=$stageAbs" `
        "/DOutputDir=$outAbs" `
        "/DArch=$Arch" `
        $iss
    if ($LASTEXITCODE -ne 0) {
        throw "ISCC failed with exit code $LASTEXITCODE"
    }
    Write-Host "Installer: (see OutputBaseFilename under $outAbs)"
}

Get-ChildItem $Stage | ForEach-Object { Write-Host ("  staged: " + $_.Name) }
Get-ChildItem $OutDir -File | ForEach-Object { Write-Host ("  dist: " + $_.Name) }
