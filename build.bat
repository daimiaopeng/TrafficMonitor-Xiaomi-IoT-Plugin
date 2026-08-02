@echo off
chcp 65001 >nul
setlocal EnableExtensions
echo ============================================================
echo [Build] Compiling XiaomiIoTPlugin.dll (x64 Native C++)
echo ============================================================

rem A very long user PATH makes vcvars64.bat exceed cmd.exe's 8191-char
rem command-line limit.  The compiler environment only needs a small Windows
rem PATH before vcvars64 adds the Visual Studio tool directories.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem"
call "D:\app\Microsoft Visual Studio\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo [Build] FAILED! Visual Studio x64 compiler environment was not loaded.
    exit /b 1
)
cd /d "%~dp0"

cl /utf-8 /std:c++20 /EHsc /LD XiaomiIoTPlugin.cpp /I. /FeXiaomiIoTPlugin.dll /link onecore.lib wininet.lib

if errorlevel 1 (
    echo [Build] FAILED! Compilation or linking failed.
    exit /b 1
)

if exist XiaomiIoTPlugin.dll (
    echo ============================================================
    echo [Build] SUCCESS! XiaomiIoTPlugin.dll created.
    echo ============================================================

    echo [Deploy] Copying to TrafficMonitor plugins directory...
    if not exist "C:\Users\daimi\Documents\TrafficMonitor\plugins" (
        mkdir "C:\Users\daimi\Documents\TrafficMonitor\plugins"
    )
    copy /Y XiaomiIoTPlugin.dll "C:\Users\daimi\Documents\TrafficMonitor\plugins\XiaomiIoTPlugin.dll"
    if errorlevel 1 (
        echo [Deploy] FAILED! The destination DLL may be locked by TrafficMonitor.
        exit /b 1
    )

    if exist ".env" (
        copy /Y ".env" "C:\Users\daimi\Documents\TrafficMonitor\plugins\.env" >nul
        echo [Deploy] Local .env copied to the plugin configuration directory.
    )

    echo ============================================================
    echo [Deploy] Deployed to C:\Users\daimi\Documents\TrafficMonitor\plugins\XiaomiIoTPlugin.dll
    echo ============================================================
) else (
    echo [Build] FAILED! Check compiler output above.
)
