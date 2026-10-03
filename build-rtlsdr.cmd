@echo off
rem Builds rtl-sdr-blog (librtlsdr + rtl_* tools) with MSVC into bin (see env.cmd for ARCH)
setlocal
call "%~dp0env.cmd" || exit /b 1
call "%VCVARS%" >nul || exit /b 1
cmake -S "%ROOT%\src\rtl-sdr-blog" -B "%BLD%\rtl-sdr-blog" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON ^
  -DLIBUSB_INCLUDE_DIRS="%VP%\include\libusb-1.0" -DLIBUSB_LIBRARIES="%VP%\lib\libusb-1.0.lib" ^
  -DTHREADS_PTHREADS_INCLUDE_DIR="%VP%\include" -DTHREADS_PTHREADS_LIBRARY="%VP%\lib\pthreadVC3.lib" ^
  -DCMAKE_INSTALL_PREFIX="%BLD%\rtl-install" || exit /b 1
cmake --build "%BLD%\rtl-sdr-blog" || exit /b 1
cmake --install "%BLD%\rtl-sdr-blog" || exit /b 1
copy /y "%BLD%\rtl-install\bin\*.exe" "%BIN%\" >nul
copy /y "%BLD%\rtl-install\bin\*.dll" "%BIN%\" >nul
copy /y "%VP%\bin\libusb-1.0.dll" "%BIN%\" >nul
copy /y "%VP%\bin\pthreadVC3.dll" "%BIN%\" >nul
echo BUILD OK
