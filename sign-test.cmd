@echo off
rem Explicitly sign the built driver with the local test certificate
rem (PrivateCertStore\EmExpTest, created by the EM-Driver project).
rem The build itself NEVER signs - signing is always an explicit step.
setlocal
set SIG="C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\signtool.exe"
set SYS=x64\Release\PowerDashSYS\PowerDash.sys

%SIG% sign /ph /fd SHA256 /s PrivateCertStore /n EmExpTest %SYS% || (
  echo.
  echo test certificate not found - create it once:
  echo   makecert -r -pe -ss PrivateCertStore -n "CN=EmExpTest" EmExpTest.cer
  exit /b 1
)
%SIG% verify /pa %SYS% 2>nul
echo signed with test certificate (requires testsigning mode to load)
endlocal
