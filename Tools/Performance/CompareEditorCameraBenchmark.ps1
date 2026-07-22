[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BeforeDirectory,

    [Parameter(Mandatory = $true)]
    [string]$AfterDirectory,

    [Parameter(Mandatory = $true)]
    [string]$JsonOutput,

    [Parameter(Mandatory = $true)]
    [string]$MarkdownOutput
)

$ErrorActionPreference = 'Stop'

function Get-MedianRun([string]$Directory, [string]$Configuration) {
    $pattern = "{0}-trial-*.json" -f $Configuration.ToLowerInvariant()
    $runs = @(Get-ChildItem -LiteralPath $Directory -Filter $pattern -File |
        ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json } |
        Sort-Object meanFrameMs)
    if ($runs.Count -lt 1) {
        throw "No $Configuration benchmark runs found in $Directory."
    }
    return $runs[[math]::Floor($runs.Count / 2)]
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
foreach ($configuration in @('Debug', 'Release')) {
    $before = Get-MedianRun $BeforeDirectory $configuration
    $after = Get-MedianRun $AfterDirectory $configuration
    $comparison[$configuration] = [ordered]@{
        before = $before
        after = $after
        change = [ordered]@{
            meanFrameMsPercent = Get-LowerIsBetterPercent $before.meanFrameMs $after.meanFrameMs
            meanFpsPercent = Get-HigherIsBetterPercent $before.meanFps $after.meanFps
            p95FrameMsPercent = Get-LowerIsBetterPercent $before.p95FrameMs $after.p95FrameMs
            p99FrameMsPercent = Get-LowerIsBetterPercent $before.p99FrameMs $after.p99FrameMs
            maxFrameMsPercent = Get-LowerIsBetterPercent $before.maxFrameMs $after.maxFrameMs
            publicationRatioPoints = ([double]$after.publicationRatio - [double]$before.publicationRatio) * 100.0
        }
    }
}

$jsonParent = Split-Path -Parent $JsonOutput
$markdownParent = Split-Path -Parent $MarkdownOutput
if ($jsonParent) { New-Item -ItemType Directory -Force -Path $jsonParent | Out-Null }
if ($markdownParent) { New-Item -ItemType Directory -Force -Path $markdownParent | Out-Null }
$comparison | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $JsonOutput -Encoding utf8

$lines = @(
    '# Editor Scene View Camera Performance Results',
    '',
    '| Configuration | Metric | Before | After | Change |',
    '| --- | --- | ---: | ---: | ---: |'
)
foreach ($configuration in @('Debug', 'Release')) {
    $entry = $comparison[$configuration]
    $lines += "| $configuration | Mean frame time | $([math]::Round($entry.before.meanFrameMs, 3)) ms | $([math]::Round($entry.after.meanFrameMs, 3)) ms | $([math]::Round($entry.change.meanFrameMsPercent, 2))% |"
    $lines += "| $configuration | Mean FPS | $([math]::Round($entry.before.meanFps, 2)) | $([math]::Round($entry.after.meanFps, 2)) | $([math]::Round($entry.change.meanFpsPercent, 2))% |"
    $lines += "| $configuration | P95 frame time | $([math]::Round($entry.before.p95FrameMs, 3)) ms | $([math]::Round($entry.after.p95FrameMs, 3)) ms | $([math]::Round($entry.change.p95FrameMsPercent, 2))% |"
    $lines += "| $configuration | P99 frame time | $([math]::Round($entry.before.p99FrameMs, 3)) ms | $([math]::Round($entry.after.p99FrameMs, 3)) ms | $([math]::Round($entry.change.p99FrameMsPercent, 2))% |"
    $lines += "| $configuration | Max frame time | $([math]::Round($entry.before.maxFrameMs, 3)) ms | $([math]::Round($entry.after.maxFrameMs, 3)) ms | $([math]::Round($entry.change.maxFrameMsPercent, 2))% |"
    $lines += "| $configuration | Publication ratio | $([math]::Round($entry.before.publicationRatio * 100.0, 2))% | $([math]::Round($entry.after.publicationRatio * 100.0, 2))% | $([math]::Round($entry.change.publicationRatioPoints, 2)) pp |"
}
$lines | Set-Content -LiteralPath $MarkdownOutput -Encoding utf8
