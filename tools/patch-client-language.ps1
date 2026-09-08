<#
    English-patches a CN Star Rail client.

    The language table lives in a .bytes file under
    StarRail_Data/StreamingAssets/DesignData/Windows. It looks like:

        cn..Chinese(PRC)  en..English  jp..Japanese  kr..Korean
        os  en kr jp en   cn  cn cn        <- voice block
        os  cn en kr jp en  cn  cn cn      <- text block

    Each entry is a length byte (0x02) followed by two ASCII chars, so cn -> en
    is one byte per entry. The first "cn cn" pair is voice, the second is text.

    Every change is backed up next to the original as *.capysr-backup.
    Run with -Revert to put the originals back.
#>
param(
    [string]$ClientPath,
    [ValidateSet('en', 'cn', 'jp', 'kr')] [string]$Text = 'en',
    [ValidateSet('en', 'cn', 'jp', 'kr')] [string]$Voice = '',
    [switch]$Revert
)

$ErrorActionPreference = 'Stop'

if (-not $ClientPath) {
    $configPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'config\config.json'
    if (Test-Path $configPath) {
        $ClientPath = (Get-Content $configPath -Raw | ConvertFrom-Json).Client.Path
    }
}

if (-not $ClientPath -or -not (Test-Path $ClientPath)) {
    Write-Host "client folder not found. pass -ClientPath <folder with StarRail.exe>" -ForegroundColor Red
    exit 1
}

$dir = Join-Path $ClientPath 'StarRail_Data\StreamingAssets\DesignData\Windows'
if (-not (Test-Path $dir)) {
    Write-Host "no DesignData at $dir" -ForegroundColor Red
    exit 1
}

if ($Revert) {
    $backups = Get-ChildItem $dir -Filter '*.capysr-backup' -ErrorAction SilentlyContinue
    if (-not $backups) { Write-Host 'nothing to revert' -ForegroundColor Yellow; exit 0 }

    foreach ($b in $backups) {
        $target = $b.FullName -replace '\.capysr-backup$', ''
        Copy-Item $b.FullName $target -Force
        Remove-Item $b.FullName -Force
        Write-Host "  reverted $(Split-Path $target -Leaf)" -ForegroundColor Green
    }
    exit 0
}

$patched = 0

foreach ($file in Get-ChildItem $dir -Filter '*.bytes') {
    $data = [System.IO.File]::ReadAllBytes($file.FullName)

    # the table sits just past the literal "Korean"
    $marker = [System.Text.Encoding]::ASCII.GetBytes('Korean')
    $at = -1
    for ($i = 0; $i -le $data.Length - $marker.Length; $i++) {
        $hit = $true
        for ($j = 0; $j -lt $marker.Length; $j++) {
            if ($data[$i + $j] -ne $marker[$j]) { $hit = $false; break }
        }
        if ($hit) { $at = $i; break }
    }
    if ($at -lt 0) { continue }

    # collect the "02 'c' 'n'" entries that follow, in order
    $entries = @()
    for ($i = $at + 6; $i -lt [Math]::Min($at + 140, $data.Length - 3); $i++) {
        if ($data[$i] -eq 0x02 -and $data[$i + 1] -eq 0x63 -and $data[$i + 2] -eq 0x6E) {
            $entries += ($i + 1)
        }
    }

    # voice block then text block; the last two entries of each are the pair
    if ($entries.Count -lt 6) { continue }

    Write-Host "  $($file.Name): $($entries.Count) cn entries at $($entries -join ', ')" -ForegroundColor DarkGray

    $backup = "$($file.FullName).capysr-backup"
    if (-not (Test-Path $backup)) { Copy-Item $file.FullName $backup }

    function Set-Code($offset, $code) {
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($code)
        $data[$offset] = $bytes[0]
        $data[$offset + 1] = $bytes[1]
    }

    # each block is: standalone cn, then the "cn cn" pair.
    # block 1 pair = entries 1,2 (voice); block 2 pair = entries 5,6 (text).
    if ($Voice) {
        Set-Code $entries[1] $Voice
        Set-Code $entries[2] $Voice
    }

    Set-Code $entries[5] $Text
    Set-Code $entries[6] $Text

    [System.IO.File]::WriteAllBytes($file.FullName, $data)
    $patched++
    Write-Host "    text -> $Text$(if ($Voice) { ", voice -> $Voice" })" -ForegroundColor Green
}

if ($patched -eq 0) {
    Write-Host 'no language table found - the client layout may have changed' -ForegroundColor Yellow
    exit 1
}

Write-Host ''
Write-Host "patched $patched file(s). backups are *.capysr-backup" -ForegroundColor Cyan
Write-Host 'revert with:  tools\patch-client-language.ps1 -Revert' -ForegroundColor DarkGray
