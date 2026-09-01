Get-Process PowerDash -ErrorAction SilentlyContinue |
    Select-Object Id, StartTime | Format-Table | Out-String
Stop-Process -Name PowerDash -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
"stopped: $((Get-Process PowerDash -ErrorAction SilentlyContinue | Measure-Object).Count) remaining"
