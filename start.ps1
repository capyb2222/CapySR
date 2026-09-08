#requires -version 5
param(
    [switch]$NoClient,   # servers only
    [switch]$NoBuild     # skip the build step
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

function Say($text, $colour = 'Gray') { Write-Host $text -ForegroundColor $colour }

Say ''
Say '  CapySR' Cyan
Say '  ------' Cyan

# a leftover server holds the ports and the new one dies on bind
$stale = Get-Process CapySR.SdkServer, CapySR.GameServer -ErrorAction SilentlyContinue
if ($stale) {
    Say "  stopping $($stale.Count) running server(s)..." DarkGray
    $stale | Stop-Process -Force
    Start-Sleep -Milliseconds 800
}

if (-not $NoBuild) {
    Say '  building...' DarkGray
    $build = & dotnet build "$root\CapySR.slnx" -c Release --nologo -v quiet 2>&1
    if ($LASTEXITCODE -ne 0) {
        Say '  BUILD FAILED' Red
        $build | Select-Object -Last 25 | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
}

New-Item -ItemType Directory -Force -Path "$root\logs" | Out-Null
Remove-Item "$root\logs\sdk.log", "$root\logs\game.log" -ErrorAction SilentlyContinue

Say '  starting sdk server  (dispatch, port 21000)' DarkGray
Start-Process dotnet -ArgumentList "run --project `"$root\src\CapySR.SdkServer`" -c Release --no-build" `
    -WorkingDirectory $root -WindowStyle Minimized

Say '  starting game server (kcp, port 23301)' DarkGray
Start-Process dotnet -ArgumentList "run --project `"$root\src\CapySR.GameServer`" -c Release --no-build" `
    -WorkingDirectory $root -WindowStyle Minimized

function Wait-ForLog($file, $needle, $seconds = 90) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $file) {
            if (Select-String -Path $file -Pattern $needle -Quiet -ErrorAction SilentlyContinue) { return $true }
        }
        Start-Sleep -Milliseconds 400
    }
    return $false
}

$sdkOk  = Wait-ForLog "$root\logs\sdk.log"  'sdkserver listening'
$gameOk = Wait-ForLog "$root\logs\game.log" 'kcp gateway listening'

Say ''
if ($sdkOk)  { Say '  [ok]   sdk server  ready' Green }  else { Say '  [FAIL] sdk server  - see logs\sdk.log' Red }
if ($gameOk) { Say '  [ok]   game server ready' Green } else { Say '  [FAIL] game server - see logs\game.log' Red }

if (-not ($sdkOk -and $gameOk)) {
    Say ''
    Say '  servers did not come up. last lines:' Yellow
    foreach ($f in @("$root\logs\sdk.log", "$root\logs\game.log")) {
        if (Test-Path $f) { Say "  -- $f" DarkGray; Get-Content $f -Tail 8 | ForEach-Object { Write-Host "     $_" } }
    }
    exit 1
}

foreach ($line in (Select-String -Path "$root\logs\game.log" -Pattern 'game data loaded|srtools data').Line) {
    Say "         $($line -replace '^\S+\s+\S+\s+\S+:\s*', '')" DarkGray
}

if ($NoClient) { Say ''; Say '  servers up. -NoClient given, not launching the game.' Cyan; exit 0 }

# --- client -----------------------------------------------------------------

$config = Get-Content "$root\config\config.json" -Raw | ConvertFrom-Json
$clientDir = $config.Client.Path

if (-not $clientDir -or -not (Test-Path $clientDir)) {
    Say ''
    Say '  no client configured. set Client.Path in config\config.json to the folder' Yellow
    Say '  holding StarRail.exe, then run this again.' Yellow
    exit 0
}

$launcher = Join-Path $clientDir 'launcher.exe'
$dll      = Join-Path $clientDir 'hkrpg.dll'
$patch    = Join-Path $root 'pearl-sr\launcher'

# keep the injector in step with the copy in pearl-sr
foreach ($file in 'launcher.exe', 'hkrpg.dll') {
    $src = Join-Path $patch $file
    $dst = Join-Path $clientDir $file
    if ((Test-Path $src) -and (-not (Test-Path $dst) -or
        (Get-Item $src).LastWriteTime -gt (Get-Item $dst).LastWriteTime)) {
        Say "  updating $file in the client folder" DarkGray
        Copy-Item $src $dst -Force
    }
}

if (-not (Test-Path $launcher) -or -not (Test-Path $dll)) {
    Say ''
    Say "  launcher.exe / hkrpg.dll missing from $clientDir" Red
    Say "  copy them from $patch" Yellow
    exit 1
}

Say ''
Say '  launching the client (accept the admin prompt)...' Cyan
Start-Process -FilePath $launcher -WorkingDirectory $clientDir -Verb RunAs

Say ''
Say '  logs\sdk.log and logs\game.log are live if anything goes wrong.' DarkGray
Say ''
