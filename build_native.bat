@echo off
setlocal EnableDelayedExpansion
set "ROOT=%~dp0"
set "BIN=%ROOT%native\toolchain\w64devkit\bin"
if not exist "%BIN%\gcc.exe" (
    echo Missing w64devkit gcc at %BIN%.
    exit /b 1
)
set "PATH=%BIN%;%PATH%"

set "XUT=%ROOT%payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src\DeepSeekXUnityTranslator.csproj"
if exist "%XUT%" (
    where dotnet >nul 2>nul
    if errorlevel 1 (
        echo Missing dotnet SDK needed to build DeepSeek XUnity translator endpoint.
        exit /b 1
    )
    dotnet build "%XUT%" -c Release --nologo
    if errorlevel 1 exit /b 1
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
        echo Existing payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll will be used.
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
        echo Existing payloads\UnityTranslator\UnityTranslator.dll and UnityTranslator.BepInEx6.dll will be used.
    )
)

set "UT_JSON=%ROOT%payloads\UnityIL2CPP\XUnityAutoTranslator\BepInEx\plugins\XUnity.AutoTranslator\Translators\FullNET\Newtonsoft.Json.dll"
if exist "%UT_JSON%" (
    copy /Y "%UT_JSON%" "%ROOT%payloads\UnityTranslator\Newtonsoft.Json.dll" >nul
) else if not exist "%ROOT%payloads\UnityTranslator\Newtonsoft.Json.dll" (
    echo Missing Newtonsoft.Json.dll needed by UnityTranslator Mono payload.
    exit /b 1
)

set "SVR=%ROOT%native\src\server"
set "SVR_SRC=%SVR%\main.c %SVR%\util.c %SVR%\buf.c %SVR%\b64.c %SVR%\json.c %SVR%\cache.c %SVR%\api.c %SVR%\http.c"

gcc -std=c17 -O2 -D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -I"%SVR%" %SVR_SRC% -lws2_32 -lwinhttp -o "%ROOT%native\dst_server.exe"
if errorlevel 1 exit /b 1

set "LCH=%ROOT%native\src\launcher"
set "LCH_SRC=%LCH%\main.c %LCH%\globals.c %LCH%\fsutil.c %LCH%\engine.c %LCH%\deploy.c %LCH%\server_proc.c %LCH%\api_config.c %LCH%\warmup.c %LCH%\ui.c"

gcc -std=c17 -O2 -municode -mwindows -D_CRT_SECURE_NO_WARNINGS -I"%LCH%" %LCH_SRC% -lcomctl32 -lshell32 -lole32 -lmsimg32 -lwinhttp -o "%ROOT%DeepSeekTranslator.exe"
if errorlevel 1 exit /b 1

echo Built native server and launcher.
