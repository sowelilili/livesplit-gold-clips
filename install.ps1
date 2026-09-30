# Installs build\livesplit-gold-clips.dll into OBS's per-machine plugin folder
# (C:\ProgramData\obs-studio\plugins, used by OBS 28 and later). Close OBS first.
# If Windows denies access, run this from an elevated PowerShell.
$ErrorActionPreference = 'Stop'

$dll = Join-Path $PSScriptRoot 'build\livesplit-gold-clips.dll'
if (-not (Test-Path $dll)) {
    throw "Plugin not built yet: $dll is missing. Run ./build.sh in Git Bash first."
}
if (Get-Process -Name 'obs64' -ErrorAction SilentlyContinue) {
    throw 'OBS is running. Close OBS, then run this script again.'
}

# OBS looks for <name>\bin\64bit\<name>.dll here; the plugin has no data files,
# and OBS does not require a data folder.
$target = Join-Path $env:ProgramData 'obs-studio\plugins\livesplit-gold-clips\bin\64bit'
New-Item -ItemType Directory -Force -Path $target | Out-Null
Copy-Item -Force -Path $dll -Destination $target
Write-Host "Installed $dll to $target"
Write-Host 'Start OBS and check Tools > LiveSplit Gold Clips settings.'
