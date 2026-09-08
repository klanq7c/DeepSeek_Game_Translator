@echo off
setlocal EnableDelayedExpansion
set "ROOT=%~dp0"
set "APP_VERSION=dev"
if exist "%ROOT%VERSION" (
    set /p APP_VERSION=<"%ROOT%VERSION"
)
set "APP_VERSION_COMMA=0,0,0,0"
for /f "tokens=1-5 delims=." %%a in ("%APP_VERSION%") do (
    if not "%%a"=="" if not "%%b"=="" if not "%%c"=="" if not "%%d"=="" if "%%e"=="" (
        set "APP_VERSION_COMMA=%%a,%%b,%%c,%%d"
    )
)
set "BIN=%ROOT%native\toolchain\w64devkit\bin"
if exist "%BIN%\gcc.exe" (
    set "PATH=%BIN%;%PATH%"
) else (
    where gcc >nul 2>nul
    if errorlevel 1 (
        echo Missing gcc. Install w64devkit and add its bin directory to PATH, or place it at %BIN%.
        exit /b 1
    )
    where windres >nul 2>nul
    if errorlevel 1 (
        echo Missing windres. Install w64devkit and add its bin directory to PATH, or place it at %BIN%.
        exit /b 1
    )
)

rem First-party managed payloads (XUnity endpoint, TMP fallback, UnityTranslator,
rem font patcher). Kept in its own script so its fingerprint only changes when the
rem managed build recipe changes.
call "%ROOT%scripts\build_managed_payloads.bat"
if errorlevel 1 exit /b 1

set "SVR=%ROOT%native\src\server"
set SVR_SRC="%SVR%\main.c" "%SVR%\util.c" "%SVR%\buf.c" "%SVR%\b64.c" "%SVR%\json.c" "%SVR%\cache.c" "%SVR%\api.c" "%SVR%\http.c"

gcc -std=c17 -O2 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -I"%SVR%" %SVR_SRC% -lws2_32 -lwinhttp -o "%ROOT%native\dst_server.exe"
if errorlevel 1 exit /b 1

rem C# server (dst_server_cs): same HTTP contract as dst_server, selectable via
rem launcher.ini [server] binary=cs. Built before the launcher so it can be
rem embedded (RCDATA 105) and shipped inside the single-file program release.
rem Shares native\src\core with the Unity payloads and tests\core_tests.
set "SERVER_CS=%ROOT%native\src\server_cs\server_cs.csproj"
where dotnet >nul 2>nul
if errorlevel 1 (
    echo Missing dotnet SDK needed to build the C# server.
    exit /b 1
)
dotnet build "%SERVER_CS%" -c Release --nologo --no-incremental
if errorlevel 1 (
    echo C# server build failed.
    exit /b 1
)
if not exist "%ROOT%native\dst_server_cs.exe" (
    echo C# server build succeeded but native\dst_server_cs.exe was not found.
    exit /b 1
)
rem Shared-core unit tests: cheap, and the server above compiled the same files.
dotnet build "%ROOT%tests\core_tests\core_tests.csproj" -c Release --nologo -v q
if errorlevel 1 (
    echo core_tests build failed.
    exit /b 1
)
"%ROOT%tests\core_tests\bin\Release\net472\dst_core_tests.exe"
if errorlevel 1 (
    echo Shared core tests failed.
    exit /b 1
)

