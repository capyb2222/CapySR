param(
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$Test
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build"

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

# cmake needs cl.exe on PATH, so pull in the MSVC environment once.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - install Visual Studio with the C++ workload" }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw "no Visual Studio install with the C++ toolset" }

    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vsPath" }

    cmd /c "`"$vcvars`" && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "vcvars64 did not put cl.exe on PATH" }
}

New-Item -ItemType Directory -Force -Path $build | Out-Null
cmake -S "$root" -B "$build" -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

cmake --build $build --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

if ($Test) {
    & (Join-Path $build "bin\capysr_tests.exe")
    if ($LASTEXITCODE -ne 0) { throw "tests failed" }
}

Write-Host "built $build\bin\capysr.exe" -ForegroundColor Green
