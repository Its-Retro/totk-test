@echo off
rem One-command builder. Example:
rem   build.cmd -ExeFS D:\dumps\totk\exefs -RomFS D:\dumps\totk\romfs
rem Options: -OutputDir <folder>  -Workers <n>  -CheckInputs  -From <step>  -Only <step>  -DryRun
rem See README.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set "RC=%ERRORLEVEL%"
rem Started by double-click (Explorer is the parent of this cmd.exe): keep the window open so the messages can be
rem read. Never pauses when run from a terminal, a script or Claude, or with TOTK_NO_PAUSE set.
if not defined TOTK_NO_PAUSE powershell -NoProfile -Command "$c=(Get-CimInstance Win32_Process -Filter ('ProcessId='+$PID)).ParentProcessId; $e=(Get-CimInstance Win32_Process -Filter ('ProcessId='+$c)).ParentProcessId; if ((Get-Process -Id $e -ErrorAction SilentlyContinue).ProcessName -eq 'explorer') { Write-Host ''; Read-Host 'Press Enter to close this window' | Out-Null }"
exit /b %RC%
