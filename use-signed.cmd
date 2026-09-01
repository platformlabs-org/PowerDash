@echo off
rem Place an externally signed PowerDash.sys at the embed location, rebuild
rem the exe around it and verify the embedded bytes match.
rem   use-signed.cmd C:\path\to\signed\PowerDash.sys
setlocal
if "%~1"=="" ( echo usage: use-signed.cmd ^<signed PowerDash.sys^> & exit /b 1 )
if not exist "%~1" ( echo file not found: %~1 & exit /b 1 )

cd /d "%~dp0"
copy /y "%~1" x64\Release\PowerDashSYS\PowerDash.sys || exit /b 1
echo signed driver staged at embed path

call build-exe.cmd || exit /b 1
py verify_embed.py || (
  echo EMBED MISMATCH - exe does not contain the signed driver
  exit /b 1
)
echo.
echo x64\Release\PowerDash.exe now embeds the signed driver.
endlocal
