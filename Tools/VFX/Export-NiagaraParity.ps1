param(
    [string]$EngineRoot = 'D:/Epic Games/UE_5.7/Engine',
    [string]$OutputPath = "$PSScriptRoot/../../Docs/Plans/NiagaraParity.json"
)
$ErrorActionPreference = 'Stop'
$version = Get-Content -LiteralPath (Join-Path $EngineRoot 'Build/Build.version') -Raw | ConvertFrom-Json
if ($version.MajorVersion -ne 5 -or $version.MinorVersion -ne 7 -or $version.PatchVersion -ne 4 -or $version.Changelist -ne 51494982) {
    throw 'The acceptance baseline must be installed UE 5.7.4 CL 51494982.'
}
$pluginRoot = Join-Path $EngineRoot 'Plugins/FX/Niagara'
$previous = @{}
if (Test-Path -LiteralPath $OutputPath) {
    $saved = Get-Content -LiteralPath $OutputPath -Raw | ConvertFrom-Json
    foreach ($requirement in $saved.requirements) { $previous[$requirement.id] = $requirement }
}
$rows = [System.Collections.Generic.List[object]]::new()
function Add-Requirement([string]$id, [string]$category, [string]$name, [string]$path, [object]$metadata) {
    $relative = [System.IO.Path]::GetRelativePath($pluginRoot, $path).Replace('\','/')
    $rows.Add([ordered]@{
        id = $id; category = $category; name = $name; reference = $relative
        referenceSha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        metadata = $metadata; implementation = @(); examples = @(); evidence = @(); status = 'pending'
    })
}
foreach ($folder in @('Modules','DynamicInputs','Functions','DefaultAssets','ParameterDefinitions')) {
    Get-ChildItem -LiteralPath (Join-Path $pluginRoot "Content/$folder") -Filter '*.uasset' -Recurse -File | Sort-Object FullName | ForEach-Object {
        $relative = [System.IO.Path]::GetRelativePath((Join-Path $pluginRoot 'Content'),$_.FullName).Replace('\','/')
        Add-Requirement "asset:$relative" $folder $_.BaseName $_.FullName @{}
    }
}
Get-ChildItem -LiteralPath (Join-Path $pluginRoot 'Source/Niagara') -Filter '*.h' -Recurse -File |
    Where-Object { $_.Name -match '^Niagara(DataInterface.*|.*RendererProperties|SimulationStage.*|.*DataChannel.*|StatelessModule_.*)\.h$' } |
    Sort-Object FullName | ForEach-Object {
        $category = if ($_.Name -match 'RendererProperties') { 'renderer' } elseif ($_.Name -match 'StatelessModule') { 'stateless' } else { 'data-interface' }
        Add-Requirement "native:$($_.BaseName)" $category $_.BaseName $_.FullName @{}
    }
$commands = Join-Path $pluginRoot 'Source/NiagaraEditor/Private/NiagaraEditorCommands.cpp'
$commandSource = [regex]::Replace((Get-Content -LiteralPath $commands -Raw),'(?s)/\*.*?\*/','')
foreach ($match in [regex]::Matches($commandSource,'(?m)^\s*UI_COMMAND\((\w+),\s*"([^"]+)".*$')) {
    Add-Requirement "command:$($match.Groups[1].Value)" 'command' $match.Groups[2].Value $commands @{ declaration = $match.Value.Trim() }
}
$modeBase = Join-Path $pluginRoot 'Source/NiagaraEditor/Private/Toolkits/SystemToolkitModes/NiagaraSystemToolkitModeBase.cpp'
foreach ($match in [regex]::Matches((Get-Content -LiteralPath $modeBase -Raw),'RegisterTabSpawner\((\w+)[\s\S]*?SetDisplayName\(LOCTEXT\("[^"]*",\s*"([^"]*)"\)\)')) {
    Add-Requirement "panel:$($match.Groups[1].Value)" 'panel' $match.Groups[2].Value $modeBase @{}
}
foreach ($mode in @('Default','Scalability')) {
    $path = Join-Path $pluginRoot "Source/NiagaraEditor/Private/Toolkits/SystemToolkitModes/NiagaraSystemToolkitMode_$mode.cpp"
    Add-Requirement "layout:$mode" 'layout' $mode $path @{}
}
$widgetStyle = Join-Path $pluginRoot 'Source/NiagaraEditorWidgets/Private/NiagaraEditorWidgetsStyle.cpp'
if (-not (Test-Path -LiteralPath $widgetStyle)) { throw 'Niagara widget style reference is missing.' }
foreach ($match in [regex]::Matches((Get-Content -LiteralPath $widgetStyle -Raw),'Set\("([^"]+)",\s*FLinearColor\(FColor\((\d+),\s*(\d+),\s*(\d+)\)\)\)')) {
    Add-Requirement "style:$($match.Groups[1].Value)" 'style' $match.Groups[1].Value $widgetStyle @{srgb=@([int]$match.Groups[2].Value,[int]$match.Groups[3].Value,[int]$match.Groups[4].Value)}
}
foreach ($category in @('typed-ir','parameter-map','inheritance-versioning','system-emitter-stages','events','simulation-stages','multi-document',
    'system-preview','curves-gradients','timeline','gizmos','simcache','baker','scalability','debugger','runtime-packaging',
    'windows-dx12','linux-vulkan','macos-metal','dpi-interactions','stability-soak','performance')) {
    $rows.Add([ordered]@{id="acceptance:$category";category='acceptance';name=$category;reference='UE 5.7.4 CL 51494982 behavior';
        implementation=@();examples=@();evidence=@();status='pending'})
}
$result = [ordered]@{
    schemaVersion=1;referenceVersion='5.7.4';changelist=51494982;plugin='Niagara';defaultEditor='module-stack'
    coordinates='Nullus native';excludedPlugins=@('NiagaraFluids','NiagaraNanite','NiagaraMRQ')
    completionRule='Every applicable requirement needs implementation, a runnable example and passing evidence; inventory alone proves no parity.'
    requirements=$rows
}
foreach ($row in $rows) {
    if ($previous.ContainsKey($row.id)) {
        $old = $previous[$row.id]
        if ($old.referenceSha256 -and $old.referenceSha256 -ne $row.referenceSha256) { throw "Frozen reference changed: $($row.id)" }
        foreach ($field in @('implementation','examples','evidence','status')) { $row[$field] = $old.$field }
        if ($old.localValidation) { $row['localValidation'] = $old.localValidation }
        if ($old.metadata) {
            foreach ($property in $old.metadata.PSObject.Properties) {
                if (-not $row.metadata.Contains($property.Name)) { $row.metadata[$property.Name] = $property.Value }
            }
        }
    }
}
if ($saved -and $saved.installedMetadata) { $result['installedMetadata'] = $saved.installedMetadata }
$policy = $saved
if (-not $policy) {
    $policyPath = Join-Path $PSScriptRoot '../../Docs/Plans/NiagaraParity.json'
    if (Test-Path -LiteralPath $policyPath) { $policy = Get-Content -LiteralPath $policyPath -Raw | ConvertFrom-Json }
}
if ($policy -and $policy.defaultEditor) { $result['defaultEditor'] = $policy.defaultEditor }
if ($policy -and $policy.editorCutover) { $result['editorCutover'] = $policy.editorCutover }
$destination = [System.IO.Path]::GetFullPath($OutputPath)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($destination)) | Out-Null
[System.IO.File]::WriteAllText($destination,($result | ConvertTo-Json -Depth 12),[System.Text.UTF8Encoding]::new($false))
Write-Output "Frozen $($rows.Count) acceptance requirements from UE 5.7.4 CL 51494982."
