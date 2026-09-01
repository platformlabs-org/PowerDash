# remote-test.ps1 - run a script block on labs-xiaoxin via WinRM (labs / empty password)
param(
    [Parameter(Mandatory=$true)][string]$ScriptFile,
    [string]$CopyFrom,
    [string]$CopyTo
)
$ErrorActionPreference = 'Stop'
$sec = New-Object System.Security.SecureString   # empty password
$cred = New-Object System.Management.Automation.PSCredential('labs', $sec)
$s = New-PSSession -ComputerName labs-xiaoxin -Credential $cred
try {
    if ($CopyFrom) {
        Copy-Item -ToSession $s -Path $CopyFrom -Destination $CopyTo -Force
        Write-Host "[copied $CopyFrom -> $CopyTo]"
    }
    $sb = [scriptblock]::Create((Get-Content -Raw -LiteralPath $ScriptFile))
    Invoke-Command -Session $s -ScriptBlock $sb
} finally {
    Remove-PSSession $s
}
