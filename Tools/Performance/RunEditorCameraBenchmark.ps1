[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('before', 'after')]
    [string]$Stage,

    [Parameter(Mandatory = $true)]
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$ProjectPath,

    [string]$BuildDirectory = 'build-editor-camera-perf',
    [int]$Trials = 3,
    [int]$WarmupFrames = 30,
    [int]$MeasuredFrames = 300
)

$ErrorActionPreference = 'Stop'

if ($Trials -lt 1) { throw 'Trials must be greater than zero.' }
if ($WarmupFrames -lt 1) { throw 'WarmupFrames must be greater than zero.' }
if ($MeasuredFrames -lt 1) { throw 'MeasuredFrames must be greater than zero.' }

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$projectFullPath = (Resolve-Path -LiteralPath $ProjectPath).Path
$buildFullPath = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
    [System.IO.Path]::GetFullPath($BuildDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDirectory))
}
$editorPath = Join-Path $repoRoot "App\Win64_${Configuration}_Runtime_Shared\Editor.exe"
if (-not (Test-Path -LiteralPath $editorPath -PathType Leaf)) {
    throw "Editor executable not found: $editorPath"
}

$outputDirectory = Join-Path $buildFullPath "perf\editor-camera\$Stage"
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

for ($trial = 1; $trial -le $Trials; ++$trial) {
    $outputPath = Join-Path $outputDirectory ("{0}-trial-{1}.json" -f $Configuration.ToLowerInvariant(), $trial)
    if (Test-Path -LiteralPath $outputPath) {
        throw "Benchmark output already exists: $outputPath"
    }

    $arguments = @(
        '--backend', 'dx12',
        '--editor-validation-focus-view', 'scene',
        '--editor-validation-exclusive-view', 'scene',
        '--editor-validation-scene-camera', '-10,3,10;0,135,0',
        '--editor-camera-performance-output', $outputPath,
        '--editor-camera-performance-warmup-frames', $WarmupFrames,
        '--editor-camera-performance-frames', $MeasuredFrames,
        $projectFullPath
    )

    $process = Start-Process -FilePath $editorPath -ArgumentList $arguments -PassThru -Wait
    if ($process.ExitCode -ne 0) {
        throw "Editor benchmark trial $trial failed with exit code $($process.ExitCode)."
    }
    if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
        throw "Editor benchmark trial $trial produced no summary: $outputPath"
    }

    $summary = Get-Content -LiteralPath $outputPath -Raw | ConvertFrom-Json
    if ($summary.measuredFrameCount -ne $MeasuredFrames) {
        throw "Editor benchmark trial $trial measured $($summary.measuredFrameCount) frames; expected $MeasuredFrames."
    }
    if ($summary.backend -ne 'DX12' -and $summary.backend -ne 'dx12') {
        throw "Editor benchmark trial $trial used unexpected backend: $($summary.backend)"
    }
    if ($summary.vsync) {
        throw "Editor benchmark trial $trial ran with VSync enabled."
    }
}
