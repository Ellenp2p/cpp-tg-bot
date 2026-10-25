@echo off
chcp 65001 >nul 2>&1
rem Git Bash does not inherit ProgramFiles vars
set "ProgramFiles(x86)=C:\Program Files (x86)"
set "ProgramFiles=C:\Program Files"
set "ProgramW6432=C:\Program Files"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul || exit /b 1
set "VCPKG_ROOT="
set "VCPKG_BINARY_SOURCES=files,%~dp0vcpkg-bincache,readwrite"
cmake -S "%~dp0." -B "%~dp0build" -DCMAKE_TOOLCHAIN_FILE="%~dp0.vcpkg\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%~dp0build" --config Release || exit /b 1
