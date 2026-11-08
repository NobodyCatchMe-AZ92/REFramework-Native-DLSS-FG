@echo off
rem Builds build\Release\refw_framegen.dll (Visual Studio 2022, x64).
set NoDefaultCurrentDirectoryInExePath=
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build cmake -S . -B build -G "Visual Studio 17 2022" -A x64 || exit /b 1
cmake --build build --config Release -- -m -v:minimal
