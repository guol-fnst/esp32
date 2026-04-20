param(
    [string]$TaskName = "QuotaDashboardServer"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoDir = Resolve-Path (Join-Path $scriptDir "..")
$workspaceDir = Resolve-Path (Join-Path $repoDir "..")

$pythonExe = Join-Path $workspaceDir ".venv\Scripts\python.exe"
$serverPy = Join-Path $repoDir "quota_server.py"
$outLog = Join-Path $repoDir "server.out.log"
$errLog = Join-Path $repoDir "server.err.log"
$powershellExe = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"

if (-not (Test-Path $pythonExe)) {
    throw "Python not found: $pythonExe"
}

if (-not (Test-Path $serverPy)) {
    throw "Server script not found: $serverPy"
}

$command = '& "' + $pythonExe + '" "' + $serverPy + '" 1>> "' + $outLog + '" 2>> "' + $errLog + '"'
$encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
$arg = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand ' + $encodedCommand

$action = New-ScheduledTaskAction -Execute $powershellExe -Argument $arg -WorkingDirectory $repoDir
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
}

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings -Principal $principal | Out-Null

# Start now so the setting takes effect immediately.
Start-ScheduledTask -TaskName $TaskName

Write-Host "Installed and started scheduled task: $TaskName"