set "RES_RC=%ROOT%build\launcher_payloads.rc"
set "RES_OBJ=%ROOT%build\launcher_payloads.o"
if not exist "%ROOT%build" mkdir "%ROOT%build"
if not exist "%ROOT%assets\app_icon.ico" (
    echo Missing application icon: assets\app_icon.ico
    exit /b 1
)
rem Resource paths inside the .rc must be absolute (forward slashes only;
rem rc treats backslash as escape) so windres works from any caller CWD.
set "ROOT_RC=%ROOT:\=/%"
> "%RES_RC%" echo 1 ICON "%ROOT_RC%assets/app_icon.ico"
>> "%RES_RC%" echo 1 VERSIONINFO
>> "%RES_RC%" echo FILEVERSION !APP_VERSION_COMMA!
>> "%RES_RC%" echo PRODUCTVERSION !APP_VERSION_COMMA!
>> "%RES_RC%" echo FILEFLAGSMASK 0x3fL
>> "%RES_RC%" echo FILEFLAGS 0x0L
>> "%RES_RC%" echo FILEOS 0x40004L
>> "%RES_RC%" echo FILETYPE 0x1L
>> "%RES_RC%" echo FILESUBTYPE 0x0L
>> "%RES_RC%" echo BEGIN
>> "%RES_RC%" echo BLOCK "StringFileInfo"
>> "%RES_RC%" echo BEGIN
>> "%RES_RC%" echo BLOCK "040904b0"
>> "%RES_RC%" echo BEGIN
>> "%RES_RC%" echo VALUE "FileDescription", "ds Game Translator"
>> "%RES_RC%" echo VALUE "FileVersion", "%APP_VERSION%"
>> "%RES_RC%" echo VALUE "InternalName", "ds-game-translator"
>> "%RES_RC%" echo VALUE "OriginalFilename", "ds-game-translator.exe"
>> "%RES_RC%" echo VALUE "ProductName", "ds Game Translator"
>> "%RES_RC%" echo VALUE "ProductVersion", "%APP_VERSION%"
>> "%RES_RC%" echo END
>> "%RES_RC%" echo END
>> "%RES_RC%" echo BLOCK "VarFileInfo"
>> "%RES_RC%" echo BEGIN
>> "%RES_RC%" echo VALUE "Translation", 0x409, 1200
>> "%RES_RC%" echo END
>> "%RES_RC%" echo END
>> "%RES_RC%" echo 101 RCDATA "%ROOT_RC%native/dst_server.exe"
>> "%RES_RC%" echo 102 RCDATA "%ROOT_RC%scripts/install_runtime_payloads.ps1"
>> "%RES_RC%" echo 103 RCDATA "%ROOT_RC%config/api.ini.example"
>> "%RES_RC%" echo 104 RCDATA "%ROOT_RC%config/launcher.ini.example"
>> "%RES_RC%" echo 105 RCDATA "%ROOT_RC%native/dst_server_cs.exe"
>> "%RES_RC%" echo 106 RCDATA "%ROOT_RC%config/glossary.example.tsv"
rem Engine runtime scripts (first-party sources, always present). IDs match
rem native\src\launcher\resource.h; deploy.c/godot_patch.c write them verbatim.
for %%S in (payloads\RenPy\iron_deepseek.rpy payloads\RPGMaker\hook_rpgm_mv.js payloads\Godot\dst_godot_runtime_g3.gd payloads\Godot\dst_godot_runtime_g4.gd) do (
    if not exist "%ROOT%%%S" (
        echo Missing engine runtime script: %%S
        exit /b 1
    )
)
>> "%RES_RC%" echo 301 RCDATA "%ROOT_RC%payloads/RenPy/iron_deepseek.rpy"
>> "%RES_RC%" echo 302 RCDATA "%ROOT_RC%payloads/RPGMaker/hook_rpgm_mv.js"
>> "%RES_RC%" echo 303 RCDATA "%ROOT_RC%payloads/Godot/dst_godot_runtime_g3.gd"
>> "%RES_RC%" echo 304 RCDATA "%ROOT_RC%payloads/Godot/dst_godot_runtime_g4.gd"
if exist "%ROOT%payloads\UnityTranslator\UnityTranslator.dll" (
    >> "%RES_RC%" echo 201 RCDATA "%ROOT_RC%payloads/UnityTranslator/UnityTranslator.dll"
)
if exist "%ROOT%payloads\UnityTranslator\UnityTranslator.BepInEx6.dll" (
    >> "%RES_RC%" echo 202 RCDATA "%ROOT_RC%payloads/UnityTranslator/UnityTranslator.BepInEx6.dll"
)
if exist "%ROOT%payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll" (
    >> "%RES_RC%" echo 203 RCDATA "%ROOT_RC%payloads/UnityIL2CPP/DeepSeekXUnityTranslator/DeepSeekTranslate.dll"
)
if exist "%ROOT%payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll" (
    >> "%RES_RC%" echo 204 RCDATA "%ROOT_RC%payloads/UnityIL2CPP/DeepSeekTMPFontFallback/BepInEx/plugins/DeepSeekTMPFontFallback/DeepSeekTMPFontFallback.dll"
)
if exist "%ROOT%payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll" (
    >> "%RES_RC%" echo 205 RCDATA "%ROOT_RC%payloads/UnityTranslator/DeepSeekUnityFontPatcher.dll"
)
windres "%RES_RC%" -O coff -o "%RES_OBJ%"
if errorlevel 1 exit /b 1

