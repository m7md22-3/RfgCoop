@echo off
rem Builds build\xinput1_3.dll (32-bit) with llvm-mingw. Copy it (and RfgCoop.ini) next to rfg.exe to install; it runs
rem alongside other proxy-DLL mods (dinput8.dll, winmm.dll).
rem Compiler: unzip llvm-mingw into the tools folder here (any folder whose name starts with llvm-mingw), or set CXX to
rem llvm-mingw's i686-w64-mingw32-clang++.exe.
setlocal enabledelayedexpansion
cd /d "%~dp0"
rem Started by double-click (cmd /c with this script's name): keep the window open at the end.
set DBLCLICK=
echo !cmdcmdline! | findstr /i /c:"%~nx0" >nul && set DBLCLICK=1
if not defined CXX for /d %%d in (tools\llvm-mingw*) do if exist "%%d\bin\i686-w64-mingw32-clang++.exe" set CXX=%%d\bin\i686-w64-mingw32-clang++.exe
if not defined CXX goto nocxx
if exist "%CXX%" goto build
where "%CXX%" >nul 2>&1 && goto build
:nocxx
echo Compiler not found. Get llvm-mingw from https://github.com/mstorsjo/llvm-mingw/releases (a ucrt-x86_64 zip)
echo and unzip it into the tools folder next to build.cmd, so that
echo   tools\llvm-mingw-...\bin\i686-w64-mingw32-clang++.exe
echo exists. Or set CXX to the full path of that exe. Then run build.cmd again.
if defined CXX echo CXX is set to "%CXX%", which was not found.
goto fail
:build
echo Compiler: %CXX%
set CC=%CXX:clang++=clang%
set EN=third_party\enet
set FLAGS=-O2 -DNDEBUG -Isrc -I%EN%\include
if not exist build mkdir build
rem ENet objects are cached; delete the build folder to rebuild them.
set OBJ=
for %%f in (%EN%\*.c) do (
    if not exist build\enet_%%~nf.o (
        echo enet: %%~nf
        "%CC%" %FLAGS% -c "%%f" -o build\enet_%%~nf.o || goto fail
    )
    set OBJ=!OBJ! build\enet_%%~nf.o
)
set SRC=
for %%f in (src\*.cpp) do set SRC=!SRC! "%%f"
echo RfgCoop: compiling src\*.cpp
"%CXX%" %FLAGS% -std=c++20 -Wall -Wno-unused-function -shared -static %SRC% %OBJ% src\xinput1_3.def -lws2_32 -lwinmm -lbcrypt -o build\xinput1_3.dll || goto fail
echo Built build\xinput1_3.dll
if defined DBLCLICK pause
exit /b 0
:fail
echo Build FAILED.
if defined DBLCLICK pause
exit /b 1
