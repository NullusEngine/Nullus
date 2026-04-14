# Enable Vulkan validation and run with RenderDoc
$projectFile = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\TestProject\TestProject.nullus"
$workingDir = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static"

# Set environment variables for RenderDoc capture
$env:NLS_RENDERDOC_CAPTURE = "1"
$env:NLS_RENDERDOC_CAPTURE_AFTER_FRAMES = "3"

$gamePath = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\Game.exe"

Write-Host "Starting Game with Vulkan + RenderDoc..."
Write-Host "Backend: --backend vulkan"
Write-Host "RenderDoc capture: $env:NLS_RENDERDOC_CAPTURE"

$proc = Start-Process -FilePath $gamePath -ArgumentList "--backend", "vulkan", "`"$projectFile`"" -WorkingDirectory $workingDir -PassThru -WindowStyle Normal

for ($i = 1; $i -le 15; $i++) {
    Start-Sleep 1
    $proc.Refresh()

    if ($proc.HasExited) {
        Write-Host "Game exited at check $i with code: $($proc.ExitCode)"
        break
    }

    if ($i % 3 -eq 0) {
        Write-Host "Running... (check $i, Window: '$($proc.MainWindowTitle)')"
    }
}

if (-not $proc.HasExited) {
    Write-Host "Game is running! Window: $($proc.MainWindowTitle)"
    Stop-Process -Id $proc.Id -Force
}

# Check latest log
$latestLog = Get-ChildItem "$workingDir\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latestLog) {
    Write-Host ""
    Write-Host "=== Latest Log ==="
    Get-Content $latestLog.FullName
}

# Check for RenderDoc captures
$captureDir = "D:\VSProject\Nullus\Captures"
if (Test-Path $captureDir) {
    $captures = Get-ChildItem $captureDir -Filter "*.rdc" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 3
    if ($captures) {
        Write-Host ""
        Write-Host "=== RenderDoc Captures ==="
        $captures | ForEach-Object { Write-Host "  - $($_.Name) ($([math]::Round($_.Length / 1MB, 2)) MB)" }
    }
}