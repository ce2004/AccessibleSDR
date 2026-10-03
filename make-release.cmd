@echo off
rem Packs the app for other people: release\AccessibleSDR-<version>-win-<arch>.zip (see env.cmd for ARCH)
rem Only what tuner.exe needs at run time. Unzip anywhere and run tuner.exe.
setlocal
call "%~dp0env.cmd" || exit /b 1
for /f "tokens=3" %%v in ('findstr /c:"#define TUNER_VERSION" "%ROOT%\src\tools\tuner.c"') do set VER=%%~v
if "%VER%"=="" (echo could not read TUNER_VERSION & exit /b 1)
set OUT=%ROOT%\release\%ARCH%\AccessibleSDR
if exist "%ROOT%\release\%ARCH%" rmdir /s /q "%ROOT%\release\%ARCH%"
mkdir "%OUT%" || exit /b 1
for %%F in (tuner.exe rtlsdr.dll libusb-1.0.dll pthreadVC3.dll libnrsc5.dll fftw3f.dll libwinpthread-1.dll nvdaControllerClient.dll ^
            multimon-ng.exe dsd-neo.exe mbe-neo.dll sndfile.dll rtl_433.exe AIS-catcher.exe acarsdec.exe decode_ft8.exe) do (
  copy /y "%BIN%\%%F" "%OUT%\" >nul || (echo missing %BIN%\%%F & exit /b 1)
)
copy /y "%BIN%\libcrypto-3*.dll" "%OUT%\" >nul || exit /b 1
rem Microsoft C++ runtime, app-local (redistributable)
set CRT=
for /d %%D in ("%VSDIR%\VC\Redist\MSVC\*") do for /d %%C in ("%%D\%ARCH%\Microsoft.VC*.CRT") do set "CRT=%%C"
if "%CRT%"=="" (echo C++ runtime redistributable not found & exit /b 1)
copy /y "%CRT%\vcruntime140.dll" "%OUT%\" >nul || exit /b 1
copy /y "%CRT%\msvcp140.dll" "%OUT%\" >nul || exit /b 1
if exist "%CRT%\vcruntime140_1.dll" copy /y "%CRT%\vcruntime140_1.dll" "%OUT%\" >nul
for %%F in (presets.txt zipcodes.txt counties.txt) do copy /y "%ROOT%\%%F" "%OUT%\" >nul || exit /b 1
copy /y "%ROOT%\README.md" "%OUT%\README.txt" >nul || exit /b 1
set ZIP=%ROOT%\release\AccessibleSDR-%VER%-win-%ARCH%.zip
if exist "%ZIP%" del "%ZIP%"
tar -a -cf "%ZIP%" -C "%OUT%" . || exit /b 1
echo RELEASE OK %ZIP%
