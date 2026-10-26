@echo off
chcp 65001 >nul 2>&1
rem 新机器一键初始化：拉 vcpkg + 预编译依赖（GitHub deps release）+ 编译
rem 已有本地 vcpkg-bincache 的机器不需要跑这个，直接 build-app.bat
setlocal enabledelayedexpansion
set "ProgramFiles(x86)=C:\Program Files (x86)"
set "ProgramFiles=C:\Program Files"
set "ProgramW6432=C:\Program Files"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul || exit /b 1
set "VCPKG_ROOT="
cd /d "%~dp0"

if not exist .vcpkg (echo [setup] cloning vcpkg... & git clone --depth 1 https://github.com/microsoft/vcpkg.git .vcpkg || exit /b 1)
if not exist .vcpkg\vcpkg.exe (.vcpkg\bootstrap-vcpkg.bat -disableMetrics || exit /b 1)

if exist vcpkg-bincache goto localCache
echo [setup] downloading prebuilt deps from Ellenp2p/tgbot deps release...
set "BASE=https://github.com/Ellenp2p/tgbot/releases/download/deps"
curl -fsSL -o bincache.tar.gz "%BASE%/vcpkg-bincache-x64-windows-static.tar.gz" && curl -fsSL -o vcpkg-commit.txt "%BASE%/vcpkg-commit.txt" && tar xzf bincache.tar.gz
if errorlevel 1 goto noPrebuilt
if not exist vcpkg-commit.txt goto noPrebuilt
set /p sha=<vcpkg-commit.txt
echo [setup] pinning vcpkg to !sha! (must match bincache ABI)...
git -C .vcpkg fetch --depth 1 origin !sha! && git -C .vcpkg checkout --detach !sha!
goto install
:noPrebuilt
echo [setup] WARNING: prebuilt deps unavailable, compiling all dependencies locally (slow first time)
goto install
:localCache
echo [setup] local vcpkg-bincache found, skipping download (keep local vcpkg commit)
:install

set "VCPKG_BINARY_SOURCES=files,%~dp0vcpkg-bincache,readwrite"
echo [setup] vcpkg install...
.vcpkg\vcpkg.exe install --triplet=x64-windows-static || exit /b 1

echo [setup] building...
cmake -S "%~dp0." -B "%~dp0build" -DCMAKE_TOOLCHAIN_FILE="%~dp0.vcpkg\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%~dp0build" --config Release || exit /b 1
echo [setup] done. binaries in build\Release\
