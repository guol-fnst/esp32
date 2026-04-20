$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoDir = Resolve-Path (Join-Path $scriptDir "..")
$workspaceDir = Resolve-Path (Join-Path $repoDir "..")

$pythonExe = Join-Path $workspaceDir ".venv\Scripts\python.exe"
$pythonwExe = Join-Path $workspaceDir ".venv\Scripts\pythonw.exe"
$serverPy = Join-Path $repoDir "quota_server.py"
$startupDir = [Environment]::GetFolderPath("Startup")
$launcherPath = Join-Path $startupDir "QuotaDashboardServer.cmd"

if (-not (Test-Path $pythonExe)) {
    throw "Python not found: $pythonExe"
}

if (-not (Test-Path $pythonwExe)) {
    throw "Pythonw not found: $pythonwExe"
}

if (-not (Test-Path $serverPy)) {
    throw "Server script not found: $serverPy"
}

$launcherContent = @(
    '@echo off'
    'cd /d "' + $repoDir + '"'
    'start "QuotaDashboardServer" /b "' + $pythonwExe + '" "' + $serverPy + '"'
) -join [Environment]::NewLine

Set-Content -Path $launcherPath -Value $launcherContent -Encoding ASCII

# Remove legacy scheduled task if it exists.
if (Get-ScheduledTask -TaskName "QuotaDashboardServer" -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName "QuotaDashboardServer" -Confirm:$false
}

$legacyVbs = Join-Path $startupDir "QuotaDashboardServer.vbs"
if (Test-Path $legacyVbs) {
    Remove-Item $legacyVbs -Force
}

$running = $false
try {
    $response = Invoke-WebRequest -UseBasicParsing http://127.0.0.1:8765/health -TimeoutSec 2
    $running = ($response.StatusCode -eq 200)
} catch {
    $running = $false
}

if (-not $running) {
    Start-Process -FilePath $pythonwExe -ArgumentList @($serverPy) -WorkingDirectory $repoDir -WindowStyle Hidden
}

Write-Host "Installed startup launcher: $launcherPath"
