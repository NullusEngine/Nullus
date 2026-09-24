[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('before', 'after')]
    [string]$Stage,

    [Parameter(Mandatory = $true)]
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$ProjectPath,

    [string]$BuildDirectory = 'build-editor-camera-perf',
    [string]$EditorPath = '',
    [ValidateSet('scene', 'game')]
    [string]$FocusView = 'scene',
    [string]$SelectGameObject = '',
    [int]$Trials = 3,
    [int]$WarmupFrames = 30,
    [int]$MeasuredFrames = 300,
    [int]$SettleFrames = 0
)

$ErrorActionPreference = 'Stop'

if ($Trials -lt 1) { throw 'Trials must be greater than zero.' }
if ($WarmupFrames -lt 1) { throw 'WarmupFrames must be greater than zero.' }
if ($MeasuredFrames -lt 1) { throw 'MeasuredFrames must be greater than zero.' }
if ($SettleFrames -lt 0) { throw 'SettleFrames cannot be negative.' }

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$projectFullPath = (Resolve-Path -LiteralPath $ProjectPath).Path
$buildFullPath = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
    [System.IO.Path]::GetFullPath($BuildDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDirectory))
}
$editorPath = if ([string]::IsNullOrWhiteSpace($EditorPath)) {
    Join-Path $repoRoot "App\Win64_${Configuration}_Runtime_Shared\Editor.exe"
} else {
    [System.IO.Path]::GetFullPath($EditorPath)
}
if (-not (Test-Path -LiteralPath $editorPath -PathType Leaf)) {
    throw "Editor executable not found: $editorPath"
}

$outputDirectory = Join-Path $buildFullPath "perf\editor-camera\$Stage"
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

for ($trial = 1; $trial -le $Trials; ++$trial) {
    $runLabel = if ($FocusView -eq 'scene') {
        $Configuration.ToLowerInvariant()
    } else {
        "{0}-{1}" -f $Configuration.ToLowerInvariant(), $FocusView
    }
    $outputPath = Join-Path $outputDirectory ("{0}-trial-{1}.json" -f $runLabel, $trial)
    if (Test-Path -LiteralPath $outputPath) {
        throw "Benchmark output already exists: $outputPath"
    }

    $arguments = @(
        '--backend', 'dx12',
        '--editor-validation-focus-view', $FocusView,
        '--editor-validation-exclusive-view', $FocusView,
        '--editor-camera-performance-output', $outputPath,
        '--editor-camera-performance-warmup-frames', $WarmupFrames,
        '--editor-camera-performance-frames', $MeasuredFrames,
        '--editor-camera-performance-settle-frames', $SettleFrames
    )
    if ($FocusView -eq 'scene') {
        $arguments += @('--editor-validation-scene-camera', '-10,3,10;0,135,0')
    }
    if (-not [string]::IsNullOrWhiteSpace($SelectGameObject)) {
        $arguments += @('--editor-validation-select-gameobject', $SelectGameObject)
    }
    $arguments += $projectFullPath

    $process = Start-Process -FilePath $editorPath -ArgumentList $arguments -PassThru
    $summary = $null
    $summaryDeadline = [DateTime]::UtcNow.AddMinutes(10)
    while ([DateTime]::UtcNow -lt $summaryDeadline) {
        if (Test-Path -LiteralPath $outputPath -PathType Leaf) {
            try {
                $candidateSummary = Get-Content -LiteralPath $outputPath -Raw | ConvertFrom-Json
                if ($candidateSummary.measuredFrameCount -eq $MeasuredFrames -and
                    $candidateSummary.settleFrameCount -eq $SettleFrames) {
                    $summary = $candidateSummary
                    break
                }
            } catch {
                # The editor writes the JSON atomically from the benchmark's point of view;
                # tolerate a partially visible file while the write is still in progress.
            }
        }

        if ($process.HasExited) {
            if ($process.ExitCode -ne 0) {
                throw "Editor benchmark trial $trial failed with exit code $($process.ExitCode)."
            }
            break
        }
        Start-Sleep -Milliseconds 250
    }

    if ($null -eq $summary) {
        if (-not $process.HasExited) {
            try { $null = $process.CloseMainWindow() } catch { }
            try { $process.WaitForExit(5000) } catch { }
            if (-not $process.HasExited) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
        }
        throw "Editor benchmark trial $trial produced no complete summary: $outputPath"
    }

    if (-not $process.HasExited) {
        try { $null = $process.CloseMainWindow() } catch { }
        try { $process.WaitForExit(5000) } catch { }
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }

    if ($summary.measuredFrameCount -ne $MeasuredFrames) {
        throw "Editor benchmark trial $trial measured $($summary.measuredFrameCount) frames; expected $MeasuredFrames."
    }
    if ($summary.settleFrameCount -ne $SettleFrames) {
        throw "Editor benchmark trial $trial settled $($summary.settleFrameCount) frames; expected $SettleFrames."
    }
    if ($summary.backend -ne 'DX12' -and $summary.backend -ne 'dx12') {
        throw "Editor benchmark trial $trial used unexpected backend: $($summary.backend)"
    }
    if ($summary.vsync) {
        throw "Editor benchmark trial $trial ran with VSync enabled."
    }
    if ([string]$summary.focusView -ne $FocusView) {
        throw "Editor benchmark trial $trial measured focus view '$($summary.focusView)'; expected '$FocusView'."
    }
}
