@echo off
rem Builds ft8_lib's decode_ft8 with llvm-mingw into bin (see env.cmd for ARCH). Static, no DLLs.
rem   decode_ft8.exe slot.wav        (15 s, 12000 Hz mono 16-bit; add -ft4 for FT4)
setlocal
call "%~dp0env.cmd" || exit /b 1
set "PATH=%ROOT%\llvm-mingw\bin;%PATH%"
set SRC=%ROOT%\src\ft8_lib
if not exist "%BLD%\ft8_lib" mkdir "%BLD%\ft8_lib"
%MINGW%-clang -O3 -I"%SRC%" ^
  "%SRC%\demo\decode_ft8.c" ^
  "%SRC%\ft8\constants.c" "%SRC%\ft8\crc.c" "%SRC%\ft8\decode.c" "%SRC%\ft8\encode.c" "%SRC%\ft8\ldpc.c" ^
  "%SRC%\ft8\message.c" "%SRC%\ft8\text.c" ^
  "%SRC%\common\audio.c" "%SRC%\common\monitor.c" "%SRC%\common\wave.c" ^
  "%SRC%\fft\kiss_fft.c" "%SRC%\fft\kiss_fftr.c" ^
  -static -lpthread -lm -o "%BLD%\ft8_lib\decode_ft8.exe" || exit /b 1
copy /y "%BLD%\ft8_lib\decode_ft8.exe" "%BIN%\" >nul || exit /b 1
echo BUILD OK
