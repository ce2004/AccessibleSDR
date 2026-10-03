@echo off
rem Builds AIS-catcher with MSVC into bin (see env.cmd for ARCH). No SDR hardware backends:
rem the tuner owns the dongle and feeds cs16 IQ on stdin   (AIS-catcher.exe -r CS16 . -s 248062 -o 5)
setlocal
call "%~dp0env.cmd" || exit /b 1
call "%VCVARS%" >nul || exit /b 1
cmake -S "%ROOT%\src\AIS-catcher" -B "%BLD%\AIS-catcher" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DMSVC_VCPKG=ON ^
  -DRTLSDR=OFF -DAIRSPY=OFF -DSDRPLAY=OFF -DAIRSPYHF=OFF -DHACKRF=OFF -DHYDRASDR=OFF -DSOAPYSDR=OFF ^
  -DSOXR=OFF -DZLIB=OFF -DSAMPLERATE=OFF -DZMQ=OFF -DPSQL=OFF -DSQLITE=OFF -DOPENSSL=OFF -DNMEA2000=OFF ^
  -DWEBVIEWER=OFF -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded || exit /b 1
cmake --build "%BLD%\AIS-catcher" || exit /b 1
copy /y "%BLD%\AIS-catcher\AIS-catcher.exe" "%BIN%\" >nul || exit /b 1
echo BUILD OK
