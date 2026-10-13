@echo off
setlocal
rem Builds and runs the tests. Arguments go to the test program (--snap, --e2e: see restore_test.cpp).

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

cd /d "%~dp0.."
if not exist bin mkdir bin
if not exist obj\tests mkdir obj\tests
if not exist obj\tmp mkdir obj\tmp
set TMP=%~dp0..\obj\tmp
set TEMP=%~dp0..\obj\tmp

cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /permissive- /DUNICODE /D_UNICODE ^
   /Fo:obj\tests\ /Fe:bin\restore-test.exe tests\restore_test.cpp src\WindowTracker.cpp ^
   /link /SUBSYSTEM:CONSOLE ^
   user32.lib shell32.lib ole32.lib dwmapi.lib shcore.lib advapi32.lib version.lib windowsapp.lib || exit /b 1

bin\restore-test.exe %*
exit /b %errorlevel%
