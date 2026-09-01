@echo off
rem Build an INF-installable driver package: package\{PowerDash.inf, PowerDash.sys, PowerDash.cat}
rem Prerequisite: driver built (build-sys.cmd) and signed (your cert or sign-test.cmd).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set KIT=C:\Program Files (x86)\Windows Kits\10
set INF2CAT=%KIT%\bin\10.0.26100.0\x86\inf2cat.exe
set SIG=%KIT%\bin\10.0.22621.0\x64\signtool.exe

cd /d "%~dp0"
if not exist x64\Release\PowerDash.sys ( echo build the driver first: build-sys.cmd + sign & exit /b 1 )

rmdir /s /q package 2>nul
mkdir package
copy /y x64\Release\PowerDash.inf package\ >nul
copy /y x64\Release\PowerDashSYS\PowerDash.sys package\ >nul

"%INF2CAT%" /driver:package /os:10_x64 /v 2>&1 | findstr /i "error catalog" 
if not exist package\PowerDash.cat ( echo inf2cat failed & exit /b 1 )

rem sign the catalog with the test certificate (replace with your own for release)
"%SIG%" sign /ph /fd SHA256 /s PrivateCertStore /n EmExpTest package\PowerDash.cat || (
  echo sign the catalog with your certificate:
  echo   signtool sign /fd SHA256 /tr ^<ts^> /td SHA256 /a package\PowerDash.cat
  exit /b 1
)

echo.
echo package ready: package\PowerDash.inf + PowerDash.sys + PowerDash.cat
echo install : pnputil /add-driver package\PowerDash.inf /install   (or devcon install package\PowerDash.inf Root\PowerDash)
echo remove  : pnputil /delete-driver oem##.inf /uninstall /force
endlocal
