[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [Parameter(Mandatory = $true)]
    [string]$SourceAssetPath,

    [Parameter(Mandatory = $false)]
    [string]$SubAssetKey,

    [ValidateSet('Resident', 'Canonical')]
    [string]$CompletionMode = 'Resident',

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [ValidateRange(30, 1800)]
    [int]$CompletionTimeoutSeconds = 600,

    [ValidateRange(1, 120)]
    [int]$PostCompletionSeconds = 30,

    [ValidateRange(30, 1800)]
    [int]$StartupTimeoutSeconds = 240,

    [switch]$TriggerReimportAfterStartup
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

function Normalize-AssetPath([string]$path) {
    return $path.Replace('\', '/').TrimStart('./')
}

$normalizedSourceAssetPath = Normalize-AssetPath $SourceAssetPath
$sourceFilePath = [IO.Path]::GetFullPath((Join-Path $projectRoot $normalizedSourceAssetPath))
if ($TriggerReimportAfterStartup -and -not (Test-Path -LiteralPath $sourceFilePath -PathType Leaf)) {
    throw "Source asset does not exist for reimport trigger: $sourceFilePath"
}
if ([string]::IsNullOrWhiteSpace($SubAssetKey)) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($normalizedSourceAssetPath)
    $SubAssetKey = "prefab:$stem"
}
$validationFolder = Split-Path -Parent $normalizedSourceAssetPath
if ([string]::IsNullOrWhiteSpace($validationFolder)) {
    throw "SourceAssetPath must include an asset folder: $SourceAssetPath"
}

$outputRootFullPath = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Force -Path $outputRootFullPath | Out-Null
$runRoot = Join-Path $outputRootFullPath (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$cacheRoot = Join-Path $projectRoot "Library\AssetThumbnails\completion\$($runRoot | Split-Path -Leaf)"
$summaryPath = Join-Path $runRoot 'thumbnail-summary.txt'
$reportPath = Join-Path $runRoot 'completion-report.json'
New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null

function Get-LatestLog([string]$root, [DateTime]$startedAt) {
    return Get-ChildItem -LiteralPath (Join-Path $root 'Logs') -File -Filter '*.log' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $startedAt.AddSeconds(-1) } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Stop-Editor([Diagnostics.Process]$process) {
    if ($null -eq $process -or $process.HasExited) {
        return
    }
    try { $null = $process.CloseMainWindow() } catch { }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
    }
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
}

function Update-MemoryPeak([Diagnostics.Process]$process, [hashtable]$peak) {
    if ($null -eq $process -or $process.HasExited) { return }
    try {
        $process.Refresh()
        $peak.workingSetBytes = [math]::Max($peak.workingSetBytes, [int64]$process.WorkingSet64)
        $peak.privateBytes = [math]::Max($peak.privateBytes, [int64]$process.PrivateMemorySize64)
        $peak.pagedBytes = [math]::Max($peak.pagedBytes, [int64]$process.PagedMemorySize64)
    } catch { }
}

function Test-Png([IO.FileInfo]$file) {
    if ($null -eq $file -or -not $file.Exists -or $file.Length -lt 24) { return $false }
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    $magic = [byte[]](137, 80, 78, 71, 13, 10, 26, 10)
    for ($index = 0; $index -lt $magic.Length; ++$index) {
        if ($bytes[$index] -ne $magic[$index]) { return $false }
    }
    return $true
}

function Find-CanonicalRecord([string]$root, [string]$sourcePath, [string]$subAsset) {
    $matches = [System.Collections.Generic.List[object]]::new()
    foreach ($metadataFile in @(Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.json' -ErrorAction SilentlyContinue)) {
        try {
            $metadata = Get-Content -LiteralPath $metadataFile.FullName -Raw | ConvertFrom-Json
        } catch {
            continue
        }
        if ($null -eq $metadata) {
            continue
        }
        $metadataStatus = if ($metadata.PSObject.Properties.Name -contains 'status') {
            [string]$metadata.status
        } else {
            ''
        }
        $metadataSource = if ($metadata.PSObject.Properties.Name -contains 'sourceAssetPath') {
            [string]$metadata.sourceAssetPath
        } else {
            ''
        }
        $metadataSubAsset = if ($metadata.PSObject.Properties.Name -contains 'subAssetKey') {
            [string]$metadata.subAssetKey
        } else {
            ''
        }
        $metadataCacheKey = if ($metadata.PSObject.Properties.Name -contains 'cacheKey') {
            [string]$metadata.cacheKey
        } else {
            ''
        }
        if ($metadataSource -ne $sourcePath -or
            $metadataSubAsset -ne $subAsset -or
            $metadataStatus -ne 'fresh' -or
            $metadataCacheKey -eq '') {
            continue
        }
        $imageFile = Get-Item -LiteralPath ([IO.Path]::ChangeExtension($metadataFile.FullName, '.png')) -ErrorAction SilentlyContinue
        if ($null -eq $imageFile -or -not (Test-Png $imageFile)) { continue }
        $matches.Add([pscustomobject]@{
            metadataPath = $metadataFile.FullName
            imagePath = $imageFile.FullName
            cacheKey = $metadataCacheKey
            presentationKey = if ($metadata.PSObject.Properties.Name -contains 'presentationKey') {
                [string]$metadata.presentationKey
            } else {
                ''
            }
            requestedSize = if ($metadata.PSObject.Properties.Name -contains 'requestedSize') {
                [int]$metadata.requestedSize
            } else {
                0
            }
        })
    }
    return @($matches | Sort-Object metadataPath -Descending)
}

function Find-PresentationRecord([string]$root, [object]$canonical) {
    if ($null -eq $canonical -or [string]::IsNullOrWhiteSpace($canonical.presentationKey)) {
        return $null
    }
    $prefix = $canonical.presentationKey.Substring(0, [math]::Min(2, $canonical.presentationKey.Length))
    $indexPath = Join-Path (Join-Path $root 'presentation') "$prefix\$($canonical.presentationKey).json"
    if (-not (Test-Path -LiteralPath $indexPath -PathType Leaf)) { return $null }
    try { $index = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json } catch { return $null }
    if ($null -eq $index.current -or [string]$index.current.cacheKey -ne $canonical.cacheKey) { return $null }
    $currentImage = [string]$index.current.imagePath
    if (-not [IO.Path]::IsPathRooted($currentImage)) {
        $currentImage = Join-Path $root $currentImage
    }
    $currentImage = [IO.Path]::GetFullPath($currentImage)
    if (-not (Test-Path -LiteralPath $currentImage -PathType Leaf)) { return $null }
    return [pscustomobject]@{
        indexPath = $indexPath
        currentCacheKey = [string]$index.current.cacheKey
        currentImagePath = $currentImage
        committedRevision = [uint64]$index.committedRevision
    }
}

$editorPath = Join-Path $repoRoot "App\Win64_${Configuration}_Runtime_Shared\Editor.exe"
if (-not (Test-Path -LiteralPath $editorPath -PathType Leaf)) {
    throw "Editor executable does not exist: $editorPath"
}

$arguments = @(
    '--backend', 'dx12',
    '--editor-validation-asset-browser-folder', $validationFolder,
    '--editor-thumbnail-telemetry-summary', $summaryPath,
    '--editor-thumbnail-cache-root', $cacheRoot,
    '--editor-thumbnail-resident', '1',
    '--editor-thumbnail-proxy-pool', '0',
    '--editor-thumbnail-atlas', '0',
    # Durable thumbnail verification must keep the GPU readback ring enabled;
    # resident reuse is still isolated by disabling the proxy pool and atlas,
    # while the ring is the output path that turns the rendered resident frame
    # into canonical PNG metadata and the presentation index.
    '--editor-thumbnail-readback-ring', '1',
    # Keep lane routing enabled so a completed resident marker can move from
    # the light pump to the heavy continuation lane exactly as in production.
    # Adaptive budgeting remains disabled to keep the completion assertion
    # deterministic.
    '--editor-thumbnail-adaptive-budget', '0',
    '--editor-thumbnail-lanes', '1',
    $projectFile.FullName
)

$startedAt = Get-Date
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$process = Start-Process -FilePath $editorPath -ArgumentList $arguments -WorkingDirectory (Split-Path -Parent $editorPath) -WindowStyle Hidden -PassThru
$logFile = $null
$logText = ''
$windowShown = $false
$completion = $null
$memoryPeak = @{ workingSetBytes = [int64]0; privateBytes = [int64]0; pagedBytes = [int64]0 }
$escapedSource = [regex]::Escape($normalizedSourceAssetPath)
$escapedSubAsset = [regex]::Escape($SubAssetKey)
$completionPattern = "resident-prefab-preview-refresh\|.*source=$escapedSource\|subAsset=$escapedSubAsset\|drawItems=(?<drawItems>[0-9]+)\|resourceDrawItems=(?<resourceDrawItems>[0-9]+)\|sourceExpected=(?<sourceExpected>[0-9]+)\|complete=1"
$failureReason = $null
$originalSourceLastWriteTimeUtc = $null
$reimportTriggered = $false

try {
    while ($stopwatch.Elapsed.TotalSeconds -lt $StartupTimeoutSeconds) {
        Update-MemoryPeak $process $memoryPeak
        $candidate = Get-LatestLog $projectRoot $startedAt
        if ($null -ne $candidate) {
            $logFile = $candidate.FullName
            $logText = Get-Content -LiteralPath $logFile -Raw -ErrorAction SilentlyContinue
            if ($logText -match 'EditorWindowShown elapsedMs=') { $windowShown = $true; break }
            if ($logText -match 'Startup asset preimport failed|\[Editor::TryRun\]') {
                $failureReason = "Editor startup failed; see $logFile"
                break
            }
        }
        if ($process.HasExited) {
            $failureReason = "Editor exited before EditorWindowShown with code $($process.ExitCode)"
            break
        }
        Start-Sleep -Milliseconds 250
    }

    if (-not $windowShown -and $null -eq $failureReason) {
        $failureReason = "Editor startup timed out after $StartupTimeoutSeconds seconds"
    }

    if ($windowShown -and $null -eq $failureReason) {
        if ($TriggerReimportAfterStartup) {
            $originalSourceLastWriteTimeUtc = [IO.File]::GetLastWriteTimeUtc($sourceFilePath)
            [IO.File]::SetLastWriteTimeUtc($sourceFilePath, [DateTime]::UtcNow)
            $reimportTriggered = $true
        }
        $completionDeadline = [DateTime]::UtcNow.AddSeconds($CompletionTimeoutSeconds)
        while ([DateTime]::UtcNow -lt $completionDeadline) {
            Update-MemoryPeak $process $memoryPeak
            if ($CompletionMode -eq 'Canonical') {
                $canonicalProbe = @(Find-CanonicalRecord $cacheRoot $normalizedSourceAssetPath $SubAssetKey)
                if ($canonicalProbe.Count -gt 0 -and $null -ne (Find-PresentationRecord $cacheRoot $canonicalProbe[0])) {
                    break
                }
            }
            $logText = Get-Content -LiteralPath $logFile -Raw -ErrorAction SilentlyContinue
            $match = [regex]::Match($logText, $completionPattern)
            if ($CompletionMode -eq 'Resident' -and $match.Success) {
                $completion = [pscustomobject]@{
                    drawItems = [int]$match.Groups['drawItems'].Value
                    resourceDrawItems = [int]$match.Groups['resourceDrawItems'].Value
                    sourceExpected = [int]$match.Groups['sourceExpected'].Value
                    logLine = ($logText -split "`r?`n" | Where-Object { $_ -match $completionPattern } | Select-Object -Last 1)
                }
                # Resource completion is only the input-side milestone. Keep the
                # editor alive until the target has crossed the durable output
                # boundary, otherwise a valid resident snapshot can be mistaken
                # for a failed thumbnail simply because GPU readback/persistence
                # had not run yet.
                $canonicalDeadline = [DateTime]::UtcNow.AddSeconds($PostCompletionSeconds)
                while ([DateTime]::UtcNow -lt $canonicalDeadline) {
                    Update-MemoryPeak $process $memoryPeak
                    $canonicalProbe = @(Find-CanonicalRecord $cacheRoot $normalizedSourceAssetPath $SubAssetKey)
                    if ($canonicalProbe.Count -gt 0 -and $null -ne (Find-PresentationRecord $cacheRoot $canonicalProbe[0])) {
                        break
                    }
                    if ($process.HasExited) {
                        break
                    }
                    Start-Sleep -Milliseconds 250
                }
                break
            }
            if ($process.HasExited) {
                $failureReason = "Editor exited before $($CompletionMode.ToLowerInvariant()) completion with code $($process.ExitCode)"
                break
            }
            Start-Sleep -Milliseconds 250
        }
        if ($null -eq $failureReason) {
            $canonicalProbe = @(Find-CanonicalRecord $cacheRoot $normalizedSourceAssetPath $SubAssetKey)
            $canonicalPresentation = if ($canonicalProbe.Count -gt 0) {
                Find-PresentationRecord $cacheRoot $canonicalProbe[0]
            } else {
                $null
            }
            if ($CompletionMode -eq 'Resident' -and $null -eq $completion) {
                $failureReason = "Resident completion timed out after $CompletionTimeoutSeconds seconds"
            } elseif ($CompletionMode -eq 'Canonical' -and
                ($canonicalProbe.Count -eq 0 -or $null -eq $canonicalPresentation)) {
                $failureReason = "Canonical completion timed out after $CompletionTimeoutSeconds seconds"
            }
        }
    }
}
finally {
    Stop-Editor $process
    if ($reimportTriggered -and $null -ne $originalSourceLastWriteTimeUtc) {
        [IO.File]::SetLastWriteTimeUtc($sourceFilePath, $originalSourceLastWriteTimeUtc)
    }
    $stopwatch.Stop()
}

$summaryDeadline = [DateTime]::UtcNow.AddSeconds(15)
while (-not (Test-Path -LiteralPath $summaryPath -PathType Leaf) -and [DateTime]::UtcNow -lt $summaryDeadline) {
    Start-Sleep -Milliseconds 250
}

$canonical = @(Find-CanonicalRecord $cacheRoot $normalizedSourceAssetPath $SubAssetKey)
$presentation = if ($canonical.Count -gt 0) { Find-PresentationRecord $cacheRoot $canonical[0] } else { $null }
$residentCompletionValid = $null -ne $completion -and
    $completion.drawItems -eq $completion.sourceExpected -and
    $completion.resourceDrawItems -eq $completion.sourceExpected
$success = $null -eq $failureReason -and
    ($CompletionMode -eq 'Canonical' -or $residentCompletionValid) -and
    $canonical.Count -gt 0 -and
    $null -ne $presentation

$report = [ordered]@{
    success = $success
    failureReason = $failureReason
    sourceAssetPath = $normalizedSourceAssetPath
    subAssetKey = $SubAssetKey
    completionMode = $CompletionMode
    reimportTriggered = $reimportTriggered
    configuration = $Configuration
    projectRoot = $projectRoot
    editorPath = $editorPath
    cacheRoot = $cacheRoot
    summaryPath = $summaryPath
    logPath = $logFile
    completion = $completion
    canonical = $canonical
    presentation = $presentation
    elapsedToStopMs = [math]::Round($stopwatch.Elapsed.TotalMilliseconds, 3)
    peakWorkingSetBytes = $memoryPeak.workingSetBytes
    peakPrivateBytes = $memoryPeak.privateBytes
    peakPagedMemoryBytes = $memoryPeak.pagedBytes
}
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportPath -Encoding UTF8
Write-Output "RESULT_ROOT=$runRoot"
Write-Output "REPORT=$reportPath"
Write-Output "SUCCESS=$success"

if (-not $success) {
    throw "$CompletionMode thumbnail completion verification failed. See $reportPath"
}
