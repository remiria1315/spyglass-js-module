@echo off
setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=relwithdebinfo-windows"
set "LOG=build\%PRESET%.log"

where clang-cl >nul 2>nul && goto toolchain_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
:toolchain_ready

if not exist build mkdir build
if exist "build\%PRESET%" goto configured
if "%CLIENTS%"=="" set CLIENTS=1.26.60
cmake --preset %PRESET% -DMINECRAFT_CLIENTS=%CLIENTS% > "%LOG%" 2>&1
if errorlevel 1 goto failed
:configured
cmake --build --preset %PRESET% >> "%LOG%" 2>&1
if errorlevel 1 goto failed

powershell -NoProfile -Command "Get-Content -Tail 20 '%LOG%'"
echo.
echo built: build\%PRESET%
exit /b 0

:failed
powershell -NoProfile -Command "Select-String -Path '%LOG%' -Pattern 'error' | Select-Object -First 10 | ForEach-Object { $_.Line }"
echo.
powershell -NoProfile -Command "Get-Content -Tail 20 '%LOG%'"
echo.
echo failed, full log: %LOG%
exit /b 1
