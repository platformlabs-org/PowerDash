$exe = 'C:\pdtest\PowerDash.exe'
"=== idle frame ==="
& $exe power 1 2>&1 | Select-Object -Last 12
"=== load frames (4 parallel CPU burners, ~10s) ==="
$jobs = 1..4 | ForEach-Object { Start-Job { $end = 1e8; $x = 1.1; for ($i = 0; $i -lt $end; $i++) { $x = $x * 1.0000001 } } }
& $exe power 5 2>&1 | Select-Object -Last 12
$jobs | Stop-Job
$jobs | Remove-Job
"=== cores ==="
(Get-CimInstance Win32_Processor).NumberOfLogicalProcessors
