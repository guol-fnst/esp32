$startupDir = [Environment]::GetFolderPath("Startup")
$launcherPath = Join-Path $startupDir "QuotaDashboardServer.cmd"

if (Test-Path $launcherPath) {
    Remove-Item $launcherPath -Force
    Write-Host "Removed startup launcher: $launcherPath"
} else {
    Write-Host "Startup launcher not found: $launcherPath"
}

$legacyVbs = Join-Path $startupDir "QuotaDashboardServer.vbs"
if (Test-Path $legacyVbs) {
    Remove-Item $legacyVbs -Force
    Write-Host "Removed legacy startup launcher: $legacyVbs"
}

if (Get-ScheduledTask -TaskName "QuotaDashboardServer" -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName "QuotaDashboardServer" -Confirm:$false
    Write-Host "Removed legacy scheduled task: QuotaDashboardServer"
}
