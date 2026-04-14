param(
    [string]$Backend = "dx12",
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

Write-Host "Testing requested backend: $Backend"
Write-Host "========================================"

$editor = Invoke-SmokeProcess -ExePath $editorPath -Arguments @("--backend", $Backend, $ProjectFile) -Dir $WorkingDir -Seconds $RunSeconds
$game = Invoke-SmokeProcess -ExePath $gamePath -Arguments @("--backend", $Backend, $ProjectFile) -Dir $WorkingDir -Seconds $RunSeconds

Write-Host ("RequestedBackend: {0}" -f $Backend)
Write-Host ("EditorStatus: {0}" -f $editor.Status)
Write-Host ("EditorExitCode: {0}" -f $editor.ExitCode)
Write-Host ("EditorLogPath: {0}" -f $editor.LogPath)
Write-Host ("GameStatus: {0}" -f $game.Status)
Write-Host ("GameExitCode: {0}" -f $game.ExitCode)
Write-Host ("GameLogPath: {0}" -f $game.LogPath)
