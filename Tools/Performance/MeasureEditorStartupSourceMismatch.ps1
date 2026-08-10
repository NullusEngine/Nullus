[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot,

    [Parameter(Mandatory = $true)]
    [string]$EditorPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [ValidateRange(30, 120)]
    [int]$StartupTimeoutSeconds = 60
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-UInt32At {
    param([byte[]]$Buffer, [ref]$Position)

    if ($Position.Value + 4 -gt $Buffer.Length) {
        throw 'The startup cache index is truncated while reading a uint32.'
    }

    $value = [BitConverter]::ToUInt32($Buffer, $Position.Value)
    $Position.Value += 4
    return $value
}

function Skip-IndexString {
    param([byte[]]$Buffer, [ref]$Position)

    $length = Read-UInt32At $Buffer $Position
    if ($length -gt 16MB -or $Position.Value + [int64]$length -gt $Buffer.Length) {
        throw 'The startup cache index is truncated while reading a string.'
    }

    $Position.Value += [int]$length
}

function Mutate-IndexStringFirstByte {
    param([byte[]]$Buffer, [ref]$Position)

    $length = Read-UInt32At $Buffer $Position
    if ($length -gt 16MB -or $Position.Value + [int64]$length -gt $Buffer.Length) {
        throw 'The startup cache index is truncated while mutating a string.'
    }

    if ($length -gt 0) {
        # Preserve the serialized length. The value must differ from the real metadata,
        # but it does not need to be a syntactically valid stamp or fingerprint.
        $Buffer[$Position.Value] = if ($Buffer[$Position.Value] -eq 126) { 33 } else { 126 }
    }

    $Position.Value += [int]$length
}

function Stop-EditorProcess {
    param([Diagnostics.Process]$Process)

    if ($null -eq $Process -or $Process.HasExited) {
        return
    }

    try {
        $null = $Process.CloseMainWindow()
    }
    catch {
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $Process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }

    if (-not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force
    }
}

$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$EditorPath = [IO.Path]::GetFullPath($EditorPath)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$projectFile = Get-ChildItem -LiteralPath $ProjectRoot -File -Filter '*.nullus' | Select-Object -First 1
if ($null -eq $projectFile) {
    throw "No .nullus project file found in $ProjectRoot"
}

$stampPath = Join-Path $ProjectRoot 'Library\Editor\StartupAssetPreimport.stamp'
if (-not (Test-Path -LiteralPath $stampPath -PathType Leaf)) {
    throw "Startup cache index does not exist: $stampPath"
}
if (-not (Test-Path -LiteralPath $EditorPath -PathType Leaf)) {
    throw "Editor executable does not exist: $EditorPath"
}
if (Get-Process Editor -ErrorAction SilentlyContinue) {
    throw 'An Editor process is already running; refusing to modify the startup cache index.'
}

$runTag = Get-Date -Format 'yyyyMMdd-HHmmss'
$runRoot = Join-Path $OutputRoot $runTag
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
$backupPath = Join-Path $runRoot 'StartupAssetPreimport.original.stamp'
Copy-Item -LiteralPath $stampPath -Destination $backupPath -Force

$result = [ordered]@{
    runTag = $runTag
    projectRoot = $ProjectRoot
    editorPath = $EditorPath
    sourceEntryCount = 0
    mutatedSourceEntries = 0
    windowShown = $false
    logPath = $null
    cacheValidationMs = $null
    importPlanningMs = $null
    importExecutionMs = $null
    indexPatchMs = $null
    editorWindowShownMs = $null
    plannedAssetCount = $null
    importedAssetCount = $null
}

$editorProcess = $null
$logFile = $null
try {
    $bytes = [IO.File]::ReadAllBytes($stampPath)
    if ($bytes.Length -lt 12 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 8) -ne 'NLSSPIDX') {
        throw 'Expected an NLSSPIDX binary startup cache index.'
    }

    $offset = 12 # magic plus schema version
    for ($headerStringIndex = 0; $headerStringIndex -lt 4; $headerStringIndex++) {
        Skip-IndexString $bytes ([ref]$offset)
    }

    $sourceCount = [BitConverter]::ToUInt64($bytes, $offset); $offset += 8
    $sourceDirectoryCount = [BitConverter]::ToUInt64($bytes, $offset); $offset += 8
    $dependencyCount = [BitConverter]::ToUInt64($bytes, $offset); $offset += 8
    $artifactCount = [BitConverter]::ToUInt64($bytes, $offset); $offset += 8
    if ($sourceCount -gt 4MB -or $sourceDirectoryCount -gt 4MB -or
        $dependencyCount -gt 4MB -or $artifactCount -gt 4MB) {
        throw 'The startup cache index declares an unreasonable entry count.'
    }

    $result.sourceEntryCount = [int]$sourceCount
    for ($sourceIndex = 0; $sourceIndex -lt $sourceCount; $sourceIndex++) {
        Skip-IndexString $bytes ([ref]$offset) # root mount
        Skip-IndexString $bytes ([ref]$offset) # relative path
        Mutate-IndexStringFirstByte $bytes ([ref]$offset) # stamp
        Skip-IndexString $bytes ([ref]$offset) # content hash
        Mutate-IndexStringFirstByte $bytes ([ref]$offset) # fingerprint
        $result.mutatedSourceEntries++
    }

    [IO.File]::WriteAllBytes($stampPath, $bytes)
    $startupStarted = Get-Date
    $editorProcess = Start-Process -FilePath $EditorPath `
        -ArgumentList @('--backend', 'dx12', $projectFile.FullName) `
        -WorkingDirectory (Split-Path -Parent $EditorPath) `
        -PassThru

    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    $logsRoot = Join-Path $ProjectRoot 'Logs'
    while ([DateTime]::UtcNow -lt $deadline) {
        $candidate = Get-ChildItem -LiteralPath $logsRoot -File -Filter '*.log' -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -ge $startupStarted.AddSeconds(-1) } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($null -ne $candidate) {
            $logFile = $candidate
            $logText = Get-Content -LiteralPath $logFile.FullName -Raw -ErrorAction SilentlyContinue
            if ($logText -match 'EditorWindowShown elapsedMs=') {
                $result.windowShown = $true
                break
            }
            if ($logText -match 'Startup asset preimport failed|\[Editor::TryRun\]') {
                break
            }
        }
        if ($editorProcess.HasExited) {
            break
        }
        Start-Sleep -Milliseconds 250
    }

    if ($null -ne $logFile) {
        $archivedLogPath = Join-Path $runRoot 'editor.log'
        Copy-Item -LiteralPath $logFile.FullName -Destination $archivedLogPath -Force
        $result.logPath = $archivedLogPath
        $logText = Get-Content -LiteralPath $logFile.FullName -Raw -ErrorAction SilentlyContinue
        $patterns = @{
            cacheValidationMs = 'Startup asset cache miss analysis took (?<value>[0-9]+) ms'
            importPlanningMs = 'Startup asset import planning took (?<value>[0-9]+) ms'
            importExecutionMs = 'Startup asset import execution took (?<value>[0-9]+) ms'
            indexPatchMs = 'Startup asset cache index patch took (?<value>[0-9]+) ms'
            editorWindowShownMs = 'EditorWindowShown elapsedMs=(?<value>[0-9]+)'
            plannedAssetCount = 'Startup asset import planning took [0-9]+ ms for (?<value>[0-9]+) items'
            importedAssetCount = 'Startup asset preimport completed: (?<value>[0-9]+) imported / [0-9]+ planned'
        }
        foreach ($property in $patterns.Keys) {
            $match = [regex]::Match($logText, $patterns[$property])
            if ($match.Success) {
                $result[$property] = [int]$match.Groups['value'].Value
            }
        }
    }
}
finally {
    Stop-EditorProcess $editorProcess
    Copy-Item -LiteralPath $backupPath -Destination $stampPath -Force
}

$resultPath = Join-Path $runRoot 'result.json'
$result | ConvertTo-Json | Set-Content -LiteralPath $resultPath -Encoding UTF8
$result | ConvertTo-Json -Compress
if (-not $result.windowShown) {
    exit 2
}
