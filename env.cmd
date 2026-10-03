@echo off
rem Shared build settings, called by every build-*.cmd after setlocal.
rem   set ARCH=arm64  (default)  or  set ARCH=x64   before building.
rem ARM64 builds go to bin\ and build\; x64 builds to bin-x64\ and build-x64\.
if "%ARCH%"=="" set ARCH=arm64
set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%
set ROOTF=%ROOT:\=/%
if /i "%ARCH%"=="arm64" (
  set TRIPLET=arm64-windows& set MINGW=aarch64-w64-mingw32& set CMPROC=ARM64& set SUF=
) else if /i "%ARCH%"=="x64" (
  set TRIPLET=x64-windows& set MINGW=x86_64-w64-mingw32& set CMPROC=AMD64& set SUF=-x64
) else (echo ARCH must be arm64 or x64 & exit /b 1)
set BIN=%ROOT%\bin%SUF%
set BLD=%ROOT%\build%SUF%
set BLDF=%ROOTF%/build%SUF%
set VP=%ROOT%\vcpkg\installed\%TRIPLET%
set VPF=%ROOTF%/vcpkg/installed/%TRIPLET%
if not exist "%BIN%" mkdir "%BIN%"
if not exist "%BLD%" mkdir "%BLD%"
rem Visual Studio (Build Tools): compiler environment, plus its own CMake and Ninja
set VSDIR=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" (echo Visual Studio Build Tools not found & exit /b 1)
set HOSTARCH=%PROCESSOR_ARCHITECTURE%
if /i "%PROCESSOR_ARCHITEW6432%"=="ARM64" set HOSTARCH=ARM64
if /i "%HOSTARCH%"=="ARM64" (
  if /i "%ARCH%"=="arm64" (set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvarsarm64.bat") else (set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvarsarm64_amd64.bat")
) else (
  if /i "%ARCH%"=="arm64" (set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvarsamd64_arm64.bat") else (set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars64.bat")
)
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "NINJA=%VSCMAKE%\Ninja\ninja.exe"
set "NINJAF=%NINJA:\=/%"
rem a clean PATH: no stray x64 gcc, pkg-config or DLLs from other tools
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\WindowsPowerShell\v1.0;C:\Program Files\Git\cmd;C:\Program Files\Git\usr\bin"
exit /b 0
