@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>&1
if errorlevel 1 (
  echo cl.exe was not found. Open the x64 Native Tools Command Prompt for Visual Studio or Build Tools, then run this script there.
  exit /b 1
)
echo Building the 3P web compressor with Microsoft C++...
cl /nologo /std:c++17 /O2 /EHsc /DNDEBUG /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 p3cli.cpp /Fe:p3cli.exe
if errorlevel 1 (
  echo Build failed. Check the compiler output above.
  exit /b 1
)
echo Done. The website can now use p3cli.exe.
