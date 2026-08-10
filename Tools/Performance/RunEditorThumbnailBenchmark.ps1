[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [Parameter(Mandatory = $false)]
    [string]$ResumeRoot,

    [ValidateRange(1, 5)]
    [int]$Trials = 5,

    [ValidateRange(5, 180)]
    [int]$WarmupSeconds = 5,

    [ValidateRange(10, 300)]
    [int]$MeasureSeconds = 30,

    [ValidateRange(30, 300)]
    [int]$StartupTimeoutSeconds = 180,

    [ValidateSet('Debug', 'Release')]
    [string[]]$Configurations = @('Debug', 'Release'),

    [ValidateSet('legacy', 'default', 'resident-adaptive', 'resident', 'proxy-pool', 'atlas', 'readback-ring', 'adaptive-budget', 'lanes', 'all-features')]
    [string[]]$Profiles = @('legacy', 'resident', 'proxy-pool', 'atlas', 'readback-ring', 'adaptive-budget', 'lanes', 'all-features'),

    [ValidateRange(0, 2147483647)]
    [int]$Seed = 20260801
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$projectInput = (Resolve-Path -LiteralPath $ProjectPath).Path
$projectRoot = if ((Get-Item -LiteralPath $projectInput).PSIsContainer) {
    $projectInput
} else {
    (Split-Path -Parent $projectInput)
}
$projectFile = if ((Get-Item -LiteralPath $projectInput).PSIsContainer) {
    Get-ChildItem -LiteralPath $projectRoot -File -Filter '*.nullus' | Select-Object -First 1
} else {
    Get-Item -LiteralPath $projectInput
}
if ($null -eq $projectFile) {
    throw "No .nullus project file found in $projectRoot"
}

$outputRootFullPath = [IO.Path]::GetFullPath($OutputRoot)
if ([string]::IsNullOrWhiteSpace($ResumeRoot)) {
    New-Item -ItemType Directory -Force -Path $outputRootFullPath | Out-Null
    $runRoot = Join-Path $outputRootFullPath (Get-Date -Format 'yyyyMMdd-HHmmss')
    New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
}
else {
    $runRoot = (Resolve-Path -LiteralPath $ResumeRoot -ErrorAction Stop).Path
    if (-not (Test-Path -LiteralPath $runRoot -PathType Container)) {
        throw "ResumeRoot is not a directory: $runRoot"
    }
}

function Get-LatestStartupLog([string]$root, [DateTime]$startedAt) {
    return Get-ChildItem -LiteralPath (Join-Path $root 'Logs') -File -Filter '*.log' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $startedAt.AddSeconds(-1) } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Stop-Editor([Diagnostics.Process]$process) {
    if ($null -eq $process -or $process.HasExited) {
        return
    }

    try {
        $null = $process.CloseMainWindow()
    }
    catch {
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
    }
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
}

function Get-LogValue([string]$text, [string]$pattern) {
    $match = [regex]::Match($text, $pattern)
    if (-not $match.Success) {
        return $null
    }
    return [double]::Parse($match.Groups['value'].Value, [Globalization.CultureInfo]::InvariantCulture)
}

function Get-SummaryValue([string]$text, [string]$key) {
    $escapedKey = [regex]::Escape($key)
    $match = [regex]::Match(
        $text,
        "(?m)(?:^|[ \t])$escapedKey=(?<value>n/a|[-+]?[0-9]+(?:\.[0-9]+)?)")
    if (-not $match.Success -or $match.Groups['value'].Value -eq 'n/a') {
        return $null
    }
    return [double]::Parse($match.Groups['value'].Value, [Globalization.CultureInfo]::InvariantCulture)
}

function Get-SummaryBoolean([string]$text, [string]$key) {
    $escapedKey = [regex]::Escape($key)
    $match = [regex]::Match($text, "(?m)(?:^|[ \t])$escapedKey=(?<value>true|false)")
    if (-not $match.Success) {
        return $null
    }
    return $match.Groups['value'].Value -eq 'true'
}

function Get-SummaryToken([string]$text, [string]$key) {
    $escapedKey = [regex]::Escape($key)
    $match = [regex]::Match($text, "(?m)(?:^|[ \t])$escapedKey=(?<value>[^\r\n]+)")
    if (-not $match.Success) {
        return $null
    }
    return $match.Groups['value'].Value.Trim()
}

function Get-StageMetric([string]$text, [string]$stage) {
    $escapedStage = [regex]::Escape($stage)
    $match = [regex]::Match(
        $text,
        "(?m)^-\s+$escapedStage records=(?<records>[0-9]+) totalMs=(?<totalMs>[-+]?[0-9]+(?:\.[0-9]+)?)(?: avgMs=[-+]?[0-9]+(?:\.[0-9]+)?)?(?: maxMs=[-+]?[0-9]+(?:\.[0-9]+)?)? totalBytes=(?<totalBytes>[0-9]+)")
    if (-not $match.Success) {
        return [pscustomobject]@{
            records = [int64]0
            totalMs = [double]0
            totalBytes = [int64]0
        }
    }
    return [pscustomobject]@{
        records = [int64]::Parse($match.Groups['records'].Value, [Globalization.CultureInfo]::InvariantCulture)
        totalMs = [double]::Parse($match.Groups['totalMs'].Value, [Globalization.CultureInfo]::InvariantCulture)
        totalBytes = [int64]::Parse($match.Groups['totalBytes'].Value, [Globalization.CultureInfo]::InvariantCulture)
    }
}

function Sum-StageMetrics([string]$text, [string[]]$stages) {
    $records = [int64]0
    $totalMs = [double]0
    $totalBytes = [int64]0
    foreach ($stage in $stages) {
        $metric = Get-StageMetric $text $stage
        $records += $metric.records
        $totalMs += $metric.totalMs
        $totalBytes += $metric.totalBytes
    }
    return [pscustomobject]@{
        records = $records
        totalMs = $totalMs
        totalBytes = $totalBytes
    }
}

function Read-ThumbnailSummaryMetrics([string]$summaryPath) {
    $text = Get-Content -LiteralPath $summaryPath -Raw -ErrorAction Stop
    $resourcePump = Sum-StageMetrics $text @(
        'ThumbnailGpuPreviewPumpDependencies',
        'ThumbnailGpuPreviewPumpMeshDependencies',
        'ThumbnailGpuPreviewPumpMeshInspection',
        'ThumbnailGpuPreviewPumpMaterialDependencies',
        'ThumbnailGpuPreviewPumpMaterialInspection',
        'ThumbnailGpuPreviewPumpTextureDependencies',
        'ThumbnailGpuPreviewPumpTextureBinding',
        'ThumbnailTexturePump')
    $meshInspection = Get-StageMetric $text 'ThumbnailGpuPreviewPumpMeshInspection'
    $materialInspection = Get-StageMetric $text 'ThumbnailGpuPreviewPumpMaterialInspection'
    $textureBinding = Get-StageMetric $text 'ThumbnailGpuPreviewPumpTextureBinding'
    $readback = Sum-StageMetrics $text @(
        'ThumbnailGpuPreviewReadback',
        'ThumbnailGpuPreviewPollReadback')
    $artifactFileRead = Get-StageMetric $text 'NativeArtifactFileRead'
    $artifactFileMap = Get-StageMetric $text 'NativeArtifactFileMap'
    $gpuSubmit = Sum-StageMetrics $text @(
        'ThumbnailGpuPreviewSubmit',
        'ThumbnailGpuPreviewRender')
    return [ordered]@{
        initialVisibleThumbnailCount = Get-SummaryValue $text 'initialVisibleThumbnailCount'
        initialVisibleCanonicalEligibleCount = Get-SummaryValue $text 'initialVisibleCanonicalEligibleCount'
        canonicalVisibleThumbnailCount = Get-SummaryValue $text 'canonicalVisibleThumbnailCount'
        initialVisibleLoadingCount = Get-SummaryValue $text 'initialVisibleLoadingCount'
        initialVisibleReadyCount = Get-SummaryValue $text 'initialVisibleReadyCount'
        initialVisibleFailedCount = Get-SummaryValue $text 'initialVisibleFailedCount'
        initialVisibleFallbackCount = Get-SummaryValue $text 'initialVisibleFallbackCount'
        initialVisibleAllTerminal = Get-SummaryBoolean $text 'initialVisibleAllTerminal'
        initialVisibleTimedOut = Get-SummaryBoolean $text 'initialVisibleTimedOut'
        initialVisibleSetFingerprint = Get-SummaryToken $text 'initialVisibleSetFingerprint'
        firstCanonicalDrawMs = Get-SummaryValue $text 'firstCanonicalDrawMs'
        canonical90PercentFillMs = Get-SummaryValue $text 'canonical90PercentFillMs'
        editorFrameP50Ms = Get-SummaryValue $text 'p50Ms'
        editorFrameP95Ms = Get-SummaryValue $text 'p95Ms'
        editorFrameP99Ms = Get-SummaryValue $text 'p99Ms'
        residentEntryCount = Get-SummaryValue $text 'residentEntryCount'
        residentActiveLeaseCount = Get-SummaryValue $text 'residentActiveLeaseCount'
        residentBytes = Get-SummaryValue $text 'residentBytes'
        residentHitCount = Get-SummaryValue $text 'residentHitCount'
        residentMissCount = Get-SummaryValue $text 'residentMissCount'
        residentStaleCount = Get-SummaryValue $text 'residentStaleCount'
        residentZeroArtifactReadHitCount = Get-SummaryValue $text 'residentZeroArtifactReadHitCount'
        residentEvictionCount = Get-SummaryValue $text 'residentEvictionCount'
        thumbnailResourcePumpRecords = $resourcePump.records
        thumbnailResourcePumpTotalMs = [math]::Round($resourcePump.totalMs, 3)
        thumbnailResourcePumpBytes = $resourcePump.totalBytes
        thumbnailResourceMeshInspectionRecords = $meshInspection.records
        thumbnailResourceMeshInspectionTotalMs = [math]::Round($meshInspection.totalMs, 3)
        thumbnailResourceMaterialInspectionRecords = $materialInspection.records
        thumbnailResourceMaterialInspectionTotalMs = [math]::Round($materialInspection.totalMs, 3)
        thumbnailResourceTextureBindingRecords = $textureBinding.records
        thumbnailResourceTextureBindingTotalMs = [math]::Round($textureBinding.totalMs, 3)
        thumbnailReadbackRecords = $readback.records
        thumbnailReadbackTotalMs = [math]::Round($readback.totalMs, 3)
        artifactFileReadRecords = $artifactFileRead.records
        artifactFileReadBytes = $artifactFileRead.totalBytes
        artifactFileMapRecords = $artifactFileMap.records
        artifactFileMapBytes = $artifactFileMap.totalBytes
        gpuPreviewSubmitRecords = $gpuSubmit.records
        gpuPreviewSubmitTotalMs = [math]::Round($gpuSubmit.totalMs, 3)
    }
}

function Update-ProcessMemoryPeak([Diagnostics.Process]$process, [hashtable]$peak) {
    if ($null -eq $process -or $process.HasExited) {
        return
    }
    try {
        $process.Refresh()
        $peak.workingSetBytes = [math]::Max($peak.workingSetBytes, [int64]$process.WorkingSet64)
        $peak.privateBytes = [math]::Max($peak.privateBytes, [int64]$process.PrivateMemorySize64)
        $peak.pagedBytes = [math]::Max($peak.pagedBytes, [int64]$process.PagedMemorySize64)
    }
    catch {
    }
}

function Get-ThumbnailFeatureConfig([string]$profile) {
    $config = [ordered]@{
        resident = $false
        proxyPool = $false
        atlas = $false
        readbackRing = $false
        adaptiveBudget = $false
        lanes = $false
    }

    switch ($profile) {
        'default' {
            $config.resident = $true
            $config.adaptiveBudget = $true
        }
        'resident-adaptive' {
            $config.resident = $true
            $config.adaptiveBudget = $true
        }
        'resident' { $config.resident = $true }
        'proxy-pool' { $config.proxyPool = $true }
        'atlas' { $config.atlas = $true }
        'readback-ring' { $config.readbackRing = $true }
        'adaptive-budget' { $config.adaptiveBudget = $true }
        'lanes' { $config.lanes = $true }
        'all-features' {
            foreach ($key in @($config.Keys)) { $config[$key] = $true }
        }
        'legacy' { }
        default { throw "Unsupported thumbnail benchmark profile: $profile" }
    }
    return $config
}

function Invoke-ThumbnailTrial(
    [string]$configuration,
    [string]$profile,
    [int]$trial,
    [System.Collections.IDictionary]$featureConfig,
    [int]$executionOrder) {

    $editorPath = Join-Path $repoRoot "App\Win64_${configuration}_Runtime_Shared\Editor.exe"
    if (-not (Test-Path -LiteralPath $editorPath -PathType Leaf)) {
        throw "Editor executable does not exist: $editorPath"
    }

    $cacheRoot = Join-Path $projectRoot "Library\AssetThumbnails\A-B\$($runRoot | Split-Path -Leaf)\${profile}-${configuration}-trial-${trial}"
    $summaryPath = Join-Path $runRoot "${profile}-${configuration}-trial-${trial}.summary.txt"
    New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null

    $arguments = @(
        '--backend', 'dx12',
        '--editor-validation-asset-browser-folder', 'Assets/main_sponza/main_sponza',
        '--editor-thumbnail-telemetry-summary', $summaryPath,
        '--editor-thumbnail-cache-root', $cacheRoot,
        '--editor-thumbnail-resident', $(if ($featureConfig.resident) { '1' } else { '0' }),
        '--editor-thumbnail-proxy-pool', $(if ($featureConfig.proxyPool) { '1' } else { '0' }),
        '--editor-thumbnail-atlas', $(if ($featureConfig.atlas) { '1' } else { '0' }),
        '--editor-thumbnail-readback-ring', $(if ($featureConfig.readbackRing) { '1' } else { '0' }),
        '--editor-thumbnail-adaptive-budget', $(if ($featureConfig.adaptiveBudget) { '1' } else { '0' }),
        '--editor-thumbnail-lanes', $(if ($featureConfig.lanes) { '1' } else { '0' }),
        $projectFile.FullName
    )

    $startedAt = Get-Date
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $editorPath `
        -ArgumentList $arguments `
        -WorkingDirectory (Split-Path -Parent $editorPath) `
        -PassThru
    $logFile = $null
    $logText = ''
    $windowShown = $false
    $memoryPeak = @{
        workingSetBytes = [int64]0
        privateBytes = [int64]0
        pagedBytes = [int64]0
    }
    try {
        while ($stopwatch.Elapsed.TotalSeconds -lt $StartupTimeoutSeconds) {
            Update-ProcessMemoryPeak $process $memoryPeak
            $candidate = Get-LatestStartupLog $projectRoot $startedAt
            if ($null -ne $candidate) {
                $logFile = $candidate.FullName
                $logText = Get-Content -LiteralPath $logFile -Raw -ErrorAction SilentlyContinue
                if ($logText -match 'EditorWindowShown elapsedMs=') {
                    $windowShown = $true
                    break
                }
                if ($logText -match 'Startup asset preimport failed|\[Editor::TryRun\]') {
                    throw "Editor startup failed; see $logFile"
                }
            }
            if ($process.HasExited) {
                throw "Editor exited before EditorWindowShown with code $($process.ExitCode)"
            }
            Start-Sleep -Milliseconds 250
        }

        if (-not $windowShown) {
            throw "Editor startup timed out after $StartupTimeoutSeconds seconds"
        }

        $measurementDeadline = [DateTime]::UtcNow.AddSeconds($WarmupSeconds + $MeasureSeconds)
        while ([DateTime]::UtcNow -lt $measurementDeadline) {
            Update-ProcessMemoryPeak $process $memoryPeak
            Start-Sleep -Milliseconds 100
        }
    }
    finally {
        Stop-Editor $process
        $stopwatch.Stop()
    }

    $summaryDeadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path -LiteralPath $summaryPath -PathType Leaf) -and [DateTime]::UtcNow -lt $summaryDeadline) {
        Start-Sleep -Milliseconds 250
    }
    if (-not (Test-Path -LiteralPath $summaryPath -PathType Leaf)) {
        throw "Editor produced no thumbnail telemetry summary: $summaryPath"
    }

    $summaryMetrics = Read-ThumbnailSummaryMetrics $summaryPath

    if ($null -eq $logFile) {
        $candidate = Get-LatestStartupLog $projectRoot $startedAt
        if ($null -ne $candidate) {
            $logFile = $candidate.FullName
            $logText = Get-Content -LiteralPath $logFile -Raw -ErrorAction SilentlyContinue
        }
    }

    [ordered]@{
        profile = $profile
        configuration = $configuration
        trial = $trial
        seed = $Seed
        executionOrder = $executionOrder
        featureConfig = [ordered]@{
            resident = [bool]$featureConfig.resident
            proxyPool = [bool]$featureConfig.proxyPool
            atlas = [bool]$featureConfig.atlas
            readbackRing = [bool]$featureConfig.readbackRing
            adaptiveBudget = [bool]$featureConfig.adaptiveBudget
            lanes = [bool]$featureConfig.lanes
        }
        projectRoot = $projectRoot
        editorPath = $editorPath
        cacheRoot = $cacheRoot
        summaryPath = $summaryPath
        summaryMetrics = $summaryMetrics
        logPath = $logFile
        processToWindowMs = Get-LogValue $logText 'EditorWindowShown elapsedMs=(?<value>[0-9]+)'
        elapsedToStopMs = [math]::Round($stopwatch.Elapsed.TotalMilliseconds, 3)
        peakWorkingSetBytes = $memoryPeak.workingSetBytes
        peakPrivateBytes = $memoryPeak.privateBytes
        peakPagedMemoryBytes = $memoryPeak.pagedBytes
    }
}

function Read-ExistingThumbnailTrial(
    [string]$configuration,
    [string]$profile,
    [int]$trial,
    [System.Collections.IDictionary]$featureConfig) {

    $cacheRoot = Join-Path $projectRoot "Library\AssetThumbnails\A-B\$($runRoot | Split-Path -Leaf)\${profile}-${configuration}-trial-${trial}"
    $summaryPath = Join-Path $runRoot "${profile}-${configuration}-trial-${trial}.summary.txt"
    if (-not (Test-Path -LiteralPath $summaryPath -PathType Leaf)) {
        throw "Existing thumbnail trial summary does not exist: $summaryPath"
    }

    [ordered]@{
        profile = $profile
        configuration = $configuration
        trial = $trial
        seed = $Seed
        executionOrder = $null
        resumed = $true
        featureConfig = [ordered]@{
            resident = [bool]$featureConfig.resident
            proxyPool = [bool]$featureConfig.proxyPool
            atlas = [bool]$featureConfig.atlas
            readbackRing = [bool]$featureConfig.readbackRing
            adaptiveBudget = [bool]$featureConfig.adaptiveBudget
            lanes = [bool]$featureConfig.lanes
        }
        projectRoot = $projectRoot
        editorPath = Join-Path $repoRoot "App\Win64_${configuration}_Runtime_Shared\Editor.exe"
        cacheRoot = $cacheRoot
        summaryPath = $summaryPath
        summaryMetrics = Read-ThumbnailSummaryMetrics $summaryPath
        logPath = $null
        processToWindowMs = $null
        elapsedToStopMs = $null
        peakWorkingSetBytes = $null
        peakPrivateBytes = $null
        peakPagedMemoryBytes = $null
    }
}

$records = [System.Collections.Generic.List[object]]::new()
$rawPath = Join-Path $runRoot 'raw.json'
$workItems = [System.Collections.Generic.List[object]]::new()
foreach ($configuration in $Configurations) {
    foreach ($profile in $Profiles) {
        for ($trial = 1; $trial -le $Trials; ++$trial) {
            $workItems.Add([pscustomobject]@{
                configuration = $configuration
                profile = $profile
                trial = $trial
            })
        }
    }
}

$random = [System.Random]::new($Seed)
for ($index = $workItems.Count - 1; $index -gt 0; --$index) {
    $swapIndex = $random.Next($index + 1)
    $temporary = $workItems[$index]
    $workItems[$index] = $workItems[$swapIndex]
    $workItems[$swapIndex] = $temporary
}

$executionOrder = 0
foreach ($workItem in $workItems) {
    ++$executionOrder
    $featureConfig = Get-ThumbnailFeatureConfig $workItem.profile
    $summaryPath = Join-Path $runRoot "$($workItem.profile)-$($workItem.configuration)-trial-$($workItem.trial).summary.txt"
    if (Test-Path -LiteralPath $summaryPath -PathType Leaf) {
        $record = Read-ExistingThumbnailTrial `
            $workItem.configuration `
            $workItem.profile `
            $workItem.trial `
            $featureConfig
        Write-Output ("{0} {1} trial {2}: reused summary={3}" -f $workItem.profile, $workItem.configuration, $workItem.trial, $summaryPath)
    }
    else {
        $record = Invoke-ThumbnailTrial `
            $workItem.configuration `
            $workItem.profile `
            $workItem.trial `
            $featureConfig `
            $executionOrder
        Write-Output ("{0} {1} trial {2}: summary={3}" -f $workItem.profile, $workItem.configuration, $workItem.trial, $record.summaryPath)
    }
    $records.Add($record)
    $records | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $rawPath -Encoding UTF8
    Start-Sleep -Seconds 3
}

