# Package RetComM Windows portable single-exe and Inno Setup installer.
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
$PortableName = "RetComM-Launcher-$Version-windows-portable.exe"

Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$PrefixBin = Join-Path $Prefix "bin"
Copy-Item (Join-Path $PrefixBin "retcomm.exe") $Stage
Copy-Item (Join-Path $PrefixBin "retcomm-hub.exe") $Stage

# Prefer DLLs already installed beside the exes (CMake TARGET_RUNTIME_DLLS).
# Note: TARGET_RUNTIME_DLLS often misses *transitive* deps (e.g. zlib behind
# libcurl), so we always harvest from vcpkg installed/bin as well.
Get-ChildItem -Path $PrefixBin -Filter "*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName $Stage -Force
}

function Copy-RuntimeDlls([string]$Dir, [switch]$AllDlls) {
    if (-not $Dir -or -not (Test-Path $Dir)) { return 0 }
    $count = 0
    if ($AllDlls) {
        Get-ChildItem -Path $Dir -Filter "*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item $_.FullName $Stage -Force
            $count++
        }
        return $count
    }
    $patterns = @(
        "SDL3.dll",
        "libcurl.dll",
        "libcurl-d.dll",
        "zlib1.dll",
        "zlibd1.dll",
        "zlib.dll",
        "z.dll",
        "libssl*.dll",
        "libcrypto*.dll",
        "libssl-*.dll",
        "libcrypto-*.dll",
        "nghttp2.dll",
        "libssh2.dll",
        "brotlicommon.dll",
        "brotlidec.dll",
        "brotlienc.dll",
        "fmt.dll",
        "legacy.dll",
        "libomp*.dll"
    )
    foreach ($pat in $patterns) {
        Get-ChildItem -Path $Dir -Filter $pat -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item $_.FullName $Stage -Force
            $count++
        }
    }
    return $count
}

function Test-StagedDll([string[]]$Names) {
    foreach ($n in $Names) {
        if (Test-Path (Join-Path $Stage $n)) { return $true }
        if (Get-ChildItem $Stage -Filter $n -ErrorAction SilentlyContinue) { return $true }
    }
    return $false
}

# Always merge from every known location. Skipping the vcpkg installed tree when
# build/Release already has SDL3 was leaving zlib (curl transitive) out of the
# installer — clean machines then fail with "zlib1.dll / z.dll was not found".
$searchRoots = [System.Collections.Generic.List[string]]::new()
if ($VcpkgBin) { [void]$searchRoots.Add($VcpkgBin) }
@(
    (Join-Path $Root "build\Release"),
    (Join-Path $Root "build\RelWithDebInfo"),
    (Join-Path $Root "vcpkg_installed\x64-windows\bin"),
    (Join-Path $Root "retcomm-vcpkg\installed\x64-windows\bin"),
    (Join-Path $Root "build\vcpkg_installed\x64-windows\bin"),
    (Join-Path $env:RETCOMM_VCPKG "installed\x64-windows\bin")
) | ForEach-Object {
    if ($_ -and (Test-Path $_)) { [void]$searchRoots.Add($_) }
}

$copied = 0
foreach ($dir in ($searchRoots | Select-Object -Unique)) {
    # Full harvest from vcpkg package bins (small; covers curl→zlib→…).
    $isVcpkgPkgBin = ($dir -match '[/\\]installed[/\\]x64-windows[/\\]bin$')
    $n = Copy-RuntimeDlls $dir -AllDlls:$isVcpkgPkgBin
    if ($n -gt 0) {
        Write-Host "Harvested $n DLL(s) from $dir"
        $copied += $n
    }
}

if (-not (Test-StagedDll @("SDL3.dll"))) {
    $hit = Get-ChildItem -Path (Join-Path $Root "build") -Recurse -Filter "SDL3.dll" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($hit) {
        Write-Host "Discovered SDL3.dll at $($hit.DirectoryName)"
        Copy-RuntimeDlls $hit.DirectoryName | Out-Null
        Copy-RuntimeDlls $hit.DirectoryName -AllDlls | Out-Null
    }
}

# MSVC/vcpkg ships zlib1.dll; some MinGW layouts use z.dll / zlib.dll.
$hasZlib = Test-StagedDll @("zlib1.dll", "zlibd1.dll", "z.dll", "zlib.dll")
$hasCurl = Test-StagedDll @("libcurl.dll", "libcurl-d.dll", "libcurl*.dll")
$hasSdl = Test-StagedDll @("SDL3.dll")

if (-not $hasSdl) {
    throw "SDL3.dll not bundled — cannot ship a Windows release without it"
}
if (-not $hasCurl) {
    throw "libcurl DLL not bundled — cannot ship a Windows release without it"
}
if (-not $hasZlib) {
    throw @"
zlib runtime DLL not bundled (need zlib1.dll from vcpkg, or z.dll/zlib.dll).
libcurl depends on zlib; clean machines fail with 'z.dll / zlib1.dll was not found'.
Searched: $($searchRoots -join '; ')
Staged so far: $((Get-ChildItem $Stage -Filter '*.dll' | ForEach-Object Name) -join ', ')
"@
}

# If an import expects z.dll but we only have zlib1.dll, provide an alias copy.
# (Harmless when unused; unblocks mismatched import names.)
$zlib1 = Join-Path $Stage "zlib1.dll"
$zDll = Join-Path $Stage "z.dll"
if ((Test-Path $zlib1) -and -not (Test-Path $zDll)) {
    Copy-Item $zlib1 $zDll -Force
    Write-Host "Aliased zlib1.dll -> z.dll for import-name compatibility"
}
if ((Test-Path $zDll) -and -not (Test-Path $zlib1)) {
    Copy-Item $zDll $zlib1 -Force
    Write-Host "Aliased z.dll -> zlib1.dll for import-name compatibility"
}

# Installer channel marker (self-update picks the windows-*-setup.exe asset).
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
# Hub UI fonts next to the exes (fonts/LatoLatin-*.ttf).
$FontsSrc = Join-Path $Prefix "share\retcomm\fonts"
if (-not (Test-Path (Join-Path $FontsSrc "LatoLatin-Regular.ttf"))) {
    $FontsSrc = Join-Path $Root "assets\fonts"
}
$FontsRegular = Join-Path $FontsSrc "LatoLatin-Regular.ttf"
if (-not (Test-Path $FontsRegular)) {
    throw "Hub fonts missing (expected LatoLatin-Regular.ttf under share/retcomm/fonts or assets/fonts)"
}
$FontsDst = Join-Path $Stage "fonts"
New-Item -ItemType Directory -Force -Path $FontsDst | Out-Null
Copy-Item (Join-Path $FontsSrc "*") $FontsDst -Force
if (-not (Test-Path (Join-Path $FontsDst "LatoLatin-Regular.ttf"))) {
    throw "Failed to stage hub fonts into $FontsDst"
}

# --- Portable single-exe (stub + appended zip trailer; zip is temporary only) ---
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
