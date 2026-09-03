@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /I..\PowerDash PowerDashUiTests.cpp ..\PowerDash\PowerDashUi.cpp ..\PowerDash\PowerDashProbe.cpp ..\PowerDash\PowerDashAmd.cpp ..\PowerDash\PowerDashIntel.cpp ..\PowerDash\PowerDashPmTable.cpp ..\PowerDash\PowerDashSampler.cpp ..\PowerDash\PowerDashSensors.cpp ..\PowerDash\PowerDashUsage.cpp /Fe:build\PowerDashUiTests.exe
if errorlevel 1 exit /b %errorlevel%
build\PowerDashUiTests.exe
exit /b %errorlevel%
