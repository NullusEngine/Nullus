# Test Vulkan Game with visible window
$projectFile = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\TestProject\TestProject.nullus"
$workingDir = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static"

$gamePath = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\Game.exe"
$gameProc = Start-Process -FilePath $gamePath -ArgumentList "--backend", "vulkan", "`"$projectFile`"" -WorkingDirectory $workingDir -PassThru -WindowStyle Normal

Write-Host "Game started with PID: $($gameProc.Id)"

for ($i = 1; $i -le 10; $i++) {
    Start-Sleep 1
    $gameProc.Refresh()
    if ($gameProc.HasExited) {
        Write-Host "Game exited at check $i with code: $($gameProc.ExitCode)"
        break
    }
    if ($i % 3 -eq 0) {
        Write-Host "Running... (check $i, Window: '$($gameProc.MainWindowTitle)')"
    }
}

if (-not $gameProc.HasExited) {
    Write-Host "Game is running successfully! Window: $($gameProc.MainWindowTitle)"
    Stop-Process -Id $gameProc.Id -Force
}