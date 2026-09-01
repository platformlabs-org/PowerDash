cmd /c "set PDASH_FORCE_VT=1&& C:\Users\labs\Desktop\PowerDash.exe power 4 > C:\Users\labs\vtcap.bin 2>&1"
[Convert]::ToBase64String([IO.File]::ReadAllBytes('C:\Users\labs\vtcap.bin'))
Remove-Item C:\Users\labs\vtcap.bin -ErrorAction SilentlyContinue
