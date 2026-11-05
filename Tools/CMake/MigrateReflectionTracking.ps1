param([Parameter(Mandatory = $true)][string]$Manifest)
$ErrorActionPreference = 'Stop'
$taskChanged = 0
foreach ($taskRecord in [IO.File]::ReadAllLines($Manifest)) {
    if (-not $taskRecord) { continue }
    $taskParts = $taskRecord.Split('|')
    if ($taskParts.Count -ne 5) { throw 'Malformed reflection tracking migration record.' }
    $taskModule, $taskBinary, $taskGenerated, $taskStamp, $taskContract = $taskParts
    $taskDirectory = Join-Path $taskBinary ($taskModule + '.dir')
    if (-not (Test-Path -LiteralPath $taskDirectory)) { continue }
    foreach ($taskLog in Get-ChildItem -LiteralPath $taskDirectory -Recurse -File -Filter 'CustomBuild.write.*.tlog') {
        $taskBytes = [IO.File]::ReadAllBytes($taskLog.FullName)
        $taskEncoding = New-Object Text.UTF8Encoding($false)
        $taskOffset = 0
        if ($taskBytes.Length -ge 2 -and $taskBytes[0] -eq 255 -and $taskBytes[1] -eq 254) {
            $taskEncoding = New-Object Text.UnicodeEncoding($false, $true)
            $taskOffset = 2
        }
        elseif ($taskBytes.Length -ge 3 -and $taskBytes[0] -eq 239 -and $taskBytes[1] -eq 187 -and $taskBytes[2] -eq 191) {
            $taskEncoding = New-Object Text.UTF8Encoding($true)
            $taskOffset = 3
        }
        elseif ($taskBytes.Length -ge 4 -and $taskBytes[1] -eq 0 -and $taskBytes[3] -eq 0) {
            $taskEncoding = New-Object Text.UnicodeEncoding($false, $false)
        }
        $taskText = $taskEncoding.GetString($taskBytes, $taskOffset, $taskBytes.Length - $taskOffset)
        $taskSeparator = "`n"
        if ($taskText.Contains("`r`r`n")) { $taskSeparator = "`r`r`n" }
        elseif ($taskText.Contains("`r`n")) { $taskSeparator = "`r`n" }
        $taskLines = [regex]::Split($taskText, '\r*\n')
        $taskKept = New-Object 'Collections.Generic.List[string]'
        $taskSkip = $false
        $taskMixed = $false
        $taskRule = [regex]::Escape($taskModule + '_MetaGenerated.stamp.rule') + '$'
        foreach ($taskLine in $taskLines) {
            if ($taskLine.StartsWith('^')) {
                $taskRoots = $taskLine.Substring(1).Split('|')
                $taskOwnedRoots = @($taskRoots | Where-Object { $_ -match $taskRule })
                $taskSkip = $taskOwnedRoots.Count -eq $taskRoots.Count
                $taskMixed = $taskOwnedRoots.Count -gt 0 -and -not $taskSkip
            }
            if ($taskSkip) { continue }
            if ($taskMixed -and -not $taskLine.StartsWith('^')) {
                $taskPath = $taskLine.Replace('/', '\')
                if ($taskPath.StartsWith($taskGenerated.Replace('/', '\').TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
                    $taskPath.Equals($taskStamp.Replace('/', '\'), [StringComparison]::OrdinalIgnoreCase) -or
                    ($taskContract -and $taskPath.StartsWith($taskContract.Replace('/', '\').TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase))) {
                    continue
                }
            }
            $taskKept.Add($taskLine)
        }
        $taskNew = [string]::Join($taskSeparator, $taskKept)
        if ($taskNew -and $taskText.EndsWith($taskSeparator) -and -not $taskNew.EndsWith($taskSeparator)) {
            $taskNew += $taskSeparator
        }
        if ($taskNew -ceq $taskText) { continue }
        $taskBackup = $taskLog.FullName + '.before-reflection-migration'
        if (Test-Path -LiteralPath $taskBackup) { $taskBackup += '.' + [Guid]::NewGuid().ToString('N') }
        $taskTemporary = $taskLog.FullName + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
        try {
            [IO.File]::WriteAllText($taskTemporary, $taskNew, $taskEncoding)
            [IO.File]::Replace($taskTemporary, $taskLog.FullName, $taskBackup)
        }
        finally {
            if (Test-Path -LiteralPath $taskTemporary) { [IO.File]::Delete($taskTemporary) }
        }
        ++$taskChanged
    }
}
if ($taskChanged) { Write-Output "Migrated reflection output ownership in $taskChanged tracking logs." }
