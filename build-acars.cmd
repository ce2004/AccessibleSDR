@echo off
rem Builds acarsdec (f00b4r0 fork) with llvm-mingw into bin (see env.cmd for ARCH). Static, no DLLs.
rem SDR local change adds --stdin (raw s16le mono AM audio):  acarsdec.exe --stdin --output json:file
setlocal
call "%~dp0env.cmd" || exit /b 1
set "PATH=%ROOT%\llvm-mingw\bin;%PATH%"
set SRC=%ROOT%\src\acarsdec
if not exist "%BLD%\acarsdec" mkdir "%BLD%\acarsdec"
%MINGW%-clang -O3 -ffast-math -Wall -Wno-unused-function -std=gnu11 ^
  -DWITH_STDIN -DHAVE_CJSON -D_POSIX_THREAD_SAFE_FUNCTIONS -DVERSION="\"4.6-sdr\"" ^
  -I"%SRC%\win32" -I"%ROOT%\src\cJSON" -include "%SRC%\win32\sdr_compat.h" ^
  "%SRC%\acars.c" "%SRC%\acarsdec.c" "%SRC%\label.c" "%SRC%\msk.c" "%SRC%\output.c" "%SRC%\netout.c" ^
  "%SRC%\fileout.c" "%SRC%\lib.c" "%SRC%\statsd.c" "%SRC%\rawstdin.c" "%SRC%\win32\sdr_compat.c" ^
  "%ROOT%\src\cJSON\cJSON.c" ^
  -static -lws2_32 -lpthread -lm -o "%BLD%\acarsdec\acarsdec.exe" || exit /b 1
copy /y "%BLD%\acarsdec\acarsdec.exe" "%BIN%\" >nul || exit /b 1
echo BUILD OK
