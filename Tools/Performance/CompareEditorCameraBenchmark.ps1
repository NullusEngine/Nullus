[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BeforeDirectory,

    [Parameter(Mandatory = $true)]
    [string]$AfterDirectory,

    [Parameter(Mandatory = $true)]
    [string]$JsonOutput,

    [Parameter(Mandatory = $true)]
    [string]$MarkdownOutput,

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string[]]$Configurations = @('Debug', 'Release'),

    [ValidateSet('scene', 'game')]
    [string]$FocusView = 'scene',

    [ValidateRange(0, 100)]
    [double]$MaxRegressionPercent = 5,

    [switch]$EnforceFrameTimeGate
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-Percentile([double[]]$SortedSamples, [double]$Quantile) {
    if ($Quantile -eq 0.5 -and $SortedSamples.Count % 2 -eq 0) {
        $middle = $SortedSamples.Count / 2
        return ($SortedSamples[$middle - 1] + $SortedSamples[$middle]) / 2
    }
    $index = [math]::Max(0, [math]::Ceiling($Quantile * $SortedSamples.Count) - 1)
    return $SortedSamples[$index]
}

function Read-BenchmarkRuns([string]$Directory, [string]$Configuration) {
    $label = $Configuration.ToLowerInvariant()
    if ($FocusView -eq 'game') { $label += '-game' }
    $files = @(Get-ChildItem -LiteralPath $Directory -Filter "$label-trial-*.json" -File |
        Sort-Object Name)
    if ($files.Count -eq 0) {
        throw "No $Configuration benchmark runs found in $Directory."
    }
    foreach ($file in $files) {
        $run = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
        $samples = @($run.measuredFrameMs)
        if ($samples.Count -eq 0 -or $samples.Count -ne $run.measuredFrameCount -or
            $run.measuredFrameCount -ne $run.requestedFrameCount) {
            throw "Incomplete frame samples: $($file.FullName)"
        }
        foreach ($sample in $samples) {
            $value = [double]$sample
            if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -le 0) {
                throw "Invalid frame time in $($file.FullName): $sample"
            }
        }
        if ($run.vsync -or $run.focusView -ne $FocusView) {
            throw "Benchmark must use VSync off and focus view '$FocusView': $($file.FullName)"
        }
        [pscustomobject]@{ Path = $file.FullName; Data = $run }
    }
}

function Assert-MatchedRuns($Runs) {
    $reference = $Runs[0]
    $fields = @('backend', 'configuration', 'focusView', 'projectPath', 'scenePath',
        'viewportWidth', 'viewportHeight', 'vsync', 'warmupFrameCount',
        'measuredFrameCount', 'settleFrameCount', 'cameraForwardStep', 'selectedGameObject')
    foreach ($run in $Runs) {
        foreach ($field in $fields) {
            if ($run.Data.$field -ne $reference.Data.$field) {
                throw "Unmatched $field in $($run.Path); reference: $($reference.Path)"
            }
        }
    }
}

function Get-Aggregate($Runs) {
    [double[]]$samples = @($Runs | ForEach-Object { $_.Data.measuredFrameMs } | Sort-Object)
    $mean = ($samples | Measure-Object -Average).Average
    return [ordered]@{
        runCount = $Runs.Count
        measuredFrameCount = $samples.Count
        meanFrameMs = $mean
        meanFps = 1000 / $mean
        medianFrameMs = Get-Percentile $samples 0.5
        p95FrameMs = Get-Percentile $samples 0.95
        p99FrameMs = Get-Percentile $samples 0.99
        maxFrameMs = $samples[-1]
        publicationRatio = ($Runs | ForEach-Object { $_.Data.publicationRatio } |
            Measure-Object -Average).Average
        runs = @($Runs | ForEach-Object { [ordered]@{
            path = $_.Path
            sha256 = (Get-FileHash -LiteralPath $_.Path -Algorithm SHA256).Hash
            meanFrameMs = $_.Data.meanFrameMs
            p95FrameMs = $_.Data.p95FrameMs
            publicationRatio = $_.Data.publicationRatio
        } })
    }
}

function Get-LowerIsBetterPercent([double]$Before, [double]$After) {
    if ($Before -eq 0.0) { return 0.0 }
    return (($Before - $After) / $Before) * 100.0
}

function Get-HigherIsBetterPercent([double]$Before, [double]$After) {
    if ($Before -eq 0.0) { return 0.0 }
    return (($After - $Before) / $Before) * 100.0
}

