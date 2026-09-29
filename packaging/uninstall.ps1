# Removes everything install.ps1 created. Runs from the install folder, so the folder itself is
# deleted by a short-lived cmd.exe after this script exits.
param([switch]$Quiet)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

if (-not $Quiet -and (Show-Message "$AppName 를 제거할까요?" 'YesNo' 'Question') -ne 'Yes') { exit 0 }

Stop-StageManager
Remove-ItemProperty -Path $RunKey -Name $AppId -ErrorAction SilentlyContinue
Remove-Item -Force $ShortcutLnk -ErrorAction SilentlyContinue
Remove-Item -Force -Recurse $UninstallKey -ErrorAction SilentlyContinue
Remove-Item -Force -Recurse $StateKey -ErrorAction SilentlyContinue

# Only the files install.ps1 copied; the folder goes too if that leaves it empty (it may be one the
# user chose with -InstallDir that holds other things).
$dir = $PSScriptRoot
# A short-lived PowerShell (not cmd, which would expand %...% in the path) started outside the folder,
# so it isn't holding the folder open. Paths go in as single-quoted literals.
$q = { param($p) "'" + ($p -replace "'", "''") + "'" }
$files = ($ExeName, 'uninstall.ps1', 'common.ps1' | ForEach-Object { & $q (Join-Path $dir $_) }) -join ','
$cleanup = "Start-Sleep 2; Remove-Item -LiteralPath $files -Force -ErrorAction SilentlyContinue; " +
           "if (-not (Get-ChildItem -LiteralPath $(& $q $dir) -Force)) { Remove-Item -LiteralPath $(& $q $dir) -Force }"
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cleanup))
Start-Process -WindowStyle Hidden -WorkingDirectory $env:TEMP -FilePath powershell.exe -ArgumentList "-NoProfile -ExecutionPolicy Bypass -EncodedCommand $encoded"
if (-not $Quiet) { Show-Message "제거했어요." | Out-Null }
