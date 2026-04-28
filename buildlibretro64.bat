@echo off
setlocal

set "ROOT=%~dp0"
set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "BUILD=%ROOT%libretro-build\release64"
set "RETROARCH_CORES=D:\Emulation\Emulators\RetroArch\cores"
set "CORE_DLL=%BUILD%\libretro\Release\tsugaru_libretro.dll"

call "%VCVARS%"
if errorlevel 1 exit /b %errorlevel%

"%CMAKE%" -S "%ROOT%src" -B "%BUILD%" -G "Visual Studio 17 2022" -A x64 -DBUILD_LIBRETRO_CORE=ON
if errorlevel 1 exit /b %errorlevel%

"%CMAKE%" --build "%BUILD%" --config Release --target tsugaru_libretro --parallel
if errorlevel 1 exit /b %errorlevel%

if not exist "%RETROARCH_CORES%" (
	echo RetroArch cores directory not found: "%RETROARCH_CORES%"
	exit /b 1
)

copy /Y "%CORE_DLL%" "%RETROARCH_CORES%\tsugaru_libretro.dll"
exit /b %errorlevel%
