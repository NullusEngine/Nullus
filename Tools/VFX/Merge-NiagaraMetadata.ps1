param(
    [string]$MetadataPath = "$PSScriptRoot/../../build/niagara-reference/installed-metadata.json",
    [string]$ManifestPath = "$PSScriptRoot/../../Docs/Plans/NiagaraParity.json"
)
$ErrorActionPreference = 'Stop'
$metadata = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json -AsHashtable
if ($metadata.engineVersion -ne '5.7.4-51494982+++UE5+Release-5.7') { throw 'Unexpected executable reference version.' }
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json -AsHashtable
$byPackage = @{}
foreach ($asset in $metadata.assets) { $byPackage[$asset.package] = $asset }
$matched = 0
foreach ($row in $manifest.requirements) {
    if ($row.id.StartsWith('asset:')) {
        $package = '/Niagara/'+$row.id.Substring(6).Replace('.uasset','')
        if ($byPackage.ContainsKey($package)) {
            $asset = $byPackage[$package]
            $row.metadata['installedClass'] = $asset.class
            $row.metadata['installedRegistryTags'] = $asset.registryTags
            $row.metadata['unavailablePythonProperties'] = $asset.unavailableProperties
            $row.metadata['behaviorCaptured'] = $false
            $matched++
        }
    }
}
$manifest['installedMetadata'] = @{engineVersion=$metadata.engineVersion;assetCount=$metadata.assets.Count;matchedRequirements=$matched;
    captureTool='Tools/VFX/CaptureNiagaraMetadata.py';coverage='registry metadata only; property defaults and behavior require further capture'}
[System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($ManifestPath),($manifest | ConvertTo-Json -Depth 20),[System.Text.UTF8Encoding]::new($false))
Write-Output "Merged installed metadata for $matched reference assets; no acceptance status changed."
