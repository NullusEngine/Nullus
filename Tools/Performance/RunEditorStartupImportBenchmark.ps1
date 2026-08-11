[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [Parameter(Mandatory = $true)]
    [string]$TemplateProject,

    [Parameter(Mandatory = $true)]
    [string]$BeforeEditorRoot,

    [Parameter(Mandatory = $true)]
    [string]$AfterEditorRoot,

    [ValidateRange(1, 20)]
    [int]$Trials = 3,

    [ValidateRange(30, 600)]
    [int]$StartupTimeoutSeconds = 120,

    [switch]$CacheHitOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$TemplateProject = [IO.Path]::GetFullPath($TemplateProject)
$BeforeEditorRoot = [IO.Path]::GetFullPath($BeforeEditorRoot)
$AfterEditorRoot = [IO.Path]::GetFullPath($AfterEditorRoot)

if (-not (Test-Path -LiteralPath $TemplateProject -PathType Container)) {
    throw "Template project directory does not exist: $TemplateProject"
}

New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$runTag = Get-Date -Format 'yyyyMMdd-HHmmss'
$RunRoot = Join-Path $OutputRoot $runTag
New-Item -ItemType Directory -Path $RunRoot -Force | Out-Null
$ProjectRoot = Join-Path $RunRoot 'projects'
$LogRoot = Join-Path $RunRoot 'logs'
New-Item -ItemType Directory -Path $ProjectRoot,$LogRoot -Force | Out-Null

function Get-ProjectFile([string]$projectRoot) {
    $projectFile = Get-ChildItem -LiteralPath $projectRoot -File -Filter '*.nullus' | Select-Object -First 1
    if ($null -eq $projectFile) {
        throw "No .nullus project file found in $projectRoot"
    }
    return $projectFile.FullName
}

function Get-RegexValue([string]$text, [string]$pattern) {
    $match = [regex]::Match($text, $pattern, [Text.RegularExpressions.RegexOptions]::Multiline)
    if (-not $match.Success) {
        return $null
    }
    return [double]::Parse($match.Groups['value'].Value, [Globalization.CultureInfo]::InvariantCulture)
}

function Get-RegexInt([string]$text, [string]$pattern) {
    $match = [regex]::Match($text, $pattern, [Text.RegularExpressions.RegexOptions]::Multiline)
    if (-not $match.Success) {
        return $null
    }
    return [int]::Parse($match.Groups['value'].Value, [Globalization.CultureInfo]::InvariantCulture)
}

function Get-LatestLog([string]$projectRoot) {
    $logs = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'Logs') -File -Filter '*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    if ($logs.Count -eq 0) {
        return $null
    }
    return $logs[0]
}

function Stop-BenchmarkProcess([Diagnostics.Process]$process) {
    if ($null -eq $process -or $process.HasExited) {
        return
    }

    try {
        $null = $process.CloseMainWindow()
    }
    catch {
    }

    $closeDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $process.HasExited -and [DateTime]::UtcNow -lt $closeDeadline) {
        Start-Sleep -Milliseconds 100
    }

    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
}

