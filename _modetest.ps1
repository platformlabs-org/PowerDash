$exe = 'C:\pdtest\PowerDash.exe'
$out = 'C:\pdtest\out.txt'
$env:PDASH_FORCE_VT = '1'
& cmd /c "`"$exe`" power 4 > $out 2>&1"
$env:PDASH_FORCE_VT = $null
$raw = Get-Content -Raw -Encoding UTF8 $out
$esc = [string][char]27
$clean = $raw -replace "$esc\[[0-9;?]*[A-Za-z]", ''
$lines = $clean -split "`r?`n" | Where-Object { $_ -ne '' }
"total lines (power 4): $($lines.Count)  expect 1 static + 4x12 = 49"
"rewind bytes: " + ($raw.Contains($esc + '[12A'))
"line widths: " + (($lines | Select-Object -Last 12 | ForEach-Object { $_.Length }) -join ',')
""
"--- last frame ---"
$lines | Select-Object -Last 12
