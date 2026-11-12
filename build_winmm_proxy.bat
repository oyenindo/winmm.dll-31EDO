@echo off
rem ============================================================
rem  Build winmm_proxy.c  ->  winmm_x64.dll  dan  winmm_x86.dll
rem  Bisa dijalankan dari CMD biasa / double-click.
rem  Butuh Visual Studio atau "Build Tools for Visual Studio"
rem  dengan workload "Desktop development with C++".
rem ============================================================
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs

set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :novs

set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" goto :novs

where python >nul 2>nul
if not errorlevel 1 python gen_winmm_exports.py

call :build x64
call :build x86
echo.
echo Selesai. Salin winmm_x64.dll / winmm_x86.dll ke folder winmm_proxy di sebelah script Python.
pause
goto :eof

:build
setlocal
echo.
echo ===== Build %1 =====
call "%VCVARS%" %1 >nul
cl /nologo /O2 /LD winmm_proxy.c /Fe:winmm_%1.dll /link ws2_32.lib user32.lib kernel32.lib
if errorlevel 1 (echo BUILD %1 GAGAL) else (echo OK: winmm_%1.dll)
del /q winmm_proxy.obj winmm_%1.lib winmm_%1.exp 2>nul
endlocal
exit /b 0

:novs
echo.
echo Visual Studio / Build Tools (C++) tidak ditemukan.
echo.
echo Pilihan 1: pasang "Build Tools for Visual Studio" (gratis, dari Microsoft),
echo            centang workload "Desktop development with C++", lalu jalankan file ini lagi.
echo Pilihan 2: build lewat GitHub Actions memakai build-winmm-proxy.yml (tanpa pasang apa pun).
echo.
pause
exit /b 1
