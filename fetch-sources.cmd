@echo off
rem Downloads everything the build needs (see tools\fetch-sources.py)
python "%~dp0tools\fetch-sources.py" || exit /b 1