function Invoke-StartupTrial(
    [string]$editorPath,
    [string]$projectRoot,
    [string]$version,
    [string]$configuration,
    [string]$scenario,
    [int]$trial,
    [bool]$measured) {

    $projectFile = Get-ProjectFile $projectRoot
    $process = $null
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $logFile = $null
    $logText = ''
    $windowShown = $false
    $cacheReady = $scenario -ne 'warm-prime'
    $failureReason = $null
    $exitCode = $null

    try {
        # Let the previous DX12 process release its device and runtime DLLs before the next trial.
        Start-Sleep -Seconds 3
        $process = Start-Process -FilePath $editorPath `
            -ArgumentList @('--backend', 'dx12', $projectFile) `
            -WorkingDirectory (Split-Path -Parent $editorPath) `
            -PassThru

        while ($stopwatch.Elapsed.TotalSeconds -lt $StartupTimeoutSeconds) {
            $candidate = Get-LatestLog $projectRoot
            if ($null -ne $candidate) {
                $logFile = $candidate.FullName
                $logText = Get-Content -LiteralPath $logFile -Raw -ErrorAction SilentlyContinue
                if ($logText -match 'EditorWindowShown elapsedMs=') {
                    $windowShown = $true
                }
                if ($scenario -eq 'warm-prime' -and
                    $logText -match '\[StartupAssetPreimport\] Startup asset cache (?:hit|index rebuild) took') {
                    $cacheReady = $true
                }
                if ($windowShown -and $cacheReady) {
                    break
                }
                if ($logText -match 'Startup asset preimport failed|\[Editor::TryRun\]') {
                    $failureReason = 'startup failure reported in log'
                    break
                }
            }

            if ($process.HasExited) {
                $exitCode = $process.ExitCode
                $failureReason = 'editor exited before EditorWindowShown'
                break
            }
            Start-Sleep -Milliseconds 250
        }

        if ((-not $windowShown -or -not $cacheReady) -and $null -eq $failureReason) {
            $failureReason = "startup timeout after $StartupTimeoutSeconds seconds"
        }

        if ($windowShown -and $cacheReady) {
            Stop-BenchmarkProcess $process
        }
        elseif ($null -ne $process -and -not $process.HasExited) {
            Stop-BenchmarkProcess $process
        }

        if ($null -ne $process -and $process.HasExited) {
            $exitCode = $process.ExitCode
        }
    }
    finally {
        $stopwatch.Stop()
        if ($null -ne $process -and -not $process.HasExited) {
            Stop-BenchmarkProcess $process
        }
    }

    if ($null -eq $logFile) {
        $candidate = Get-LatestLog $projectRoot
        if ($null -ne $candidate) {
            $logFile = $candidate.FullName
            $logText = Get-Content -LiteralPath $logFile -Raw -ErrorAction SilentlyContinue
        }
    }

    $record = [ordered]@{
        version = $version
        configuration = $configuration
        scenario = $scenario
        trial = $trial
        measured = $measured
        projectRoot = $projectRoot
        logPath = $logFile
        editorPath = $editorPath
        success = $windowShown -and $cacheReady
        failureReason = $failureReason
        exitCode = $exitCode
        processToWindowMs = [math]::Round($stopwatch.Elapsed.TotalMilliseconds, 3)
        windowShownElapsedMs = Get-RegexValue $logText 'EditorWindowShown elapsedMs=(?<value>[0-9]+)'
        cacheHit = $logText -match '\[StartupAssetPreimport\] Startup asset cache hit'
        cacheValidationMs = Get-RegexValue $logText 'Startup asset cache (?:hit|miss analysis) took (?<value>[0-9]+) ms'
        cacheIndexRebuildMs = Get-RegexValue $logText 'Startup asset cache index rebuild took (?<value>[0-9]+) ms'
        databaseRefreshMs = Get-RegexValue $logText 'Startup asset database refresh took (?<value>[0-9]+) ms'
        importPlanningMs = Get-RegexValue $logText 'Startup asset import planning took (?<value>[0-9]+) ms'
        importExecutionMs = Get-RegexValue $logText 'Startup asset import execution took (?<value>[0-9]+) ms'
        plannedAssetCount = Get-RegexInt $logText 'Startup asset import planning took [0-9]+ ms for (?<value>[0-9]+) items'
        importedAssetCount = Get-RegexInt $logText 'Startup asset preimport completed: (?<value>[0-9]+) imported / [0-9]+ planned'
    }

    $record
}

function Invoke-StartupTrialWithRetry(
    [string]$editorPath,
    [string]$projectRoot,
    [string]$version,
    [string]$configuration,
    [string]$scenario,
    [int]$trial,
    [bool]$measured) {

    $record = $null
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $record = Invoke-StartupTrial $editorPath $projectRoot $version $configuration $scenario $trial $measured
        if ($record.success) {
            return $record
        }

        $retryable =
            $record.failureReason -eq 'editor exited before EditorWindowShown' -and
            ($record.exitCode -eq -1073741502 -or $null -eq $record.logPath)
        if (-not $retryable -or $attempt -eq 3) {
            return $record
        }
        Start-Sleep -Seconds 5
    }
    return $record
}

function Copy-BenchmarkProject([string]$targetRoot) {
    if (Test-Path -LiteralPath $targetRoot) {
        throw "Benchmark project already exists: $targetRoot"
    }

    New-Item -ItemType Directory -Path $targetRoot | Out-Null
    Get-ChildItem -LiteralPath $TemplateProject -Force |
        Where-Object { $_.Name -ne 'Logs' } |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $targetRoot -Recurse
        }
}

function Save-BenchmarkTrialLog([object]$record) {
    if ([string]::IsNullOrWhiteSpace($record.logPath) -or
        -not (Test-Path -LiteralPath $record.logPath -PathType Leaf)) {
        return
    }

    $archiveName = '{0}-{1}-{2}-{3}.log' -f `
        $record.version, $record.configuration, $record.scenario, $record.trial
    $archivePath = Join-Path $LogRoot $archiveName
    Copy-Item -LiteralPath $record.logPath -Destination $archivePath -Force
    $record.logPath = $archivePath
}

function Remove-BenchmarkProject([string]$targetRoot) {
    $normalizedProjectRoot = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    $normalizedTargetRoot = [IO.Path]::GetFullPath($targetRoot)
    if (-not $normalizedTargetRoot.StartsWith($normalizedProjectRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a benchmark project outside the run root: $normalizedTargetRoot"
    }
    if (Test-Path -LiteralPath $normalizedTargetRoot -PathType Container) {
        Remove-Item -LiteralPath $normalizedTargetRoot -Recurse -Force
    }
}

function Save-StartupCacheIndexSeed([string]$projectRoot, [string]$seedPath) {
    $stampPath = Join-Path $projectRoot 'Library\Editor\StartupAssetPreimport.stamp'
    if (-not (Test-Path -LiteralPath $stampPath -PathType Leaf)) {
        return $false
    }

    New-Item -ItemType Directory -Path (Split-Path -Parent $seedPath) -Force | Out-Null
    Copy-Item -LiteralPath $stampPath -Destination $seedPath -Force
    return $true
}

function Restore-StartupCacheIndexSeed([string]$projectRoot, [string]$seedPath, [bool]$hasSeed) {
    $stampPath = Join-Path $projectRoot 'Library\Editor\StartupAssetPreimport.stamp'
    if ($hasSeed) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $stampPath) -Force | Out-Null
        Copy-Item -LiteralPath $seedPath -Destination $stampPath -Force
    }
    elseif (Test-Path -LiteralPath $stampPath -PathType Leaf) {
        Remove-Item -LiteralPath $stampPath -Force
    }
}

