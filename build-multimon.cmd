@echo off
rem Builds multimon-ng (pagers, EAS/SAME, APRS, DTMF...) with llvm-mingw into bin (see env.cmd for ARCH)
setlocal
call "%~dp0env.cmd" || exit /b 1
set "PATH=%ROOT%\llvm-mingw\bin;%PATH%"
cmake -S "%ROOT%\src\multimon-ng" -B "%BLD%\multimon-ng" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=%MINGW%-clang -DCMAKE_CXX_COMPILER=%MINGW%-clang++ -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=%CMPROC% ^
  -DCMAKE_MAKE_PROGRAM="%NINJAF%" -DBUILD_GEN_NG=OFF -DENABLE_HARDENING=OFF ^
  -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON || exit /b 1
cmake --build "%BLD%\multimon-ng" || exit /b 1
copy /y "%BLD%\multimon-ng\multimon-ng.exe" "%BIN%\" >nul || exit /b 1
echo BUILD OK
