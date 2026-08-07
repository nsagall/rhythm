# Builds and runs RhythmEditor.exe (the chart editor) in Debug configuration.
$ErrorActionPreference = "Stop"
$repoRoot = $PSScriptRoot
$buildDir = Join-Path $repoRoot "build/debug"

cmake -S $repoRoot -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& (Join-Path $buildDir "RhythmEditor.exe")
