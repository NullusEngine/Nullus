param([string]$ManifestPath = "$PSScriptRoot/../../Docs/Plans/NiagaraParity.json")
$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ($manifest.referenceVersion -ne '5.7.4' -or $manifest.changelist -ne 51494982) { throw 'Unexpected Niagara acceptance baseline.' }
if ($manifest.defaultEditor -notin @('legacy','module-stack')) { throw 'Unknown default particle editor.' }
if ($manifest.defaultEditor -eq 'module-stack' -and
    ($manifest.editorCutover.mode -ne 'user-requested' -or -not $manifest.editorCutover.authorization -or
     $manifest.editorCutover.legacyEditorRemoved -ne $true)) { throw 'New editor requires a recorded cutover policy.' }
if ($manifest.defaultEditor -eq 'module-stack') {
    # The new editor owns the VfxEditor name now. Detect the removed graph editor
    # by its distinctive implementation files/symbols instead of by filename.
    $legacySource = Join-Path $repoRoot 'Project/Editor/VFX/VfxGpuPreview.cpp'
    if (Test-Path -LiteralPath $legacySource) { throw 'Removed legacy graph preview implementation reintroduced.' }
    $legacyDocument = Join-Path $repoRoot 'Project/Editor/VFX/VfxDocument.cpp'
    if (Test-Path -LiteralPath $legacyDocument) {
        $documentText = Get-Content -LiteralPath $legacyDocument -Raw
        if ($documentText -match 'NLS::VFX::CreateDefaultSystem') { throw 'Removed legacy graph document service reintroduced.' }
    }
}
$ids = @{}
$pending = [System.Collections.Generic.List[string]]::new()
foreach ($requirement in $manifest.requirements) {
    if ($ids.ContainsKey($requirement.id)) { throw "Duplicate acceptance ID: $($requirement.id)" }
    $ids[$requirement.id] = $true
    if ($requirement.status -ne 'verified') { $pending.Add($requirement.id); continue }
    if ($requirement.implementation.Count -eq 0 -or $requirement.examples.Count -eq 0 -or $requirement.evidence.Count -eq 0) {
        throw "Verified requirement lacks implementation, runnable example, or evidence: $($requirement.id)"
    }
    foreach ($path in @($requirement.implementation)+@($requirement.examples)) {
        if (-not (Test-Path -LiteralPath (Join-Path $repoRoot $path))) { throw "Missing implementation/example: $path" }
    }
    foreach ($evidence in $requirement.evidence) {
        if ($evidence.result -ne 'passed' -or -not $evidence.test -or -not $evidence.referenceCase -or -not $evidence.report) {
            throw "Evidence must identify a passing test, UE case, and report: $($requirement.id)"
        }
        if (-not (Test-Path -LiteralPath (Join-Path $repoRoot $evidence.report))) { throw "Missing evidence report: $($evidence.report)" }
    }
}
if ($pending.Count) {
    if ($manifest.editorCutover.parityAccepted -eq $true) { throw 'Incomplete Niagara parity cannot be marked accepted.' }
    Write-Output "$($pending.Count) of $($manifest.requirements.Count) Niagara requirements remain unverified."
    if ($manifest.defaultEditor -eq 'module-stack') { Write-Output 'New editor is the single default path by explicit user request; this does not establish Niagara parity.' }
    exit 1
}
Write-Output "All $($ids.Count) Niagara acceptance requirements have evidence."
