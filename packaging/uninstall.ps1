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

$dir = $PSScriptRoot
Start-Process -WindowStyle Hidden -FilePath cmd.exe -ArgumentList "/c timeout /t 2 /nobreak >nul & rmdir /s /q `"$dir`""
if (-not $Quiet) { Show-Message "제거했어요." | Out-Null }
