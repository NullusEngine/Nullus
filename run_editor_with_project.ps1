# Use command-line argument for graphics backend
$backend = "dx12"

$editorPath = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\Editor.exe"
$projectFile = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\TestProject\TestProject.nullus"
$workingDir = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static"

Write-Host "启动 Editor 并加载项目..."
Write-Host "后端: --backend $backend"
Write-Host "项目: $projectFile"

$proc = Start-Process -FilePath $editorPath -ArgumentList "--backend", $backend, "`"$projectFile`"" -WorkingDirectory $workingDir -PassThru -WindowStyle Normal

Write-Host "Editor 已启动, PID: $($proc.Id)"

# 监控 20 秒
for ($i = 1; $i -le 20; $i++) {
    Start-Sleep -Seconds 1
    $proc.Refresh()

    if ($proc.HasExited) {
        Write-Host "Editor 在检查 $i 时退出，退出码: $($proc.ExitCode)"
        break
    }

    if ($i % 5 -eq 0) {
        Write-Host "Editor 仍在运行... (检查 $i, 窗口: '$($proc.MainWindowTitle)')"
    }
}

# 最终检查
$proc.Refresh()
if (-not $proc.HasExited) {
    Write-Host ""
    Write-Host "=== 成功: Editor 正在运行 ==="
    Write-Host "窗口标题: $($proc.MainWindowTitle)"
    Write-Host "停止 Editor..."
    Stop-Process -Id $proc.Id -Force
} else {
    Write-Host ""
    Write-Host "=== Editor 已退出，退出码: $($proc.ExitCode) ==="
}

# 显示最新日志
$latestLog = Get-ChildItem "$workingDir\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latestLog) {
    Write-Host ""
    Write-Host "=== 最新日志 ==="
    Get-Content $latestLog.FullName
}