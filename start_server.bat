@echo off
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
if not exist native\dst_server.exe call build_native.bat
if not exist native\dst_server.exe exit /b 1
:loop
native\dst_server.exe --port 19999 --cache "%~dp0translation_memory_c.tsv" --api-config "%~dp0config\api.ini" >> logs\server_stdout.log 2>> logs\server_stderr.log
timeout /t 2 /nobreak >nul
goto loop
