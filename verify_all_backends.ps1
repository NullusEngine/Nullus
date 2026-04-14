param(
    [string[]]$Backends = @("dx12", "dx11", "vulkan", "opengl"),
    [int]$RunSeconds = 6,
    [string]$ProjectFile = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static\TestProject\TestProject.nullus",
    [string]$WorkingDir = "D:\VSProject\Nullus\App\Win64_Debug_Runtime_Static"
)

function Get-LatestLogPath {
    param([string]$Dir)
    $latest = Get-ChildItem -Path $Dir -Filter "*.log" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $latest) {
        return ""
    }
    return $latest.FullName
}

function Invoke-SmokeProcess {
    param(
        [string]$ExePath,
        [string[]]$Arguments,
        [string]$Dir,
        [int]$Seconds
    )

    $beforeLogPath = Get-LatestLogPath -Dir $Dir
    $proc = Start-Process -FilePath $ExePath -ArgumentList $Arguments -WorkingDirectory $Dir -PassThru -WindowStyle Minimized

    Start-Sleep -Seconds $Seconds
    $proc.Refresh()

    $status = ""
    $exitCode = $null
    if ($proc.HasExited) {
        $status = "Exited"
        $exitCode = $proc.ExitCode
    }
    else {
        $status = "StillRunning"
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }

    $afterLogPath = Get-LatestLogPath -Dir $Dir
    [PSCustomObject]@{
        Status = $status
        ExitCode = $exitCode
        LogPath = $afterLogPath
        PreviousLogPath = $beforeLogPath
    }
}

$editorPath = Join-Path $WorkingDir "Editor.exe"
$gamePath = Join-Path $WorkingDir "Game.exe"

$results = @()

foreach ($backend in $Backends) {
    Write-Host ""
    Write-Host "========================================"
    Write-Host "Testing backend: $backend"
    Write-Host "========================================"

    $editor = Invoke-SmokeProcess -ExePath $editorPath -Arguments @("--backend", $backend, $ProjectFile) -Dir $WorkingDir -Seconds $RunSeconds
    $game = Invoke-SmokeProcess -ExePath $gamePath -Arguments @("--backend", $backend, $ProjectFile) -Dir $WorkingDir -Seconds $RunSeconds

    $results += [PSCustomObject]@{
        RequestedBackend = $backend
        EditorStatus = $editor.Status
        EditorExitCode = $editor.ExitCode
        EditorLogPath = $editor.LogPath
        GameStatus = $game.Status
        GameExitCode = $game.ExitCode
        GameLogPath = $game.LogPath
    }

    Write-Host ("Editor => Status={0}, ExitCode={1}, Log={2}" -f $editor.Status, $editor.ExitCode, $editor.LogPath)
    Write-Host ("Game   => Status={0}, ExitCode={1}, Log={2}" -f $game.Status, $game.ExitCode, $game.LogPath)
}

Write-Host ""
Write-Host "========================================"
Write-Host "Summary"
Write-Host "========================================"
$results | Format-Table -AutoSize