set "LCH=%ROOT%native\src\launcher"
set LCH_SRC="%LCH%\main.c" "%LCH%\globals.c" "%LCH%\fsutil.c" "%LCH%\engine.c" "%LCH%\deploy.c" "%LCH%\server_proc.c" "%LCH%\api_config.c" "%LCH%\warmup.c" "%LCH%\godot_warmup.c" "%LCH%\godot_patch.c" "%LCH%\godot_probe.c" "%LCH%\godot_preflight_cache.c" "%LCH%\ui.c" "%LCH%\ui_probe.c" "%LCH%\self_update.c" "%LCH%\embedded.c"
set "LAUNCHER_TMP=%ROOT%build\launcher_build.exe"

rem Build to an ASCII temp path first; gcc/binutils handle the final binary
rem bytes there, then PowerShell moves it to the Chinese product filename.
gcc -std=c17 -O2 -Wall -Wextra -Werror -municode -mwindows -D_CRT_SECURE_NO_WARNINGS -DDS_TRANSLATOR_VERSION=\"%APP_VERSION%\" -I"%LCH%" %LCH_SRC% "%RES_OBJ%" -lcomctl32 -lshell32 -lole32 -lmsimg32 -lwinhttp -o "%LAUNCHER_TMP%"
if errorlevel 1 exit /b 1

powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $ds='ds'+[string][char]0x6e38+[string][char]0x620f+[string][char]0x7ffb+[string][char]0x8bd1+[string][char]0x5668; $dest = Join-Path $env:ROOT ($ds + '.exe'); if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Force }; Move-Item -LiteralPath $env:LAUNCHER_TMP -Destination $dest -Force; $legacy = Join-Path $env:ROOT 'DeepSeekTranslator.exe'; if (Test-Path -LiteralPath $legacy) { Remove-Item -LiteralPath $legacy -Force }"
if errorlevel 1 exit /b 1

powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\verify_build_artifacts.ps1" -RequireComplete
if errorlevel 1 (
    echo Final launcher resource verification failed.
    exit /b 1
)

rem ---- Migration phase 3: C# launcher port (native\src\launcher_cs) ----
rem Not shipped yet. Keep it compiling and run tests\launcher_parity, which diffs the
rem ported modules (engine detection, Ren'Py/RPGM/Godot deploy + restore) against the
rem C launcher byte for byte. Keep this file ASCII-only: cmd.exe seeks batch files by
rem character count, so multibyte comments corrupt line parsing.
dotnet build "%ROOT%native\src\launcher_cs\launcher_cs.csproj" -c Release -nologo -v quiet -p:OutputPath="%ROOT%build\launcher_cs\"
if errorlevel 1 (
    echo C# launcher port failed to build.
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tests\launcher_parity\run_launcher_parity.ps1" -CsLauncher "%ROOT%build\launcher_cs\dst_launcher_cs.exe"
if errorlevel 1 (
    echo Launcher parity check failed: C# port diverges from the C launcher.
    exit /b 1
)

echo Built native server, C# server and launcher.
