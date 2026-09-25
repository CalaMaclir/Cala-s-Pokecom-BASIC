@echo off
setlocal EnableExtensions

rem Cala's Pokecom BASIC - Pico 2 W one-click firmware updater
rem Usage:
rem   flash-cpb.cmd
rem   flash-cpb.cmd path\to\CPokecombasic.uf2
rem
rem Requirements:
rem   picotool.exe must be available on PATH.
rem   The installed CPB firmware must include the CPB Firmware Update USB
rem   interface. Installing this feature for the first time from an older CPB
rem   build still requires the physical BOOTSEL method once.

where picotool.exe >nul 2>nul
if errorlevel 1 (
    echo.
    echo ERROR: picotool.exe was not found on PATH.
    echo Install Raspberry Pi picotool and add it to PATH, then retry.
    echo.
    pause
    exit /b 2
)

if not "%~1"=="" (
    set "UF2=%~f1"
) else (
    set "UF2=%~dp0build\CPokecombasic.uf2"
)

if not exist "%UF2%" (
    echo.
    echo ERROR: UF2 file was not found:
    echo   "%UF2%"
    echo.
    echo Drag a CPokecombasic.uf2 file onto this script,
    echo or keep the script beside the build folder from the CI artifact.
    echo.
    pause
    exit /b 3
)

echo.
echo Cala's Pokecom BASIC Firmware Update
echo UF2: "%UF2%"
echo.
echo Do not disconnect USB while flashing.
echo USB Storage mode should be returned to CPB before updating.
echo.

rem CPB application-mode USB identity. -f asks the compatible running firmware
rem to enter BOOTSEL, flashes the UF2, verifies it, then returns to application.
picotool load "%UF2%" -v -f --vid 0xcafe --pid 0x4003
if not errorlevel 1 goto success

echo.
echo CPB application-mode reset was not available.
echo Trying a device that is already in BOOTSEL mode...
echo.
picotool load "%UF2%" -v
if errorlevel 1 goto fail

:success
echo.
echo SUCCESS: firmware programmed and verified.
echo The PicoCalc should reboot into Cala's Pokecom BASIC.
echo.
pause
exit /b 0

:fail
echo.
echo ERROR: firmware update failed.
echo If the installed CPB already has the Firmware menu, use:
echo   Control Center ^> Firmware ^> Enter BOOTSEL
echo then run this script again.
echo.
echo If this is the first update from an older CPB build, use the physical
echo BOOTSEL/RESET USB procedure once. Future updates can be one-click.
echo.
pause
exit /b 1
