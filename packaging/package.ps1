# Builds release packages into dist\:
#   StageManager-<version>-portable.zip   the exe and a short readme; unzip and run
#   StageManager-<version>-win64.exe      self-extracting per-user installer (IExpress, built into Windows)
# Usage: powershell -ExecutionPolicy Bypass -File packaging\package.ps1 [-NoBuild]
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
Set-Location $root

if (-not $NoBuild) {
    cmd /c "`"$root\build.bat`""
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}
$version = ([regex]::Match((Get-Content "$root\res\version.h" -Raw), 'APP_VERSION_STR "([^"]+)"')).Groups[1].Value
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'staging'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item "$root\bin\stage-manager.exe" $stage
foreach ($f in 'install.ps1', 'uninstall.ps1', 'common.ps1', 'setup.cmd') { Copy-Item "$PSScriptRoot\$f" $stage }

# ---- portable zip
$portable = Join-Path $dist "StageManager-$version-portable"
Remove-Item -Recurse -Force $portable, "$portable.zip" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $portable | Out-Null
Copy-Item "$root\bin\stage-manager.exe" $portable
@"
Stage Manager for Windows $version

stage-manager.exe 를 실행하면 왼쪽에 사이드바가 생깁니다.
- 카드 클릭 또는 Alt+1~4 : 창 전환
- 카드를 끌어 화면 가운데에 놓기 : 지금 창 옆에 함께 열기
- 카드 우클릭 : 탭 고정 / 창 닫기
- 트레이 아이콘 우클릭 : Windows 시작 시 실행, 종료

설치 없이 쓰는 버전입니다. 시작 메뉴 등록과 제거 항목이 필요하면 설치 파일(StageManager-$version-win64.exe)을 쓰세요.
"@ | Set-Content -Encoding UTF8 (Join-Path $portable 'README.txt')
Compress-Archive -Path "$portable\*" -DestinationPath "$portable.zip"
Remove-Item -Recurse -Force $portable

# ---- self-extracting installer
# The name avoids "setup"/"install" so Windows' installer heuristics don't ask for administrator rights.
$target = Join-Path $dist "StageManager-$version-win64.exe"
Remove-Item -Force $target -ErrorAction SilentlyContinue
$files = @(Get-ChildItem $stage -File)
$fileNames = ((0..($files.Count - 1)) | ForEach-Object { "FILE$_=`"$($files[$_].Name)`"" }) -join "`r`n"
$fileRefs = ((0..($files.Count - 1)) | ForEach-Object { "%FILE$_%=" }) -join "`r`n"
$sed = @"
[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=1
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=%InstallPrompt%
DisplayLicense=%DisplayLicense%
FinishMessage=%FinishMessage%
TargetName=%TargetName%
FriendlyName=%FriendlyName%
AppLaunched=%AppLaunched%
PostInstallCmd=%PostInstallCmd%
AdminQuietInstCmd=%AdminQuietInstCmd%
UserQuietInstCmd=%UserQuietInstCmd%
SourceFiles=SourceFiles
[Strings]
InstallPrompt=
DisplayLicense=
FinishMessage=
TargetName=$target
FriendlyName=Stage Manager for Windows $version
AppLaunched=cmd.exe /c setup.cmd
PostInstallCmd=<None>
AdminQuietInstCmd=cmd.exe /c setup.cmd -Quiet
UserQuietInstCmd=cmd.exe /c setup.cmd -Quiet
$fileNames
[SourceFiles]
SourceFiles0=$stage\
[SourceFiles0]
$fileRefs
"@
$sedPath = Join-Path $dist 'installer.sed'
$sed | Set-Content -Encoding ASCII $sedPath
# IExpress can't take a quoted path; run it from dist\ with a bare file name so spaces don't matter.
Start-Process -Wait -WindowStyle Hidden -WorkingDirectory $dist -FilePath "$env:WINDIR\System32\iexpress.exe" -ArgumentList '/N', '/Q', 'installer.sed'
if (-not (Test-Path $target)) { throw 'IExpress did not produce the installer' }
Remove-Item -Force $sedPath
Remove-Item -Recurse -Force $stage

Get-ChildItem $dist | Select-Object Name, @{ n = 'KB'; e = { [int]($_.Length / 1KB) } } | Format-Table -AutoSize
