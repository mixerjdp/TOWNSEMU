@echo off
setlocal

set "ROOT=%~dp0"
set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "SRC=%ROOT%gui\src"
set "BUILD=%ROOT%gui\build-vs"
set "DEFAULT_PROFILE_SRC=%USERPROFILE%\Documents\Tsugaru_TOWNS\Tsugaru_Default.Tsugaru"

if not exist "%SRC%\public\CMakeLists.txt" (
    pushd "%SRC%"
    git clone https://github.com/captainys/public.git
    if errorlevel 1 (
        popd
        echo Failed to clone public repository.
        exit /b 1
    )
    popd
)

if not exist "%BUILD%" mkdir "%BUILD%"

call "%VCVARS%"
if errorlevel 1 (
    echo Failed to initialize Visual Studio environment.
    exit /b 1
)

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo CMake configure failed.
    exit /b 1
)

"%CMAKE%" --build "%BUILD%" --config Release --target Tsugaru_CUI Tsugaru_GUI --parallel
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

if exist "%DEFAULT_PROFILE_SRC%" (
    copy /Y "%DEFAULT_PROFILE_SRC%" "%BUILD%\main_gui\Release\Tsugaru_Default.Tsugaru" >nul
    copy /Y "%DEFAULT_PROFILE_SRC%" "%BUILD%\main_cui\Release\Tsugaru_Default.Tsugaru" >nul
) else (
    echo Default profile not found at "%DEFAULT_PROFILE_SRC%".
    echo Tsugaru will fall back to its built-in defaults until you create one.
)

echo Build complete.
echo Tsugaru_CUI: %BUILD%\main_cui\Release\Tsugaru_CUI.exe
echo Tsugaru_GUI: %BUILD%\main_gui\Release\Tsugaru_GUI.exe
exit /b 0
