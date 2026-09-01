@echo off
setlocal
set PV=%1
if not "%PV%"=="" set PD_VERSION=%PV%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
msbuild PowerDash.sln /p:Configuration=Release /p:Platform=x64 /p:SpectreMitigation=false /m /nologo /v:m
endlocal
