@echo off
rem SBPC: rebuild (Visual Studio preset), run the tests, start the launcher.
rem Full output goes to rebuild.log next to this file, so Claude can read it.
setlocal
set "ROOT=%~dp0"
set "LOG=%ROOT%rebuild.log"
echo SBPC rebuild %DATE% %TIME% > "%LOG%"

rem ---- find the Visual Studio tools (cmake/ctest are not on PATH by default) ----
where cmake >nul 2>nul
if not errorlevel 1 goto have_tools
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_tools
"%VSWHERE%" -latest -products * -property installationPath > "%TEMP%\sbpc_vsdir.txt"
set "VSDIR="
set /p VSDIR=<"%TEMP%\sbpc_vsdir.txt"
if not defined VSDIR goto no_tools
echo Using Visual Studio at %VSDIR%
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
where cmake >nul 2>nul
if errorlevel 1 goto no_tools

:have_tools
cd /d "%ROOT%program"

echo [1/3] Configuring...
echo ==== configure ==== >> "%LOG%"
cmake --preset windows-msvc >> "%LOG%" 2>&1
if errorlevel 1 goto failed

echo [2/3] Building (first build takes a few minutes)...
echo ==== build ==== >> "%LOG%"
cmake --build --preset windows-msvc >> "%LOG%" 2>&1
if errorlevel 1 goto failed

echo [3/3] Running tests...
echo ==== tests ==== >> "%LOG%"
ctest --preset windows-msvc >> "%LOG%" 2>&1
if errorlevel 1 goto failed

echo RESULT: OK>> "%LOG%"
echo.
echo All good. Starting the launcher...
start "" "%ROOT%builds\sbpc.exe"
timeout /t 5 >nul
exit /b 0

:no_tools
echo RESULT: FAILED (no cmake / Visual Studio found)>> "%LOG%"
echo Could not find CMake or Visual Studio Build Tools.
pause
exit /b 1

:failed
echo RESULT: FAILED>> "%LOG%"
echo.
echo ---- last lines of rebuild.log ----
powershell -NoProfile -Command "Get-Content -Tail 30 -LiteralPath '%LOG%'"
echo.
echo FAILED - tell Claude, it can read rebuild.log.
pause
exit /b 1
