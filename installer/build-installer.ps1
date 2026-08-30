<#
.SYNOPSIS
    Builds the Rhythm install wizard (RhythmSetup-x.y.z.exe) end to end.

.DESCRIPTION
    Configures a Release build, compiles Rhythm.exe + RhythmEditor.exe, stages
    the exact install tree via `cmake --install`, then compiles
    installer/Rhythm.iss with Inno Setup's command-line compiler (ISCC).

    Run from anywhere; paths are resolved relative to this script.

.PARAMETER BuildDir
    CMake build directory. Default: <repo>/build

.PARAMETER Iscc
    Full path to ISCC.exe. Auto-detected from the usual install locations and
    PATH if omitted.

.PARAMETER SkipBuild
    Reuse whatever is already in BuildDir; only stage and run ISCC.

.EXAMPLE
    ./installer/build-installer.ps1
#>
[CmdletBinding()]
param(
    [string]$BuildDir,
    [string]$Iscc,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'build' }
$stageDir  = Join-Path $BuildDir 'stage'
$outputDir = Join-Path $BuildDir 'installer'

function Resolve-Iscc {
    param([string]$Explicit)
    if ($Explicit) {
        if (Test-Path $Explicit) { return $Explicit }
        throw "ISCC not found at: $Explicit"
    }
    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    $onPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw "Inno Setup 6 not found. Install it (https://jrsoftware.org/isdl.php) or pass -Iscc <path to ISCC.exe>."
}

$isccExe = Resolve-Iscc $Iscc
Write-Host "Using Inno Setup compiler: $isccExe"

# Read the version straight from CMakeLists.txt so it has a single source.
$cmakeText = Get-Content (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(\s*Rhythm\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not read project version from CMakeLists.txt"
}
$version = $Matches[1]
Write-Host "Rhythm version: $version"

if (-not $SkipBuild) {
    Write-Host "`n== Configuring (Release) =="
    cmake -S $repoRoot -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE) { throw "cmake configure failed" }

    Write-Host "`n== Building =="
    cmake --build $BuildDir --target Rhythm RhythmEditor
    if ($LASTEXITCODE) { throw "build failed" }
}

Write-Host "`n== Staging install tree -> $stageDir =="
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
cmake --install $BuildDir --prefix $stageDir
if ($LASTEXITCODE) { throw "cmake --install failed" }

Write-Host "`n== Compiling installer =="
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
& $isccExe `
    "/DAppVersion=$version" `
    "/DStageDir=$stageDir" `
    "/DOutputDir=$outputDir" `
    (Join-Path $PSScriptRoot 'Rhythm.iss')
if ($LASTEXITCODE) { throw "ISCC failed" }

$setup = Join-Path $outputDir "RhythmSetup-$version.exe"
Write-Host "`nDone: $setup" -ForegroundColor Green
