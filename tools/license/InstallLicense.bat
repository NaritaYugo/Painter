@echo off
REM ===========================================================================
REM InstallLicense.bat
REM ===========================================================================
REM Copies a Tiepolo Pro license file (*.tiepololicense) that sits next to this
REM script into the fixed location the app reads at startup:
REM   %APPDATA%\pwxwx\Tiepolo\license.tiepololicense
REM (see LicenseManager::defaultLicensePath() in src/backend/LicenseManager.cpp)
REM
REM Distribution: put the *.tiepololicense file issued by
REM tools/license/New-TiepoloLicense.ps1 together with this script in one
REM folder and hand that folder to the customer. They just double-click this
REM .bat file; no other setup is required.
REM
REM All console output below is plain ASCII on purpose. Non-ASCII (e.g.
REM Japanese) text in an active line of a .bat file -- even inside REM
REM comments -- has been observed to get corrupted by cmd.exe on this
REM environment's default (Shift-JIS) code page, regardless of a "chcp 65001"
REM switch, and can break batch parsing (a mangled byte gets misread as a
REM line break, splitting a comment into a stray "command"). Keeping this
REM script pure ASCII avoids that failure mode entirely, which matters more
REM here than localized wording since it must run unattended on a machine
REM we don't control.
REM ===========================================================================

setlocal

echo ============================================
echo  Tiepolo Pro License Installer
echo ============================================
echo.

set "SRC_DIR=%~dp0"
set "LICENSE_FILE="
set "LICENSE_COUNT=0"

for %%F in ("%SRC_DIR%*.tiepololicense") do (
    set "LICENSE_FILE=%%~fF"
    set /a LICENSE_COUNT+=1
)

if "%LICENSE_COUNT%"=="0" (
    echo [ERROR] No license file ^(*.tiepololicense^) found in this folder.
    echo Put this .bat file in the same folder as your license file and try again.
    goto :end
)

REM Deliberately not listing the individual matching filenames here. A nested
REM "for %%F in (...*.tiepololicense...) do echo ..." inside this if-block was
REM tried and observed to break "goto :end" in the EARLIER if-block above
REM (execution fell through into the copy section instead of stopping) --
REM reproducible only with this specific file pattern, so it looks like a
REM cmd.exe parser quirk rather than a logic bug. Keep this branch free of any
REM nested for-loop.
if not "%LICENSE_COUNT%"=="1" (
    echo [ERROR] Multiple license files found in this folder.
    echo Keep only one *.tiepololicense file in this folder and try again.
    goto :end
)

set "TARGET_DIR=%APPDATA%\pwxwx\Tiepolo"
set "TARGET_FILE=%TARGET_DIR%\license.tiepololicense"

if not exist "%TARGET_DIR%" (
    mkdir "%TARGET_DIR%" >nul 2>nul
)

if not exist "%TARGET_DIR%" (
    echo [ERROR] Could not create the install folder: %TARGET_DIR%
    goto :end
)

copy /Y "%LICENSE_FILE%" "%TARGET_FILE%" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy the license file.
    goto :end
)

echo License installed successfully.
echo   From: %LICENSE_FILE%
echo   To:   %TARGET_FILE%
echo.
echo If Tiepolo is currently running, restart it for the license to take effect.

:end
echo.
pause
endlocal
