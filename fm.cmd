@echo off
rem Plays a broadcast FM station in stereo.  usage: fm 89.1   (Ctrl+C to stop)
if "%~1"=="" (echo usage: fm ^<MHz^>   for example: fm 89.1 & exit /b 1)
"%~dp0bin\wfm.exe" %*
