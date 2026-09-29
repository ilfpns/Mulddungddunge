@echo off
setlocal

rem vswhere's path contains "(x86)", which breaks a for /f command; go through a temp file instead.
set VSDIR=
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * ^
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\stage-manager-vsdir.txt" 2>nul
set /p VSDIR=<"%TEMP%\stage-manager-vsdir.txt"
del "%TEMP%\stage-manager-vsdir.txt" 2>nul
if not defined VSDIR (
  echo Visual Studio C++ tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj
rem Compiler scratch files next to the project instead of %TEMP% (which may be on a nearly full C: drive).
if not exist obj	mp mkdir obj	mp
set TMP=%~dp0obj	mp
set TEMP=%~dp0obj	mp

rc /nologo /fo obj\app.res res\app.rc || exit /b 1

cl /nologo /std:c++20 /EHsc /O2 /Gy /MT /W3 /utf-8 /permissive- /DUNICODE /D_UNICODE ^
   /Fo:obj\ /Fe:bin\stage-manager.exe src\*.cpp obj\app.res ^
   /link /SUBSYSTEM:WINDOWS /MAP:obj\stage-manager.map /MANIFEST:NO /OPT:REF /OPT:ICF ^
   user32.lib gdi32.lib shell32.lib ole32.lib dwmapi.lib shcore.lib ^
   d3d11.lib dxgi.lib d2d1.lib dwrite.lib windowscodecs.lib windowsapp.lib CoreMessaging.lib advapi32.lib
exit /b %errorlevel%
