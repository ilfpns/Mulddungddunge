# Installs what is needed to build Stage Manager from source, using winget (built into Windows 10/11):
#   - Git
#   - Visual Studio 2022 Build Tools with the C++ workload and a Windows 11 SDK
#   - Python is NOT needed to build; it is only used by optional test scripts.
# Run from an ordinary PowerShell window:
#   powershell -ExecutionPolicy Bypass -File tools\setup-dev.ps1
# The Build Tools installer asks for administrator rights once. Nothing is installed if already present.
$ErrorActionPreference = 'Stop'

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Host 'winget이 없습니다. Microsoft Store에서 "앱 설치 관리자(App Installer)"를 설치한 뒤 다시 실행하세요.' -ForegroundColor Yellow
    exit 1
}

function Test-CppTools {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $false }
    $path = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $sdk = Test-Path (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include\*\cppwinrt\winrt\base.h')
    return [bool]$path -and $sdk
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host '[1/2] Git 설치 중...'
    winget install --id Git.Git -e --source winget --accept-package-agreements --accept-source-agreements
} else {
    Write-Host '[1/2] Git: 이미 설치됨'
}

if (Test-CppTools) {
    Write-Host '[2/2] Visual Studio C++ 빌드 도구와 Windows SDK: 이미 설치됨'
} else {
    Write-Host '[2/2] Visual Studio 2022 Build Tools (C++, Windows 11 SDK) 설치 중... 몇 분 걸립니다.'
    winget install --id Microsoft.VisualStudio.2022.BuildTools -e --source winget --accept-package-agreements --accept-source-agreements `
        --override '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621'
    if (-not (Test-CppTools)) {
        Write-Host '설치가 끝나지 않았거나 재부팅이 필요할 수 있습니다. 재부팅 후 다시 실행해 확인하세요.' -ForegroundColor Yellow
        exit 1
    }
}

Write-Host ''
Write-Host '준비 완료. 빌드:  build.bat' -ForegroundColor Green
Write-Host '패키지:           powershell -ExecutionPolicy Bypass -File packaging\package.ps1' -ForegroundColor Green
