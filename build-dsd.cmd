@echo off
rem Builds dsd-neo (digital voice decoder) + mbelib-neo with MSVC into bin (see env.cmd for ARCH).
rem vcpkg deps: libsndfile[core] openssl. No terminal UI or audio devices; rtl_tcp input is on.
setlocal
call "%~dp0env.cmd" || exit /b 1
call "%VCVARS%" >nul || exit /b 1
set PKG_CONFIG_EXECUTABLE=
rem target processor stated explicitly: when cross-building, CMake would otherwise assume the
rem host's and dsd-neo would pick ARM NEON code for an x64 build
set COMMONCM=-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON ^
  -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=%CMPROC%

rem ---- mbelib-neo (AMBE/IMBE vocoders) ----
cmake -S "%ROOT%\src\mbelib-neo" -B "%BLD%\mbelib-neo" %COMMONCM% ^
  -DMBELIB_BUILD_TESTS=OFF -DMBELIB_BUILD_EXAMPLES=OFF -DMBELIB_WARNINGS_AS_ERRORS=OFF ^
  -DCMAKE_INSTALL_PREFIX=%BLDF%/mbelib-neo-install || exit /b 1
cmake --build "%BLD%\mbelib-neo" || exit /b 1
cmake --install "%BLD%\mbelib-neo" || exit /b 1

rem ---- dsd-neo ----
cmake -S "%ROOT%\src\dsd-neo" -B "%BLD%\dsd-neo" %COMMONCM% ^
  -DCMAKE_PREFIX_PATH="%BLDF%/mbelib-neo-install;%BLDF%/rtl-install;%VPF%" ^
  -DBUILD_TESTING=OFF -DDSD_WARNINGS_AS_ERRORS=OFF ^
  -DDSD_ENABLE_TERMINAL_UI=OFF -DDSD_AUDIO_BACKEND=none -DCOLORS=OFF -DCOLORSLOGS=OFF ^
  -DDSD_ENABLE_RTLSDR=ON -DDSD_ENABLE_AIRSPY=OFF -DDSD_ENABLE_SOAPYSDR=OFF ^
  -DCMAKE_DISABLE_FIND_PACKAGE_CODEC2=ON -DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON -DCMAKE_DISABLE_FIND_PACKAGE_EXPAT=ON ^
  -DCMAKE_INSTALL_PREFIX=%BLDF%/dsd-neo-install || exit /b 1
cmake --build "%BLD%\dsd-neo" || exit /b 1
cmake --install "%BLD%\dsd-neo" || exit /b 1

copy /y "%BLD%\dsd-neo-install\bin\dsd-neo.exe" "%BIN%\" >nul || exit /b 1
copy /y "%BLD%\mbelib-neo-install\bin\*.dll" "%BIN%\" >nul
copy /y "%VP%\bin\sndfile.dll" "%BIN%\" >nul
copy /y "%VP%\bin\libcrypto-3*.dll" "%BIN%\" >nul
echo BUILD OK
