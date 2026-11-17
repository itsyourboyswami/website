@echo off
setlocal
cd /d "%~dp0"
where g++ >nul 2>&1
if errorlevel 1 (
  echo g++ was not found. Install MinGW-w64 or MSYS2 UCRT64 and add its bin folder to PATH.
  exit /b 1
)
echo Building the 3P web compressor for Windows...
g++ -std=c++17 -O3 -s -DNDEBUG -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -static -static-libgcc -static-libstdc++ -o p3cli.exe p3cli.cpp
if errorlevel 1 (
  echo Build failed. Check the compiler output above.
  exit /b 1
)
echo Done. The website can now use p3cli.exe.
