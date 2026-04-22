$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoDir = Resolve-Path (Join-Path $scriptDir "..")
$workspaceDir = Resolve-Path (Join-Path $repoDir "..")

$pythonExe = Join-Path $workspaceDir ".venv\Scripts\python.exe"
$serverPy = Join-Path $repoDir "quota_server.py"
$supervisorPs1 = Join-Path $scriptDir "quota_server_supervisor.ps1"
$startupDir = [Environment]::GetFolderPath("Startup")
$launcherPath = Join-Path $startupDir "QuotaDashboardServer.cmd"
$runKeyPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$runValueName = "QuotaDashboardServer"
$powershellExe = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"

if (-not (Test-Path $pythonExe)) {
    throw "Python not found: $pythonExe"
}

if (-not (Test-Path $serverPy)) {
    throw "Server script not found: $serverPy"
}

if (-not (Test-Path $supervisorPs1)) {
    throw "Supervisor script not found: $supervisorPs1"
}

$supervisorCmd = '"' + $powershellExe + '" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $supervisorPs1 + '"'

$launcherContent = @(
    '@echo off'
    $supervisorCmd
) -join [Environment]::NewLine

Set-Content -Path $launcherPath -Value $launcherContent -Encoding ASCII
Set-ItemProperty -Path $runKeyPath -Name $runValueName -Value $supervisorCmd -Type String

# Remove legacy scheduled task if it exists.
if (Get-ScheduledTask -TaskName "QuotaDashboardServer" -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName "QuotaDashboardServer" -Confirm:$false
}

$legacyVbs = Join-Path $startupDir "QuotaDashboardServer.vbs"
if (Test-Path $legacyVbs) {
    Remove-Item $legacyVbs -Force
}

Start-Process -FilePath $powershellExe -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', $supervisorPs1) -WorkingDirectory $repoDir -WindowStyle Hidden

Write-Host "Installed startup launcher: $launcherPath"
Write-Host "Installed HKCU Run entry: $runValueName"
Write-Host "Supervisor script: $supervisorPs1"
