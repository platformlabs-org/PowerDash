$e = [char]27
$strip = { param($t) ($t -replace "$e\[[0-9;?]*[A-Za-z]", "") -replace "`r","" }
cmd /c "set PDASH_FORCE_VT=1&& C:\Users\labs\Desktop\PowerDash.exe power 3 > C:\Users\labs\idle.bin 2>&1"
$jobs = 1..4 | ForEach-Object { Start-Job { $end=1e8; $x=1.1; for ($i=0;$i -lt $end;$i++) { $x = $x*1.0000001 } } }
Start-Sleep -Seconds 1
cmd /c "set PDASH_FORCE_VT=1&& C:\Users\labs\Desktop\PowerDash.exe power 3 > C:\Users\labs\load.bin 2>&1"
$jobs | Stop-Job; $jobs | Remove-Job
foreach ($f in 'idle','load') {
  "=== $f ==="
  (& $strip ([IO.File]::ReadAllText("C:\Users\labs\$f.bin"))) -split "`n" |
    Where-Object { $_ -match 'UTIL|C0 |W$' } | ForEach-Object { $_.TrimEnd() } | Select-Object -First 8
}
Remove-Item C:\Users\labs\idle.bin, C:\Users\labs\load.bin -ErrorAction SilentlyContinue