$records | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $rawPath -Encoding UTF8
$consistencyByConfiguration = [System.Collections.Generic.List[object]]::new()
foreach ($configurationGroup in @($records | Group-Object { $_['configuration'] })) {
    $fingerprints = @(
        $configurationGroup.Group |
            ForEach-Object { $_.summaryMetrics.initialVisibleSetFingerprint } |
            Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) } |
            Sort-Object -Unique)
    $missingCount = @(
        $configurationGroup.Group |
            Where-Object { [string]::IsNullOrWhiteSpace([string]$_.summaryMetrics.initialVisibleSetFingerprint) }
    ).Count
    $canonicalEligibleCounts = @(
        $configurationGroup.Group |
            ForEach-Object { [string]$_.summaryMetrics.initialVisibleCanonicalEligibleCount } |
            Where-Object { $_ -ne '' } |
            Sort-Object -Unique
    )
    $consistencyByConfiguration.Add([ordered]@{
        configuration = $configurationGroup.Name
        recordCount = @($configurationGroup.Group).Count
        fingerprintCount = $fingerprints.Count
        fingerprints = $fingerprints
        missingFingerprintCount = $missingCount
        canonicalEligibleCounts = $canonicalEligibleCounts
        canonicalEligibleCountConsistent = $canonicalEligibleCounts.Count -eq 1
        validForComparison = $fingerprints.Count -eq 1 -and
            $missingCount -eq 0 -and
            $canonicalEligibleCounts.Count -eq 1
    })
}
$visibleSetConsistency = [ordered]@{
    validForComparison = @($consistencyByConfiguration | Where-Object { -not $_.validForComparison }).Count -eq 0
    configurations = $consistencyByConfiguration
}
$consistencyPath = Join-Path $runRoot 'visible-set-consistency.json'
$visibleSetConsistency | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $consistencyPath -Encoding UTF8
Write-Output "RESULT_ROOT=$runRoot"
Write-Output "RAW_RESULTS=$rawPath"
Write-Output "VISIBLE_SET_CONSISTENCY=$consistencyPath"
Write-Output ("VISIBLE_SET_COMPARABLE={0}" -f $visibleSetConsistency.validForComparison)
