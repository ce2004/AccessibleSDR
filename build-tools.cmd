@echo off
rem Builds tuner.exe (the app) and the small helper tools with MSVC into bin (see env.cmd for ARCH)
setlocal
call "%~dp0env.cmd" || exit /b 1
call "%VCVARS%" >nul || exit /b 1
set T=%ROOT%\src\tools
set O=/Fo"%BLD%\tools\\"
set RTL=/I"%BLD%\rtl-install\include" "%BLD%\rtl-install\lib\rtlsdr.lib"
if not exist "%BLD%\tools" mkdir "%BLD%\tools"
cl /nologo /O2 /W3 "%T%\play.c" %O% /Fe"%BIN%\play.exe" winmm.lib || exit /b 1
cl /nologo /O2 /fp:fast /W3 /D_CRT_SECURE_NO_WARNINGS "%T%\wfm.c" %O% /Fe"%BIN%\wfm.exe" %RTL% winmm.lib || exit /b 1
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS "%T%\iqcap.c" %O% /Fe"%BIN%\iqcap.exe" %RTL% || exit /b 1
cl /nologo /O2 /fp:fast /W3 /D_CRT_SECURE_NO_WARNINGS "%T%\tuner.c" /I"%ROOT%\src\nrsc5\include" %O% /Fe"%BIN%\tuner.exe" %RTL% ^
  winmm.lib user32.lib ws2_32.lib setupapi.lib newdev.lib winhttp.lib shell32.lib advapi32.lib /link /SUBSYSTEM:WINDOWS || exit /b 1
copy /y "%ROOT%\deps\nvdaControllerClient-%ARCH%.dll" "%BIN%\nvdaControllerClient.dll" >nul || exit /b 1
echo BUILD OK
