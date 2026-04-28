@echo off
setlocal

set "ROOT=%~dp0"
set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "BUILD=%ROOT%libretro-build\release64"

call "%VCVARS%"
if errorlevel 1 exit /b %errorlevel%

"%CMAKE%" -S "%ROOT%src" -B "%BUILD%" -G "Visual Studio 17 2022" -A x64 -DBUILD_LIBRETRO_CORE=ON
if errorlevel 1 exit /b %errorlevel%

"%CMAKE%" --build "%BUILD%" --config Release --target tsugaru_libretro --parallel
exit /b %errorlevel%
