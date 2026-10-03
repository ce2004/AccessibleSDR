@echo off
rem Builds every part of AccessibleSDR into bin\ (run fetch-sources.cmd first on a fresh checkout)
setlocal
set ROOT=%~dp0
if not exist "%ROOT%bin" mkdir "%ROOT%bin"
for %%S in (build-rtlsdr build-nrsc5 build-multimon build-rtl433 build-dsd build-ais build-acars build-ft8 build-tools) do (
  echo ===== %%S
  call "%ROOT%%%S.cmd" || (echo %%S FAILED & exit /b 1)
)
echo ALL BUILT
