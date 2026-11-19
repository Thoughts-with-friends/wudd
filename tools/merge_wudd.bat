@echo off
rem ---------------------------------------------------------------------------
rem merge_wudd.bat - joins a WUDD dump that was split over multiple runs.
rem
rem Usage:
rem   1. Put this file into a folder, e.g. D:\WiiU\
rem   2. After every run, move the "wudump" folder from the USB drive into its
rem      own sub folder (names do not matter), e.g.
rem         D:\WiiU\1\wudump\...
rem         D:\WiiU\2\wudump\...
rem      Keep the "dumpling" folder on the USB drive until the last run is done.
rem   3. After the last run, double click this file.
rem
rem Result: D:\WiiU\merged\wudump\...  (*.app.part01, part02, ... are joined
rem into one *.app, all other files are moved there as well).
rem The run folders are emptied (files are moved, parts deleted after joining).
rem Optional: pass another folder as first argument.
rem ---------------------------------------------------------------------------
setlocal
set "WUDD_ROOT=%~dp0"
if not "%~1"=="" set "WUDD_ROOT=%~1"
set "WUDD_SELF=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s = Get-Content -LiteralPath $env:WUDD_SELF -Raw; $i = $s.IndexOf('#PS' + 'START'); Invoke-Expression $s.Substring($i)"
echo.
pause
exit /b

#PSSTART
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($env:WUDD_ROOT).TrimEnd('\')
$mergedRoot = Join-Path $root 'merged'
$out = Join-Path $mergedRoot 'wudump'

$sources = @(Get-ChildItem -LiteralPath $root -Directory -Recurse |
    Where-Object { $_.Name -eq 'wudump' -and -not $_.FullName.StartsWith($mergedRoot, [StringComparison]::OrdinalIgnoreCase) })
if ($sources.Count -eq 0) {
    Write-Host "No 'wudump' folder found below $root"
    return
}
Write-Host "Found $($sources.Count) run folder(s):"
$sources | ForEach-Object { Write-Host "  $($_.FullName)" }
Write-Host ""

$parts = @{}
$moved = 0
foreach ($src in $sources) {
    foreach ($file in @(Get-ChildItem -LiteralPath $src.FullName -File -Recurse)) {
        $rel = $file.FullName.Substring($src.FullName.Length).TrimStart('\')
        if ($rel -match '^(.+\.app)\.part(\d+)$') {
            $key = $Matches[1]
            if (-not $parts.ContainsKey($key)) { $parts[$key] = New-Object System.Collections.ArrayList }
            [void]$parts[$key].Add([pscustomobject]@{ N = [int]$Matches[2]; Path = $file.FullName })
        } else {
            $dst = Join-Path $out $rel
            New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
            Move-Item -LiteralPath $file.FullName -Destination $dst -Force
            $moved++
        }
    }
}
Write-Host "Moved $moved file(s)."

$errors = 0
foreach ($key in $parts.Keys) {
    $list = @($parts[$key] | Sort-Object N)
    $ok = $true
    for ($i = 0; $i -lt $list.Count; $i++) {
        if ($list[$i].N -ne $i + 1) { $ok = $false }
    }
    if (-not $ok) {
        Write-Host "ERROR: $key - parts are missing or duplicated: $(($list | ForEach-Object { $_.N }) -join ', ')"
        Write-Host "       Did you finish all runs? Nothing was changed for this file."
        $errors++
        continue
    }
    $dst = Join-Path $out $key
    $tmp = "$dst.tmp"
    New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
    Write-Host "Joining $key ($($list.Count) parts)..."
    $o = [IO.File]::Create($tmp)
    try {
        foreach ($p in $list) {
            $in = [IO.File]::OpenRead($p.Path)
            try { $in.CopyTo($o, 16MB) } finally { $in.Close() }
        }
    } finally { $o.Close() }
    Move-Item -LiteralPath $tmp -Destination $dst -Force
    foreach ($p in $list) { Remove-Item -LiteralPath $p.Path }
}

# remove the now empty run folders
foreach ($src in $sources) {
    Get-ChildItem -LiteralPath $src.FullName -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending |
        Where-Object { @(Get-ChildItem -LiteralPath $_.FullName -Force).Count -eq 0 } | Remove-Item
    if (@(Get-ChildItem -LiteralPath $src.FullName -Force).Count -eq 0) { Remove-Item -LiteralPath $src.FullName }
}

Write-Host ""
if ($errors -eq 0) {
    Write-Host "Done. Result: $out"
} else {
    Write-Host "Finished with $errors error(s). Result so far: $out"
}
