@echo off
rem One-command builder. Example:
rem   build.cmd -ExeFS D:\dumps\totk\exefs -RomFS D:\dumps\totk\romfs
rem Options: -OutputDir <folder>  -Workers <n>  -CheckInputs  -From <step>  -Only <step>  -DryRun
rem See README.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