$comparison = [ordered]@{}
$gatePassed = $true
foreach ($configuration in $Configurations) {
    $beforeRuns = @(Read-BenchmarkRuns $BeforeDirectory $configuration)
    $afterRuns = @(Read-BenchmarkRuns $AfterDirectory $configuration)
    if ($beforeRuns.Count -ne $afterRuns.Count) {
        throw "Before/after trial counts differ for $configuration."
    }
    Assert-MatchedRuns ($beforeRuns + $afterRuns)
    $before = Get-Aggregate $beforeRuns
    $after = Get-Aggregate $afterRuns
    $medianRegression = -1 * (Get-LowerIsBetterPercent $before.medianFrameMs $after.medianFrameMs)
    $p95Regression = -1 * (Get-LowerIsBetterPercent $before.p95FrameMs $after.p95FrameMs)
    $passed = $medianRegression -le $MaxRegressionPercent -and $p95Regression -le $MaxRegressionPercent
    $gatePassed = $gatePassed -and $passed
    $comparison[$configuration] = [ordered]@{
        workload = $beforeRuns[0].Data | Select-Object backend, configuration, focusView,
            projectPath, scenePath, viewportWidth, viewportHeight, vsync,
            warmupFrameCount, measuredFrameCount, settleFrameCount, cameraForwardStep, selectedGameObject
        statistics = 'All measured frames pooled; median averages the middle pair; P95/P99 use nearest rank.'
        before = $before
        after = $after
        change = [ordered]@{
            medianFrameMsPercent = Get-LowerIsBetterPercent $before.medianFrameMs $after.medianFrameMs
            meanFrameMsPercent = Get-LowerIsBetterPercent $before.meanFrameMs $after.meanFrameMs
            meanFpsPercent = Get-HigherIsBetterPercent $before.meanFps $after.meanFps
            p95FrameMsPercent = Get-LowerIsBetterPercent $before.p95FrameMs $after.p95FrameMs
            p99FrameMsPercent = Get-LowerIsBetterPercent $before.p99FrameMs $after.p99FrameMs
            maxFrameMsPercent = Get-LowerIsBetterPercent $before.maxFrameMs $after.maxFrameMs
            publicationRatioPoints = ([double]$after.publicationRatio - [double]$before.publicationRatio) * 100.0
        }
        frameTimeGate = [ordered]@{
            maxRegressionPercent = $MaxRegressionPercent
            medianRegressionPercent = $medianRegression
            p95RegressionPercent = $p95Regression
            passed = $passed
        }
    }
}

$jsonParent = Split-Path -Parent $JsonOutput
$markdownParent = Split-Path -Parent $MarkdownOutput
if ($jsonParent) { New-Item -ItemType Directory -Force -Path $jsonParent | Out-Null }
if ($markdownParent) { New-Item -ItemType Directory -Force -Path $markdownParent | Out-Null }
$comparison | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $JsonOutput -Encoding utf8

$lines = @(
    '# Editor Camera Performance Results',
    '',
    '| Configuration | Metric | Before | After | Change |',
    '| --- | --- | ---: | ---: | ---: |'
)
foreach ($configuration in $Configurations) {
    $entry = $comparison[$configuration]
    $lines += "| $configuration | Median frame time | $([math]::Round($entry.before.medianFrameMs, 3)) ms | $([math]::Round($entry.after.medianFrameMs, 3)) ms | $([math]::Round($entry.change.medianFrameMsPercent, 2))% |"
    $lines += "| $configuration | Mean frame time | $([math]::Round($entry.before.meanFrameMs, 3)) ms | $([math]::Round($entry.after.meanFrameMs, 3)) ms | $([math]::Round($entry.change.meanFrameMsPercent, 2))% |"
    $lines += "| $configuration | Mean FPS | $([math]::Round($entry.before.meanFps, 2)) | $([math]::Round($entry.after.meanFps, 2)) | $([math]::Round($entry.change.meanFpsPercent, 2))% |"
    $lines += "| $configuration | P95 frame time | $([math]::Round($entry.before.p95FrameMs, 3)) ms | $([math]::Round($entry.after.p95FrameMs, 3)) ms | $([math]::Round($entry.change.p95FrameMsPercent, 2))% |"
    $lines += "| $configuration | P99 frame time | $([math]::Round($entry.before.p99FrameMs, 3)) ms | $([math]::Round($entry.after.p99FrameMs, 3)) ms | $([math]::Round($entry.change.p99FrameMsPercent, 2))% |"
    $lines += "| $configuration | Max frame time | $([math]::Round($entry.before.maxFrameMs, 3)) ms | $([math]::Round($entry.after.maxFrameMs, 3)) ms | $([math]::Round($entry.change.maxFrameMsPercent, 2))% |"
    $lines += "| $configuration | Publication ratio | $([math]::Round($entry.before.publicationRatio * 100.0, 2))% | $([math]::Round($entry.after.publicationRatio * 100.0, 2))% | $([math]::Round($entry.change.publicationRatioPoints, 2)) pp |"
}
$lines += @('', 'Positive change means improvement. No measured frames or outliers are discarded.',
    "Frame-time gate: median and P95 may regress by at most $MaxRegressionPercent%. Passed: $gatePassed.")
$lines | Set-Content -LiteralPath $MarkdownOutput -Encoding utf8

if ($EnforceFrameTimeGate -and !$gatePassed) {
    throw "CPU median/P95 exceeded the $MaxRegressionPercent% regression gate. See $JsonOutput."
}
