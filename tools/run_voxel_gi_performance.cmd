@echo off
setlocal
set "ZEN_PERF_VS="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZEN_PERF_VS=%%i"
if not defined ZEN_PERF_VS (
    echo Visual Studio C++ tools were not found.
    exit /b 1
)
call "%ZEN_PERF_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
cmake --preset x64-windows-msvc-performance
if errorlevel 1 goto failed
cmake --build --preset x64-windows-msvc-performance --target scene_renderer_demo -j 8
if errorlevel 1 goto failed
rem Validation remains enabled in ordinary launches; this is a performance run.
"build\x64-windows-msvc-performance\bin\scene_renderer_demo.exe" --mode=3 --disable-validation %*
set "ZEN_PERF_RESULT=%ERRORLEVEL%"
popd
exit /b %ZEN_PERF_RESULT%
:failed
popd
exit /b 1
