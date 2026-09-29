# Shared by install.ps1 and uninstall.ps1.

$AppName     = 'Stage Manager for Windows'
$AppId       = 'StageManager'
$ExeName     = 'stage-manager.exe'
$DefaultDir  = Join-Path $env:LOCALAPPDATA 'Programs\StageManager'
$ShortcutLnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'Stage Manager.lnk'
$UninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$AppId"
$RunKey      = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$StateKey    = 'HKCU:\Software\StageManager'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -Namespace StageManagerSetup -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, IntPtr title);
[DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint action, uint size, int[] info, uint winIni);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
'@

# Asks a running instance to quit through its tray "exit" command, so it restores the system
# minimize animation and releases the reserved sidebar area. Falls back to ending the process.
function Stop-StageManager {
    # A $null string would reach FindWindow as "" (a window with an empty title), so pass a null pointer.
    $hwnd = [StageManagerSetup.Native]::FindWindow('StageManagerSidebar', [IntPtr]::Zero)
    if ($hwnd -ne [IntPtr]::Zero) {
        [StageManagerSetup.Native]::PostMessage($hwnd, 0x0111, [IntPtr]1, [IntPtr]::Zero) | Out-Null   # WM_COMMAND, ID_EXIT
    }
    for ($i = 0; $i -lt 30 -and (Get-Process -Name 'stage-manager' -ErrorAction SilentlyContinue); $i++) {
        Start-Sleep -Milliseconds 100
    }
    $stuck = Get-Process -Name 'stage-manager' -ErrorAction SilentlyContinue
    if ($stuck) { $stuck | Stop-Process -Force }
    Restore-MinimizeAnimation
}

# The app turns the system minimize animation off while it runs and records the original value in
# HKCU\Software\StageManager; if it could not restore it itself (killed), do it here.
function Restore-MinimizeAnimation {
    $state = Get-ItemProperty -Path $StateKey -Name MinAnimate -ErrorAction SilentlyContinue
    if ($null -eq $state) { return }
    [StageManagerSetup.Native]::SystemParametersInfo(0x0049, 8, @(8, [int]$state.MinAnimate), 0x2) | Out-Null   # SPI_SETANIMATION
    Remove-ItemProperty -Path $StateKey -Name MinAnimate -ErrorAction SilentlyContinue
}

function Show-Message([string]$text, [string]$buttons = 'OK', [string]$icon = 'Information') {
    [System.Windows.Forms.MessageBox]::Show($text, $AppName, $buttons, $icon)
}
