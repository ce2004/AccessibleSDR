@echo off
rem Builds nrsc5 (HD Radio) with llvm-mingw into bin (see env.cmd for ARCH).
rem Links the vcpkg FFTW and our MSVC-built rtlsdr/libusb DLLs; libao is replaced by src\aoshim.
setlocal
call "%~dp0env.cmd" || exit /b 1
set "PATH=%ROOT%\llvm-mingw\bin;%PATH%"
set CC=%MINGW%-clang
if not exist "%BLD%\aoshim" mkdir "%BLD%\aoshim"
%CC% -O2 -c "%ROOT%\src\aoshim\ao.c" -I"%ROOT%\src\aoshim" -o "%BLD%\aoshim\ao.o" || exit /b 1
llvm-ar rcs "%BLD%\aoshim\libao.a" "%BLD%\aoshim\ao.o" || exit /b 1
cmake -S "%ROOT%\src\nrsc5" -B "%BLD%\nrsc5" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=%CC% -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=%CMPROC% ^
  -DCMAKE_MAKE_PROGRAM="%NINJAF%" -DUSE_SSE=OFF ^
  -DFFTW3F_LIBRARIES=%VPF%/lib/fftw3f.lib -DFFTW3F_INCLUDE_DIRS=%VPF%/include ^
  -DRTL_SDR_LIBRARIES=%BLDF%/rtl-install/lib/rtlsdr.lib -DRTL_SDR_INCLUDE_DIRS=%BLDF%/rtl-install/include ^
  -DLIBUSB_LIBRARIES=%VPF%/lib/libusb-1.0.lib ^
  -DAO_LIBRARIES="%BLDF%/aoshim/libao.a;winmm" -DAO_INCLUDE_DIRS=%ROOTF%/src/aoshim ^
  -DCMAKE_INSTALL_PREFIX=%BLDF%/nrsc5-install || exit /b 1
rem Ninja can't see faad2's output until the external project has run, so build it first
cmake --build "%BLD%\nrsc5" --target faad2_external || exit /b 1
cmake --build "%BLD%\nrsc5" || exit /b 1
cmake --install "%BLD%\nrsc5" || exit /b 1
copy /y "%BLD%\nrsc5-install\bin\*" "%BIN%\" >nul
copy /y "%VP%\bin\fftw3f.dll" "%BIN%\" >nul
rem the matching pthread runtime from llvm-mingw (a wrong-architecture copy gives 0xC000007B)
copy /y "%ROOT%\llvm-mingw\%MINGW%\bin\libwinpthread-1.dll" "%BIN%\" >nul
echo BUILD OK
