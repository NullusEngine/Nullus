# Debug Vulkan crash with detailed logging
$projectFile = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\TestProject\TestProject.nullus"
$workingDir = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static"

$gamePath = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\Game.exe"

Write-Host "Starting Game with Vulkan backend..."

# Use RenderDoc to capture the frame
$env:NLS_RENDERDOC_CAPTURE = "1"
$env:NLS_RENDERDOC_CAPTURE_AFTER_FRAMES = "5"

$proc = Start-Process -FilePath $gamePath -ArgumentList "--backend", "vulkan", "`"$projectFile`"" -WorkingDirectory $workingDir -PassThru -WindowStyle Normal

for ($i = 1; $i -le 20; $i++) {
    Start-Sleep 1
    $proc.Refresh()

    if ($proc.HasExited) {
        Write-Host "Game exited at check $i with code: $($proc.ExitCode)"
        break
    }

    if ($i % 5 -eq 0) {
        Write-Host "Running... (check $i, Window: '$($proc.MainWindowTitle)')"
    }
}

if (-not $proc.HasExited) {
    Write-Host "Game is running! Window: $($proc.MainWindowTitle)"
    Stop-Process -Id $proc.Id -Force
}

# Check for RenderDoc captures
$capturePath = "D:\VSProject\Nullus\Captures"
if (Test-Path $capturePath) {
    $captures = Get-ChildItem $capturePath -Filter "*.rdc" -ErrorAction SilentlyContinue
    if ($captures) {
        Write-Host ""
        Write-Host "Found $($captures.Count) RenderDoc captures"
        $captures | ForEach-Object { Write-Host "  - $($_.Name)" }
    }
}