@echo off
rem build ONLY the kernel driver (PowerDashSYS), Release x64.
rem WDK test-signs it automatically; overwrite with your own signature:
rem   signtool sign /v /fd SHA256 /tr <ts-server> /td SHA256 /a x64\Release\PowerDashSYS\PowerDash.sys
rem INF DriverVer version is pinned (default 1.0.0.0). Override:
rem   build-sys.cmd 1.2.3.4
setlocal
set PV=%1
if not "%PV%"=="" set PD_VERSION=%PV%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
msbuild PowerDash.sln /t:PowerDashSYS /p:Configuration=Release /p:Platform=x64 /p:SpectreMitigation=false /m /nologo /v:m
endlocal
