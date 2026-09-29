@echo off
rem Entry point run by the self-extracting setup (IExpress passes no long command lines well).
powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0install.ps1" %*
