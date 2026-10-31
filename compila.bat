@echo off
rem Compila Iris (Release) con Visual Studio / Build Tools 2022 o piu' recente.
rem Uso: doppio clic, oppure "compila.bat" dal prompt. Il risultato e' build\iris.exe
rem (e build\iris_tests.exe, le prove automatiche).
rem "compila.bat iris" compila solo il programma, "compila.bat iris_tests" solo le prove.
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio non trovato: installa "Sviluppo di applicazioni desktop con C++".
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Strumenti C++ di Visual Studio non trovati.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
if not exist external\whisper.cpp\CMakeLists.txt (
    echo Mancano le librerie in external\ ^(llama.cpp, whisper.cpp, miniaudio^).
    exit /b 1
)

if not defined VULKAN_SDK (
    for /d %%d in ("C:\VulkanSDK\*") do set "VULKAN_SDK=%%d"
)
if not defined VULKAN_SDK (
    echo Vulkan SDK non trovato: installalo con "winget install KhronosGroup.VulkanSDK".
    exit /b 1
)

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
if "%~1"=="" (
    cmake --build build --config Release || exit /b 1
) else (
    cmake --build build --config Release --target %* || exit /b 1
)
echo.
echo Fatto: build\iris.exe
endlocal
