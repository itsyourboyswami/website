@echo off
rem ---- 3P Compressor: build with MinGW-w64 (MSYS2 UCRT64, WinLibs, ...). Needs GCC 9 or newer.
rem      Everything is saved to build_log.txt and the window stays open at the end.
setlocal
cd /d "%~dp0"
set LOG=build_log.txt
echo 3P Compressor build > %LOG%
where g++ >> %LOG% 2>&1
if errorlevel 1 (
  echo g++ was not found. Install MinGW-w64 ^(for example from https://winlibs.com or MSYS2^) and add its "bin" folder to PATH.
  echo g++ was not found. >> %LOG%
  goto :fail
)
g++ --version >> %LOG% 2>&1
echo [1/3] resources...
windres app.rc -O coff -o app.res >> %LOG% 2>&1
if errorlevel 1 goto :fail
echo [2/3] 3P-Compressor.exe  ^(GUI^)...
g++ -std=c++17 -O3 -s -DNDEBUG -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DUNICODE -D_UNICODE -mwindows -municode -static -static-libgcc -static-libstdc++ -o 3P-Compressor.exe gui_win32.cpp app.res -lgdiplus -lcomdlg32 -lshell32 -lole32 -luuid -lgdi32 -luser32 -ladvapi32 >> %LOG% 2>&1
if errorlevel 1 goto :fail
echo [3/3] p3cli.exe  ^(command line^)...
g++ -std=c++17 -O3 -s -DNDEBUG -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -static -static-libgcc -static-libstdc++ -o p3cli.exe p3cli.cpp >> %LOG% 2>&1
if errorlevel 1 goto :fail
echo.
echo Done:  3P-Compressor.exe   p3cli.exe
pause
exit /b 0

:fail
echo.
echo ================= BUILD FAILED =================
echo The messages are saved in build_log.txt in this folder. First lines:
echo.
powershell -NoProfile -Command "Get-Content -LiteralPath '%LOG%' | Select-Object -First 40"
echo.
echo Please copy the text above ^(or open build_log.txt^) and send it to me.
pause
exit /b 1
