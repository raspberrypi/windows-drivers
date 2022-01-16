@echo off
REM Export Script for exporting required binary files only
goto START

:Usage
echo Usage: binexport [RPi]/[RPi3.ARM64]/[RPi4.ARM64] [Release]/[Debug] [TargetDir]
echo    RPi....................... Export the Raspberry 2/3 32Bit BSP.
echo    RPi3.ARM64................ Export the Raspberry 3   64Bit BSP.
echo    RPi4.ARM64................ Export the Raspberry 4   64Bit BSP.
echo        One of the above should be specified
echo    Release................... Picks the release binaries
echo    Debug..................... Picks the debug binaries
echo        One of the above should be specified
echo    [TargetDir]............... Directory to export the files.
echo    [/?]...................... Displays this usage string.
echo    Example:
echo        binexport RPi Release C:\bsp_v1.0_release
echo        binexport RPi4.ARM64 Debug C:\bsp_v1.0_debug


exit /b 1

:START

REM Input validation
if [%1] == [/?] goto Usage
if [%1] == [-?] goto Usage
if [%1] == [] goto Usage
if [%1] == [] (
    goto Usage
) else (
    set PLAT=%1
)
if [%3] == [] (
    goto Usage
) else (
    set TDIR=%3
)

REM Detect architecture
set ARCH=INVALID
if /I [%PLAT%] == [RPi]        set ARCH=ARM
if /I [%PLAT%] == [RPi3.ARM64] set ARCH=ARM64
if /I [%PLAT%] == [RPi4.ARM64] set ARCH=ARM64
if /I [%ARCH%] == [INVALID] (
    goto Usage
)
set TDIR=%3\%PLAT%

pushd
setlocal ENABLEDELAYEDEXPANSION

REM
REM Set source root
REM
set REPO_BUILD_TOOL=%~dp0
cd /d "%REPO_BUILD_TOOL%.."
set REPO_SOURCE_ROOT=%cd%\

set OUTPUT_DIR=%REPO_SOURCE_ROOT%\build\bcm2836\%ARCH%
set BINTYPE=%2

REM Check firmware for ARM64 platform
if /I [%ARCH%] == [ARM64] (
    if not exist %REPO_SOURCE_ROOT%\bspfiles\%PLAT%\Packages\RPi.BootFirmware\RPI_EFI.fd (
        echo Error: UEFI Firmware for platform %PLAT% not found.
        echo Please refer to windows-drivers repo on how to get it.
        exit /b
    )
)

if not exist %TDIR% ( mkdir %TDIR% )
if not exist %OUTPUT_DIR%\%BINTYPE%\Output (
    echo %BINTYPE% directory not found. Do %BINTYPE% build
    goto usage
)
REM Copy the bspfiles
xcopy /E /I %REPO_SOURCE_ROOT%\bspfiles\%PLAT%\*.* %TDIR% >nul
set DRVDIR=%TDIR%\Packages\RPi.Drivers

REM Export the built binaries
copy %OUTPUT_DIR%\%BINTYPE%\Output\*.inf %DRVDIR% > nul
copy %OUTPUT_DIR%\%BINTYPE%\Output\*.sys %DRVDIR% > nul
copy %OUTPUT_DIR%\%BINTYPE%\Output\*.dll %DRVDIR% > nul

popd
exit /b


