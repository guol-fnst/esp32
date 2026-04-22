$startupDir = [Environment]::GetFolderPath("Startup")
$launcherPath = Join-Path $startupDir "QuotaDashboardServer.cmd"
$runKeyPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$runValueName = "QuotaDashboardServer"

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

if (Get-ItemProperty -Path $runKeyPath -Name $runValueName -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $runKeyPath -Name $runValueName -ErrorAction SilentlyContinue
    Write-Host "Removed HKCU Run entry: $runValueName"
} else {
    Write-Host "HKCU Run entry not found: $runValueName"
}
