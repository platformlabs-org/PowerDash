$env:PDASH_FORCE_VT = '1'
$out = & C:\Users\labs\Desktop\PowerDash.exe power 4 2>&1 | Out-String
[IO.File]::WriteAllBytes('C:\Users\labs\vtcap.bin', [Text.Encoding]::UTF8.GetBytes($out))
[Convert]::ToBase64String([IO.File]::ReadAllBytes('C:\Users\labs\vtcap.bin'))
Remove-Item C:\Users\labs\vtcap.bin -ErrorAction SilentlyContinue
