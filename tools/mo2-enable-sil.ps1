# Enables the "Simple Immersive Looting" (Chinese) mod in the MO2 Default profile.
# Mirrors the logic of build-sas.ps1 [5/5] so both stay consistent:
#   modlist.txt   -> insert "+<mod>" right before the first mod line (top = highest priority)
#   plugins.txt   -> append "*<plugin>"
#   loadorder.txt -> append "<plugin>"
# Backups are written once as *.bak-sil.
param(
    [string]$ProfileDir = 'D:\Mod Organizer 2\starfield_mods\profiles\Default',
    [string]$ModName    = 'Simple Immersive Looting',
    [string]$PluginName = 'SimpleImmersiveLooting.esm'
)

$ErrorActionPreference = 'Stop'

foreach ($f in @('modlist.txt', 'plugins.txt', 'loadorder.txt')) {
    $p = Join-Path $ProfileDir $f
    if (-not (Test-Path -LiteralPath $p)) { throw "missing: $p" }
    if (-not (Test-Path -LiteralPath "$p.bak-sil")) {
        Copy-Item -LiteralPath $p -Destination "$p.bak-sil" -Force
        Write-Host "backup: $p.bak-sil"
    }
}

# --- modlist.txt ---
$modlistPath = Join-Path $ProfileDir 'modlist.txt'
$lines = @(Get-Content -LiteralPath $modlistPath)
$lines = $lines | Where-Object { $_ -ne "+$ModName" -and $_ -ne "-$ModName" }
$firstMod = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^[+\-*]') { $firstMod = $i; break }
}
if ($firstMod -lt 0) { $lines += "+$ModName" }
else { $lines = $lines[0..($firstMod - 1)] + @("+$ModName") + $lines[$firstMod..($lines.Count - 1)] }
Set-Content -LiteralPath $modlistPath -Value $lines -Encoding UTF8
Write-Host "modlist: +$ModName"

# --- plugins.txt / loadorder.txt ---
foreach ($file in @('plugins.txt', 'loadorder.txt')) {
    $path = Join-Path $ProfileDir $file
    $pl = @(Get-Content -LiteralPath $path)
    $pl = $pl | Where-Object { $_ -ne "*$PluginName" -and $_ -ne $PluginName }
    if ($file -eq 'plugins.txt') { $pl += "*$PluginName" } else { $pl += $PluginName }
    Set-Content -LiteralPath $path -Value $pl -Encoding UTF8
    Write-Host "$file : $PluginName"
}

Write-Host 'NOTE: if Mod Organizer 2 is running, restart it (or it will overwrite these files).' -ForegroundColor Yellow