function Prepare-BenchmarkProjectForCacheHit([string]$projectRoot) {
    $stampPath = Join-Path $projectRoot 'Library\Editor\StartupAssetPreimport.stamp'
    if (-not (Test-Path -LiteralPath $stampPath -PathType Leaf)) {
        throw "Startup cache index does not exist in benchmark project: $stampPath"
    }

    $normalizedProjectRoot = [IO.Path]::GetFullPath($projectRoot).Replace('\', '/')
    $stampBytes = [IO.File]::ReadAllBytes($stampPath)
    $binaryMagic = [Text.Encoding]::ASCII.GetBytes('NLSSPIDX')
    $isBinaryIndex = $stampBytes.Length -ge $binaryMagic.Length
    if ($isBinaryIndex) {
        for ($index = 0; $index -lt $binaryMagic.Length; $index++) {
            if ($stampBytes[$index] -ne $binaryMagic[$index]) {
                $isBinaryIndex = $false
                break
            }
        }
    }

    if (-not $isBinaryIndex) {
        $stampText = [Text.Encoding]::UTF8.GetString($stampBytes)
        $replacement = 'projectRoot "' + $normalizedProjectRoot + '"'
        $updatedStampText = [regex]::Replace(
            $stampText,
            '(?m)^projectRoot\s+"[^"]*"\r?$',
            $replacement)
        if ($updatedStampText -eq $stampText) {
            throw "Startup cache index has no projectRoot entry: $stampPath"
        }

        [IO.File]::WriteAllText($stampPath, $updatedStampText, [Text.UTF8Encoding]::new($false))
        return
    }

    # Binary layout starts with magic + schema version + a length-prefixed stamp version.
    $uint32Size = 4
    $offset = $binaryMagic.Length + $uint32Size
    if ($offset + $uint32Size -gt $stampBytes.Length) {
        throw "Binary startup cache index is truncated before stampVersion: $stampPath"
    }
    $stampVersionLength = [BitConverter]::ToUInt32($stampBytes, $offset)
    $offset += $uint32Size + $stampVersionLength
    if ($offset + $uint32Size -gt $stampBytes.Length) {
        throw "Binary startup cache index is truncated before projectRoot: $stampPath"
    }

    $projectRootLengthOffset = $offset
    $oldProjectRootLength = [BitConverter]::ToUInt32($stampBytes, $projectRootLengthOffset)
    $projectRootOffset = $projectRootLengthOffset + $uint32Size
    $projectRootEnd = $projectRootOffset + $oldProjectRootLength
    if ($projectRootEnd -gt $stampBytes.Length) {
        throw "Binary startup cache index has an invalid projectRoot length: $stampPath"
    }

    $newProjectRootBytes = [Text.Encoding]::UTF8.GetBytes($normalizedProjectRoot)
    $updatedStampBytes = [Collections.Generic.List[byte]]::new(
        $stampBytes.Length - $oldProjectRootLength + $newProjectRootBytes.Length)
    $updatedStampBytes.AddRange([byte[]]($stampBytes[0..($projectRootLengthOffset - 1)]))
    $updatedStampBytes.AddRange([BitConverter]::GetBytes([UInt32]$newProjectRootBytes.Length))
    $updatedStampBytes.AddRange($newProjectRootBytes)
    if ($projectRootEnd -lt $stampBytes.Length) {
        $updatedStampBytes.AddRange([byte[]]($stampBytes[$projectRootEnd..($stampBytes.Length - 1)]))
    }

    [IO.File]::WriteAllBytes($stampPath, $updatedStampBytes.ToArray())
}

function Get-Percentile([double[]]$values, [double]$percentile) {
    $sorted = @($values | Sort-Object)
    if ($sorted.Count -eq 0) {
        return $null
    }
    $rank = [math]::Ceiling($percentile * $sorted.Count) - 1
    $rank = [math]::Max(0, [math]::Min($rank, $sorted.Count - 1))
    return [math]::Round($sorted[$rank], 3)
}

function Get-Stats([object[]]$records, [string]$property) {
    $values = @($records | Where-Object { $_.success -and $null -ne $_.$property } | ForEach-Object { [double]$_.$property })
    if ($values.Count -eq 0) {
        return [ordered]@{ count = 0; meanMs = $null; p95Ms = $null; p99Ms = $null; rawMs = @() }
    }
    return [ordered]@{
        count = $values.Count
        meanMs = [math]::Round(($values | Measure-Object -Average).Average, 3)
        p95Ms = Get-Percentile $values 0.95
        p99Ms = Get-Percentile $values 0.99
        rawMs = @($values | ForEach-Object { [math]::Round($_, 3) })
    }
}

$editorRoots = [ordered]@{
    before = $BeforeEditorRoot
    after = $AfterEditorRoot
}
$records = [System.Collections.Generic.List[object]]::new()

foreach ($version in @('before', 'after')) {
    foreach ($configuration in @('Debug', 'Release')) {
        $editorPath = Join-Path $editorRoots[$version] "Win64_${configuration}_Runtime_Shared\Editor.exe"
        if (-not (Test-Path -LiteralPath $editorPath -PathType Leaf)) {
            throw "Editor executable does not exist: $editorPath"
        }

        $benchmarkProjectRoot = Join-Path $ProjectRoot "${version}-${configuration}"
        $cacheSeedPath = Join-Path $RunRoot "cache-seeds\${version}-${configuration}.stamp"
        try {
            Copy-BenchmarkProject $benchmarkProjectRoot
            if ($CacheHitOnly) {
                Prepare-BenchmarkProjectForCacheHit $benchmarkProjectRoot
            }
            else {
                $hasCacheSeed = Save-StartupCacheIndexSeed $benchmarkProjectRoot $cacheSeedPath
                for ($trial = 1; $trial -le $Trials; $trial++) {
                    Restore-StartupCacheIndexSeed $benchmarkProjectRoot $cacheSeedPath $hasCacheSeed
                    $record = Invoke-StartupTrialWithRetry $editorPath $benchmarkProjectRoot $version $configuration 'cold' $trial $true
                    Save-BenchmarkTrialLog $record
                    $records.Add($record)
                }

                Restore-StartupCacheIndexSeed $benchmarkProjectRoot $cacheSeedPath $hasCacheSeed
            }

            $primeTrial = 0
            $primeRecord = $null
            do {
                $primeTrial++
                $primeRecord = Invoke-StartupTrialWithRetry $editorPath $benchmarkProjectRoot $version $configuration 'warm-prime' $primeTrial $false
                Save-BenchmarkTrialLog $primeRecord
                $records.Add($primeRecord)
            } while (-not $primeRecord.cacheHit -and $primeTrial -lt 5)

            if (-not $primeRecord.cacheHit) {
                throw "Warm benchmark project never reached a cache hit: $benchmarkProjectRoot"
            }

            for ($trial = 1; $trial -le $Trials; $trial++) {
                $record = Invoke-StartupTrialWithRetry $editorPath $benchmarkProjectRoot $version $configuration 'cache-hit' $trial $true
                Save-BenchmarkTrialLog $record
                $records.Add($record)
            }
        }
        finally {
            Remove-BenchmarkProject $benchmarkProjectRoot
        }
    }
}

$measuredRecords = @($records | Where-Object { $_.measured })
$measuredScenarios = if ($CacheHitOnly) { @('cache-hit') } else { @('cold', 'cache-hit') }
$summary = [System.Collections.Generic.List[object]]::new()
foreach ($version in @('before', 'after')) {
    foreach ($configuration in @('Debug', 'Release')) {
        foreach ($scenario in $measuredScenarios) {
            $group = @($measuredRecords | Where-Object {
                $_.version -eq $version -and $_.configuration -eq $configuration -and $_.scenario -eq $scenario
            })
            $summary.Add([ordered]@{
                version = $version
                configuration = $configuration
                scenario = $scenario
                startup = Get-Stats $group 'windowShownElapsedMs'
                processToWindow = Get-Stats $group 'processToWindowMs'
                cacheValidation = Get-Stats $group 'cacheValidationMs'
                cacheIndexRebuild = Get-Stats $group 'cacheIndexRebuildMs'
                databaseRefresh = Get-Stats $group 'databaseRefreshMs'
                importPlanning = Get-Stats $group 'importPlanningMs'
                importExecution = Get-Stats $group 'importExecutionMs'
            })
        }
    }
}

$rawPath = Join-Path $RunRoot 'raw.json'
$summaryPath = Join-Path $RunRoot 'summary.json'
$records | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $rawPath -Encoding UTF8
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Output "RESULT_ROOT=$RunRoot"
Write-Output "RAW_RESULTS=$rawPath"
Write-Output "SUMMARY_RESULTS=$summaryPath"
$summary | ForEach-Object {
    Write-Output ("{0} {1} {2}: mean={3}ms p95={4}ms p99={5}ms" -f `
        $_.version, $_.configuration, $_.scenario, $_.startup.meanMs, $_.startup.p95Ms, $_.startup.p99Ms)
}
