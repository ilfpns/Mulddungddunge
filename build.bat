@echo off
setlocal
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
  echo Visual Studio C++ tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj

rc /nologo /fo obj\app.res res\app.rc || exit /b 1

cl /nologo /std:c++20 /EHsc /O2 /GL /Gy /MT /W3 /utf-8 /permissive- /DUNICODE /D_UNICODE ^
   /Fo:obj\ /Fe:bin\stage-manager.exe src\*.cpp obj\app.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /LTCG /OPT:REF /OPT:ICF ^
   user32.lib gdi32.lib shell32.lib ole32.lib dwmapi.lib shcore.lib ^
   d3d11.lib dxgi.lib d2d1.lib dwrite.lib windowscodecs.lib windowsapp.lib CoreMessaging.lib advapi32.lib
exit /b %errorlevel%
