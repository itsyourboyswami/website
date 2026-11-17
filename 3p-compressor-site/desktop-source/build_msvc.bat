@echo off
rem ---- 3P Compressor: build with Visual Studio. Run from "x64 Native Tools Command Prompt".
setlocal
cd /d "%~dp0"
set LOG=build_log.txt
echo 3P Compressor build ^(MSVC^) > %LOG%
rc /nologo app.rc >> %LOG% 2>&1
if errorlevel 1 goto :fail
cl /nologo /std:c++17 /O2 /EHsc /DNDEBUG /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DUNICODE /D_UNICODE gui_win32.cpp app.res /Fe:3P-Compressor.exe /link /SUBSYSTEM:WINDOWS gdiplus.lib comdlg32.lib shell32.lib user32.lib gdi32.lib ole32.lib advapi32.lib >> %LOG% 2>&1
if errorlevel 1 goto :fail
cl /nologo /std:c++17 /O2 /EHsc /DNDEBUG /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 p3cli.cpp /Fe:p3cli.exe >> %LOG% 2>&1
if errorlevel 1 goto :fail
echo Done.
pause
exit /b 0
:fail
echo BUILD FAILED. First lines of build_log.txt:
powershell -NoProfile -Command "Get-Content -LiteralPath '%LOG%' | Select-Object -First 40"
pause
exit /b 1
