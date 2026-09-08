@echo off
rem build_managed_payloads.bat -- first-party managed (C#) payload builds.
rem Called by build_native.bat with ROOT already set (trailing backslash).
rem This file is the *recipe* fingerprinted by scripts\verify_build_artifacts.ps1
rem for payloads\ManagedBuildStamps: changing it invalidates every managed payload
rem stamp, so launcher/server build changes belong in build_native.bat instead.
setlocal EnableDelayedExpansion
if not defined ROOT set "ROOT=%~dp0..\"

set "XUT=%ROOT%payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src\DeepSeekXUnityTranslator.csproj"
set "XUT_CORE=%ROOT%payloads\UnityIL2CPP\XUnityAutoTranslator\BepInEx\plugins\XUnity.AutoTranslator\XUnity.AutoTranslator.Plugin.Core.dll"
if exist "%XUT%" (
    if exist "%XUT_CORE%" (
        where dotnet >nul 2>nul
        if errorlevel 1 (
            echo Missing dotnet SDK needed to build DeepSeek XUnity translator endpoint.
            exit /b 1
        )
        dotnet build "%XUT%" -c Release --nologo --no-incremental
        if errorlevel 1 exit /b 1
        powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\verify_build_artifacts.ps1" -RecordManagedPayload UnityXUnity
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
        dotnet build "%TMPF%" -c Release --nologo --no-incremental -p:BepInExCoreDir="!TMPF_BEP!" -p:UnityInteropDir="!TMPF_INTEROP!"
        if errorlevel 1 exit /b 1
        powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\verify_build_artifacts.ps1" -RecordManagedPayload UnityTmpFallback
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

        dotnet build "%UT%" -c Release --nologo --no-incremental -p:UnityManagedDir="!UT_MANAGED!" -p:BepInExFlavor=5
        if errorlevel 1 exit /b 1
        if not exist "%ROOT%payloads\UnityTranslator\src\bin\Release\net472\UnityTranslator.dll" (
            echo UnityTranslator build succeeded but output DLL was not found.
            exit /b 1
        )
        copy /Y "%ROOT%payloads\UnityTranslator\src\bin\Release\net472\UnityTranslator.dll" "%ROOT%payloads\UnityTranslator\UnityTranslator.dll" >nul

        set "UT_BEPINEX6_OUT=%ROOT%payloads\UnityTranslator\src\bin\Release\net472-bepinex6"
        dotnet build "%UT%" -c Release --nologo --no-incremental -p:UnityManagedDir="!UT_MANAGED!" -p:BepInExFlavor=6 -o "!UT_BEPINEX6_OUT!"
        if errorlevel 1 exit /b 1
        if not exist "!UT_BEPINEX6_OUT!\UnityTranslator.dll" (
            echo UnityTranslator BepInEx6 build succeeded but output DLL was not found.
            exit /b 1
        )
        copy /Y "!UT_BEPINEX6_OUT!\UnityTranslator.dll" "%ROOT%payloads\UnityTranslator\UnityTranslator.BepInEx6.dll" >nul
        powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\verify_build_artifacts.ps1" -RecordManagedPayload UnityMono5
        if errorlevel 1 exit /b 1
        powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\verify_build_artifacts.ps1" -RecordManagedPayload UnityMono6
        if errorlevel 1 exit /b 1
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
        dotnet build "%UT_FONT_PATCHER_PROJECT%" -c Release --nologo --no-incremental -p:BepInEx5CoreDir="%UT_BEPINEX5_CORE%"
        if errorlevel 1 exit /b 1
        if not exist "%ROOT%payloads\UnityTranslator\src\FontPatcher\bin\Release\net472\DeepSeekUnityFontPatcher.dll" (
            echo DeepSeekUnityFontPatcher build succeeded but output DLL was not found.
            exit /b 1
        )
        copy /Y "%ROOT%payloads\UnityTranslator\src\FontPatcher\bin\Release\net472\DeepSeekUnityFontPatcher.dll" "%UT_FONT_PATCHER_PAYLOAD%" >nul
        powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\verify_build_artifacts.ps1" -RecordManagedPayload UnityFontPatcher
        if errorlevel 1 exit /b 1
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

endlocal
exit /b 0
