# Per-user install: no administrator rights needed.
#   -InstallDir <path>  install somewhere else (default: %LOCALAPPDATA%\Programs\StageManager)
#   -Quiet              no dialogs; keeps the current auto-start setting
#   -NoRegister         copy files only (no shortcut, uninstall entry, auto-start, launch) - for testing
param(
    [string]$InstallDir,
    [switch]$Quiet,
    [switch]$NoRegister
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
if (-not $InstallDir) { $InstallDir = $DefaultDir }

# Windows.UI.Composition features used by the app need Windows 10 1903 (build 18362) or later.
$build = [int](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuildNumber
if ($build -lt 18362) {
    if (-not $Quiet) { Show-Message "Windows 10 1903 (빌드 18362) 이상이 필요해요.`n지금 버전: 빌드 $build`n`nWindows 업데이트 후 다시 설치해 주세요." 'OK' 'Warning' | Out-Null }
    exit 2
}

try {
    Stop-StageManager
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    foreach ($file in @($ExeName, 'uninstall.ps1', 'common.ps1')) {
        Copy-Item -Force (Join-Path $PSScriptRoot $file) (Join-Path $InstallDir $file)
    }
    $exe = Join-Path $InstallDir $ExeName
    if ($NoRegister) { return }

    # Start menu shortcut
    $shell = New-Object -ComObject WScript.Shell
    $lnk = $shell.CreateShortcut($ShortcutLnk)
    $lnk.TargetPath = $exe
    $lnk.WorkingDirectory = $InstallDir
    $lnk.IconLocation = "$exe,0"
    $lnk.Description = $AppName
    $lnk.Save()

    # Settings > Apps > Installed apps entry
    $version = (Get-Item $exe).VersionInfo.ProductVersion
    $sizeKb = [int]((Get-ChildItem $InstallDir | Measure-Object Length -Sum).Sum / 1KB)
    New-Item -Force -Path $UninstallKey | Out-Null
    $uninstall = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$InstallDir\uninstall.ps1`""
    $values = @{
        DisplayName = $AppName; DisplayVersion = $version; Publisher = 'Mulddungddunge'
        DisplayIcon = "$exe,0"; InstallLocation = $InstallDir; UninstallString = $uninstall
        QuietUninstallString = "$uninstall -Quiet"; URLInfoAbout = 'https://github.com/ilfpns/Mulddungddunge'
    }
    foreach ($k in $values.Keys) { Set-ItemProperty -Path $UninstallKey -Name $k -Value $values[$k] }
    Set-ItemProperty -Path $UninstallKey -Name NoModify -Value 1 -Type DWord
    Set-ItemProperty -Path $UninstallKey -Name NoRepair -Value 1 -Type DWord
    Set-ItemProperty -Path $UninstallKey -Name EstimatedSize -Value $sizeKb -Type DWord

    # Auto-start: ask once; the tray menu can change it later.
    $autostart = [bool](Get-ItemProperty -Path $RunKey -Name $AppId -ErrorAction SilentlyContinue)
    if (-not $Quiet) {
        $autostart = (Show-Message "Windows를 시작할 때 $AppName 를 자동으로 실행할까요?`n(나중에 트레이 아이콘 메뉴에서 바꿀 수 있어요.)" 'YesNo' 'Question') -eq 'Yes'
    }
    if ($autostart) { Set-ItemProperty -Path $RunKey -Name $AppId -Value "`"$exe`"" }
    else { Remove-ItemProperty -Path $RunKey -Name $AppId -ErrorAction SilentlyContinue }

    Start-Process -FilePath $exe -WorkingDirectory $InstallDir
    if (-not $Quiet) {
        Show-Message "설치가 끝났어요.`n`n왼쪽 사이드바의 카드를 누르거나 Alt+1~4 로 창을 전환할 수 있어요.`n끄려면 작업 표시줄 트레이 아이콘을 우클릭하세요." | Out-Null
    }
}
catch {
    if (-not $Quiet) { Show-Message "설치하지 못했어요.`n`n$($_.Exception.Message)" 'OK' 'Error' | Out-Null }
    exit 1
}
