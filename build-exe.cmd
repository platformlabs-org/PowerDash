@echo off
rem build ONLY the app, embedding whatever PowerDash.sys currently exists
rem (BuildProjectReferences=false prevents msbuild from rebuilding - and
rem  re-test-signing / overwriting - your manually signed driver).
rem Workflow:  build-sys.cmd  ->  sign the .sys yourself  ->  build-exe.cmd
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
rem MSBuild does not track the binary referenced inside PowerDash.rc, so the
rem cached .res would keep an older driver. Drop it to force re-embedding.
del /q PowerDash\x64\Release\PowerDash.res 2>nul
msbuild PowerDash.sln /t:PowerDash /p:BuildProjectReferences=false /p:Configuration=Release /p:Platform=x64 /p:SpectreMitigation=false /m /nologo /v:m
endlocal
