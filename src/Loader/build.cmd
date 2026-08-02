@echo off
setlocal

set "ROOT=%~dp0..\.."
set "LD=src\Loader"
set "OUT=%ROOT%\src\Loader\out"
set "MH=src\Loader\third_party\minhook"

set "CC=C:\devkitPro\msys2\opt\bin\x86_64-w64-mingw32-gcc.exe"
set "AR=C:\devkitPro\msys2\opt\bin\x86_64-w64-mingw32-ar.exe"
set "WR=C:\devkitPro\msys2\opt\bin\x86_64-w64-mingw32-windres.exe"

if not exist "%OUT%" mkdir "%OUT%"

echo === Building MinHook static lib ===
"%CC%" -Isrc -Iinclude -masm=intel -O2 -Wall -c "%MH%\src\buffer.c"      -o "%OUT%\buffer.o"      || goto :err
"%CC%" -Isrc -Iinclude -masm=intel -O2 -Wall -c "%MH%\src\hook.c"        -o "%OUT%\hook.o"        || goto :err
"%CC%" -Isrc -Iinclude -masm=intel -O2 -Wall -c "%MH%\src\trampoline.c"  -o "%OUT%\trampoline.o"  || goto :err
"%CC%" -Isrc -Iinclude -masm=intel -O2 -Wall -c "%MH%\src\hde\hde32.c"   -o "%OUT%\hde32.o"       || goto :err
"%CC%" -Isrc -Iinclude -masm=intel -O2 -Wall -c "%MH%\src\hde\hde64.c"   -o "%OUT%\hde64.o"       || goto :err
"%AR%" rcs "%OUT%\libMinHook.a" "%OUT%\buffer.o" "%OUT%\hook.o" "%OUT%\trampoline.o" "%OUT%\hde32.o" "%OUT%\hde64.o" || goto :err

echo === Building dinput8.dll ===
"%CC%" -O2 -Wall -I"%MH%\include" -shared -static-libgcc -s ^
    -o "%OUT%\dinput8.dll" ^
    "%LD%\loader.c" "%OUT%\libMinHook.a" "%LD%\dinput8.def" || goto :err

echo === Building UMVC3ModManager.asi ===
"%WR%" "%LD%\manager.rc" -o "%OUT%\manager_res.o" || goto :err
"%CC%" -O2 -Wall -shared -static-libgcc -s ^
    -o "%OUT%\UMVC3ModManager.asi" ^
    "%LD%\manager.c" "%OUT%\manager_res.o" -lcomctl32 -lgdi32 || goto :err

echo.
echo === Checking exports ===
"%CC%" -shared -o "%OUT%\check.dll" "%LD%\dinput8.def" -Wl,--output-def=- 2>nul | findstr /i "DirectInput8Create GetdfDIJoystick" || echo (check skipped)

echo.
echo DONE. Output: %OUT%\dinput8.dll + UMVC3ModManager.asi
exit /b 0

:err
echo BUILD FAILED
exit /b 1
