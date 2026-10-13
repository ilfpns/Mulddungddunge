@echo off
setlocal
rem Builds and runs the tests. Arguments go to restore-test (--snap, --e2e: see restore_test.cpp);
rem capture-memory-test runs too, except with --e2e or --kakao (which only use the running app).
rem --folders runs only folder-test against the running app (see folder_test.cpp; further arguments go to it).

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
set CL=/nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /permissive- /DUNICODE /D_UNICODE /Fo:obj\tests\

cl /Fe:bin\restore-test.exe tests\restore_test.cpp src\WindowTracker.cpp ^
   /link /SUBSYSTEM:CONSOLE ^
   user32.lib shell32.lib ole32.lib dwmapi.lib shcore.lib advapi32.lib version.lib windowsapp.lib || exit /b 1
cl /Fe:bin\capture-memory-test.exe tests\capture_memory_test.cpp src\Snapshot.cpp src\WindowTracker.cpp ^
   /link /SUBSYSTEM:CONSOLE ^
   user32.lib gdi32.lib shell32.lib ole32.lib dwmapi.lib shcore.lib d3d11.lib dxgi.lib d2d1.lib dwrite.lib ^
   windowscodecs.lib windowsapp.lib CoreMessaging.lib advapi32.lib version.lib || exit /b 1
cl /Fe:bin\folder-test.exe tests\folder_test.cpp /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib dwmapi.lib advapi32.lib || exit /b 1

rem The trace and the app's own temp files are in the real %TEMP%, not the compiler's.
endlocal & set "FAILED=0" & set "ARGS=%*"
setlocal
cd /d "%~dp0.."
echo %ARGS% | find "--folders" >nul
if errorlevel 1 goto others
bin\folder-test.exe %ARGS:--folders=%
exit /b %errorlevel%

:others
echo %ARGS% | findstr /c:"--e2e" /c:"--kakao" >nul
if errorlevel 1 (
  echo == capture memory
  bin\capture-memory-test.exe || set FAILED=1
  echo.
  echo == restore
)
bin\restore-test.exe %ARGS% || set FAILED=1
exit /b %FAILED%
