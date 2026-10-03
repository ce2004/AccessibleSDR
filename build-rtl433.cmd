@echo off
rem Builds rtl_433 (wireless sensors) with MSVC into bin (see env.cmd for ARCH).
rem No RTL-SDR driver: the tuner owns the dongle and feeds rtl_433 IQ on stdin.
setlocal
call "%~dp0env.cmd" || exit /b 1
call "%VCVARS%" >nul || exit /b 1
cmake -S "%ROOT%\src\rtl_433" -B "%BLD%\rtl_433" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DENABLE_RTLSDR=OFF -DENABLE_SOAPYSDR=OFF ^
  -DENABLE_OPENSSL=OFF -DBUILD_TESTING=OFF -DBUILD_DOCUMENTATION=OFF -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON || exit /b 1
cmake --build "%BLD%\rtl_433" || exit /b 1
copy /y "%BLD%\rtl_433\src\rtl_433.exe" "%BIN%\" >nul || exit /b 1
echo BUILD OK
