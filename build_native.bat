@echo off
setlocal EnableDelayedExpansion
set "ROOT=%~dp0"
set "APP_VERSION=dev"
if exist "%ROOT%VERSION" (
    set /p APP_VERSION=<"%ROOT%VERSION"
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

set "XUT=%ROOT%payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src\DeepSeekXUnityTranslator.csproj"
set "XUT_CORE=%ROOT%payloads\UnityIL2CPP\XUnityAutoTranslator\BepInEx\plugins\XUnity.AutoTranslator\XUnity.AutoTranslator.Plugin.Core.dll"
if exist "%XUT%" (
    if exist "%XUT_CORE%" (
        where dotnet >nul 2>nul
        if errorlevel 1 (
            echo Missing dotnet SDK needed to build DeepSeek XUnity translator endpoint.
            exit /b 1
        )
        dotnet build "%XUT%" -c Release --nologo
        if errorlevel 1 exit /b 1
    ) else (
        echo Skipping DeepSeek XUnity endpoint source build: XUnity.AutoTranslator runtime was not found.
        if exist "%ROOT%payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll" (
            echo Existing payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll will be used.
        ) else (
            echo Missing DeepSeek XUnity endpoint payload. Run scripts\install_runtime_payloads.ps1 -UnityIL2CPP first, or use the program release that embeds this first-party DLL.
            exit /b 1
        )
    )
)

set "TMPF=%ROOT%payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src\DeepSeekTMPFontFallback.csproj"
if exist "%TMPF%" (
    where dotnet >nul 2>nul
    if errorlevel 1 (
        echo Missing dotnet SDK needed to build DeepSeek TMP font fallback plugin.
        exit /b 1
    )

    set "TMPF_BEP=%ROOT%payloads\UnityIL2CPP\BepInExRuntime\BepInEx\core"
    set "TMPF_INTEROP=%IL2CPP_INTEROP_DIR%"
    if not defined TMPF_INTEROP (
        if exist "%ROOT%payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src\UnityInteropRefs\Il2Cppmscorlib.dll" (
            set "TMPF_INTEROP=%ROOT%payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src\UnityInteropRefs"
        )
    )

    if defined TMPF_INTEROP (
        if not exist "!TMPF_BEP!\BepInEx.Unity.IL2CPP.dll" (
            echo Missing BepInEx IL2CPP core references at !TMPF_BEP!.
            exit /b 1
        )
        if not exist "!TMPF_INTEROP!\Il2Cppmscorlib.dll" (
            echo Invalid IL2CPP_INTEROP_DIR: !TMPF_INTEROP!
            echo Expected Il2Cppmscorlib.dll in that directory.
            exit /b 1
        )
        if not exist "!TMPF_INTEROP!\UnityEngine.CoreModule.dll" (
            echo Invalid IL2CPP_INTEROP_DIR: !TMPF_INTEROP!
            echo Expected UnityEngine.CoreModule.dll in that directory.
            exit /b 1
        )
        if not exist "!TMPF_INTEROP!\UnityEngine.AssetBundleModule.dll" (
            echo Invalid IL2CPP_INTEROP_DIR: !TMPF_INTEROP!
            echo Expected UnityEngine.AssetBundleModule.dll in that directory.
            exit /b 1
        )
        if not exist "!TMPF_INTEROP!\UnityEngine.TextRenderingModule.dll" (
            echo Invalid IL2CPP_INTEROP_DIR: !TMPF_INTEROP!
            echo Expected UnityEngine.TextRenderingModule.dll in that directory.
            exit /b 1
        )
        dotnet build "%TMPF%" -c Release --nologo -p:BepInExCoreDir="!TMPF_BEP!" -p:UnityInteropDir="!TMPF_INTEROP!"
        if errorlevel 1 exit /b 1
        echo Built DeepSeek TMP font fallback plugin for IL2CPP.
    ) else (
        echo Skipping DeepSeek TMP font fallback source build: IL2CPP_INTEROP_DIR is not set and UnityInteropRefs was not found.
        if exist "%ROOT%payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll" (
            echo Existing payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll will be used.
        ) else (
            echo Missing DeepSeek TMP font fallback payload. Set IL2CPP_INTEROP_DIR to a generated IL2CPP interop reference folder, or use the program release that embeds this first-party DLL.
            exit /b 1
        )
    )
)

set "UT=%ROOT%payloads\UnityTranslator\src\UnityTranslator.csproj"
if exist "%UT%" (
    where dotnet >nul 2>nul
    if errorlevel 1 (
        echo Missing dotnet SDK needed to build UnityTranslator plugin.
        exit /b 1
    )

    set "UT_MANAGED=%UNITY_MANAGED_DIR%"
    if not defined UT_MANAGED (
        if exist "%ROOT%payloads\UnityTranslator\src\UnityManagedRefs\UnityEngine.CoreModule.dll" (
            set "UT_MANAGED=%ROOT%payloads\UnityTranslator\src\UnityManagedRefs"
        )
    )
    if not defined UT_MANAGED (
        if exist "%ROOT%payloads\UnityTranslator\src\bin\Release\net472\UnityEngine.CoreModule.dll" (
            set "UT_MANAGED=%ROOT%payloads\UnityTranslator\src\bin\Release\net472"
        )
    )

    if defined UT_MANAGED (
        if not exist "!UT_MANAGED!\UnityEngine.CoreModule.dll" (
            echo Invalid UNITY_MANAGED_DIR: !UT_MANAGED!
            echo Expected UnityEngine.CoreModule.dll in that directory.
            exit /b 1
        )

        dotnet build "%UT%" -c Release --nologo -p:UnityManagedDir="!UT_MANAGED!" -p:BepInExFlavor=5
        if errorlevel 1 exit /b 1
        if not exist "%ROOT%payloads\UnityTranslator\src\bin\Release\net472\UnityTranslator.dll" (
            echo UnityTranslator build succeeded but output DLL was not found.
            exit /b 1
        )
        copy /Y "%ROOT%payloads\UnityTranslator\src\bin\Release\net472\UnityTranslator.dll" "%ROOT%payloads\UnityTranslator\UnityTranslator.dll" >nul

        set "UT_BEPINEX6_OUT=%ROOT%payloads\UnityTranslator\src\bin\Release\net472-bepinex6"
        dotnet build "%UT%" -c Release --nologo -p:UnityManagedDir="!UT_MANAGED!" -p:BepInExFlavor=6 -o "!UT_BEPINEX6_OUT!"
        if errorlevel 1 exit /b 1
        if not exist "!UT_BEPINEX6_OUT!\UnityTranslator.dll" (
            echo UnityTranslator BepInEx6 build succeeded but output DLL was not found.
            exit /b 1
        )
        copy /Y "!UT_BEPINEX6_OUT!\UnityTranslator.dll" "%ROOT%payloads\UnityTranslator\UnityTranslator.BepInEx6.dll" >nul
        rmdir /S /Q "!UT_BEPINEX6_OUT!" >nul 2>nul
        echo Built UnityTranslator Mono payloads for BepInEx 5 and BepInEx 6.
    ) else (
        echo Skipping UnityTranslator Mono source build: UNITY_MANAGED_DIR is not set and UnityManagedRefs was not found.
        set "UT_PAYLOAD_READY=0"
        if exist "%ROOT%payloads\UnityTranslator\UnityTranslator.dll" if exist "%ROOT%payloads\UnityTranslator\UnityTranslator.BepInEx6.dll" set "UT_PAYLOAD_READY=1"
        if "!UT_PAYLOAD_READY!"=="1" (
            echo Existing payloads\UnityTranslator\UnityTranslator.dll and UnityTranslator.BepInEx6.dll will be used.
        ) else (
            echo Missing UnityTranslator Mono payloads. Set UNITY_MANAGED_DIR to a Unity Managed folder so both BepInEx 5 and 6 plugin DLLs can be built, or use the program release that embeds these first-party DLLs.
            exit /b 1
        )
    )
)

set "UT_FONT_PATCHER_PROJECT=%ROOT%payloads\UnityTranslator\src\FontPatcher\DeepSeekUnityFontPatcher.csproj"
set "UT_FONT_PATCHER_PAYLOAD=%ROOT%payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll"
set "UT_BEPINEX5_CORE=%ROOT%payloads\UnityMonoRuntime\BepInEx\core"
if exist "%UT_FONT_PATCHER_PROJECT%" (
    if exist "%UT_BEPINEX5_CORE%\Mono.Cecil.dll" (
        dotnet build "%UT_FONT_PATCHER_PROJECT%" -c Release --nologo -p:BepInEx5CoreDir="%UT_BEPINEX5_CORE%"
        if errorlevel 1 exit /b 1
        if not exist "%ROOT%payloads\UnityTranslator\src\FontPatcher\bin\Release\net472\DeepSeekUnityFontPatcher.dll" (
            echo DeepSeekUnityFontPatcher build succeeded but output DLL was not found.
            exit /b 1
        )
        copy /Y "%ROOT%payloads\UnityTranslator\src\FontPatcher\bin\Release\net472\DeepSeekUnityFontPatcher.dll" "%UT_FONT_PATCHER_PAYLOAD%" >nul
        echo Built stripped Unity Mono font metadata patcher.
    ) else (
        echo Skipping DeepSeekUnityFontPatcher source build: BepInEx 5 Mono.Cecil.dll was not found.
        if exist "%UT_FONT_PATCHER_PAYLOAD%" (
            echo Existing payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll will be used.
        ) else (
            echo Missing BepInEx 5 Mono.Cecil.dll needed to build DeepSeekUnityFontPatcher.
            exit /b 1
        )
    )
)
if not exist "%UT_FONT_PATCHER_PAYLOAD%" (
    echo Missing DeepSeekUnityFontPatcher.dll payload.
    exit /b 1
)

set "UT_JSON=%ROOT%payloads\UnityIL2CPP\XUnityAutoTranslator\BepInEx\plugins\XUnity.AutoTranslator\Translators\FullNET\Newtonsoft.Json.dll"
if exist "%UT_JSON%" (
    copy /Y "%UT_JSON%" "%ROOT%payloads\UnityTranslator\Newtonsoft.Json.dll" >nul
) else if not exist "%ROOT%payloads\UnityTranslator\Newtonsoft.Json.dll" (
    echo Missing Newtonsoft.Json.dll needed by UnityTranslator Mono payload.
    exit /b 1
)

rem Never embed a managed payload that predates its first-party source.  A
rem skipped optional build is acceptable only when the existing payload is
rem already current; otherwise "build succeeded" would package stale code.
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\verify_build_artifacts.ps1" -ManagedPayloadsOnly
if errorlevel 1 (
    echo Managed payload verification failed. Install the missing build references and rebuild the stale payload.
    exit /b 1
)

set "SVR=%ROOT%native\src\server"
set "SVR_SRC=%SVR%\main.c %SVR%\util.c %SVR%\buf.c %SVR%\b64.c %SVR%\json.c %SVR%\cache.c %SVR%\api.c %SVR%\http.c"

gcc -std=c17 -O2 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -I"%SVR%" %SVR_SRC% -lws2_32 -lwinhttp -o "%ROOT%native\dst_server.exe"
if errorlevel 1 exit /b 1

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
>> "%RES_RC%" echo 101 RCDATA "%ROOT_RC%native/dst_server.exe"
>> "%RES_RC%" echo 102 RCDATA "%ROOT_RC%scripts/install_runtime_payloads.ps1"
>> "%RES_RC%" echo 103 RCDATA "%ROOT_RC%config/api.ini.example"
>> "%RES_RC%" echo 104 RCDATA "%ROOT_RC%config/launcher.ini.example"
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
set "LCH_SRC=%LCH%\main.c %LCH%\globals.c %LCH%\fsutil.c %LCH%\engine.c %LCH%\deploy.c %LCH%\server_proc.c %LCH%\api_config.c %LCH%\warmup.c %LCH%\godot_warmup.c %LCH%\godot_patch.c %LCH%\godot_probe.c %LCH%\ui.c %LCH%\self_update.c"
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

echo Built native server and launcher.
