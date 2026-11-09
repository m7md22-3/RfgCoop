@echo off
rem Builds build\xinput1_3.dll (32-bit) with llvm-mingw. Copy it (and RfgCoop.ini) next to rfg.exe to install; it runs
rem alongside other proxy-DLL mods (dinput8.dll, winmm.dll). Set CXX to your llvm-mingw i686 clang++ first, or
rem unpack llvm-mingw into tools\llvm-mingw.
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not defined CXX set CXX=tools\llvm-mingw\bin\i686-w64-mingw32-clang++.exe
set CC=%CXX:clang++=clang%
set EN=third_party\enet
set FLAGS=-O2 -DNDEBUG -Isrc -I%EN%\include
if not exist build mkdir build
rem ENet objects are cached; delete the build folder to rebuild them.
set OBJ=
for %%f in (%EN%\*.c) do (
    if not exist build\enet_%%~nf.o (
        echo enet: %%~nf
        "%CC%" %FLAGS% -c "%%f" -o build\enet_%%~nf.o || exit /b 1
    )
    set OBJ=!OBJ! build\enet_%%~nf.o
)
set SRC=
for %%f in (src\*.cpp) do set SRC=!SRC! "%%f"
"%CXX%" %FLAGS% -std=c++20 -Wall -Wno-unused-function -shared -static %SRC% %OBJ% src\xinput1_3.def -lws2_32 -lwinmm -lbcrypt -o build\xinput1_3.dll || exit /b 1
echo Built build\xinput1_3.dll
