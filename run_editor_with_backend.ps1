# Use command-line argument for graphics backend
$backend = "dx12"

$editorPath = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\Editor.exe"
$workingDir = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static"

Write-Host "Launching Editor with DX12 backend..."
Write-Host "Command: Editor.exe --backend $backend"

$proc = Start-Process -FilePath $editorPath -ArgumentList "--backend", $backend -WorkingDirectory $workingDir -PassThru -WindowStyle Normal

Write-Host "Editor started with PID: $($proc.Id)"

# Monitor for 15 seconds
for ($i = 1; $i -le 15; $i++) {
    Start-Sleep -Seconds 1
    $proc.Refresh()

    if ($proc.HasExited) {
        Write-Host "Editor exited at check $i with code: $($proc.ExitCode)"
        break
    }

    if ($i % 3 -eq 0) {
        Write-Host "Editor still running... (check $i, Window: '$($proc.MainWindowTitle)')"
    }
}

# Final check
$proc.Refresh()
if (-not $proc.HasExited) {
    Write-Host ""
    Write-Host "=== SUCCESS: Editor is running with DX12 backend ==="
    Write-Host "Window Title: $($proc.MainWindowTitle)"
    Write-Host "Stopping Editor..."
    Stop-Process -Id $proc.Id -Force
} else {
    Write-Host ""
    Write-Host "=== Editor has exited with code: $($proc.ExitCode) ==="
}

# Show latest log
$latestLog = Get-ChildItem "$workingDir\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latestLog) {
    Write-Host ""
    Write-Host "=== Latest Log ==="
    Get-Content $latestLog.FullName | Select-Object -First 10
}