@echo off
chcp 65001 >nul 2>&1
rem Git Bash does not inherit ProgramFiles vars; vcpkg crashes without them
set "ProgramFiles(x86)=C:\Program Files (x86)"
set "ProgramFiles=C:\Program Files"
set "ProgramW6432=C:\Program Files"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul || exit /b 1
rem VsDevCmd points VCPKG_ROOT at the VS-bundled vcpkg; clear it
set "VCPKG_ROOT="
set "VCPKG_BINARY_SOURCES=files,%~dp0vcpkg-bincache,readwrite"
"%~dp0.vcpkg\vcpkg.exe" install --triplet=x64-windows-static
