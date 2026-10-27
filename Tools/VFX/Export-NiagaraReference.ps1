param(
    [Parameter(Mandatory = $true)][string]$EngineRoot,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$versionPath = Join-Path $EngineRoot 'Build/Build.version'
$version = Get-Content -LiteralPath $versionPath -Raw | ConvertFrom-Json
if ($version.MajorVersion -ne 5 -or $version.MinorVersion -ne 7 -or $version.PatchVersion -ne 4) {
    throw 'The VFX reference baseline must be UE 5.7.4.'
}
$niagaraRoot = Join-Path $EngineRoot 'Plugins/FX/Niagara'
$referenceRoot = [System.IO.Path]::GetFullPath($niagaraRoot).TrimEnd('\', '/')
$entries = @()
Get-ChildItem -LiteralPath (Join-Path $referenceRoot 'Source') -Recurse -Filter '*.h' -File |
    Where-Object {
        $_.Name -match '^Niagara(DataInterface.*|.*RendererProperties|SimulationStage.*|.*DataChannel.*|StatelessModule_.*)\.h$'
    } | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($referenceRoot.Length + 1).Replace('\', '/')
        $entries += [ordered]@{
            kind = 'native-header'
            name = $_.BaseName
            reference = $relative
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            status = 'pending'
        }
    }
foreach ($folder in @('Modules', 'Functions', 'DynamicInputs', 'DefaultAssets')) {
    $directory = Join-Path $referenceRoot "Content/$folder"
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) { continue }
    Get-ChildItem -LiteralPath $directory -Recurse -Filter '*.uasset' -File | Sort-Object FullName | ForEach-Object {
        $entries += [ordered]@{
            kind = $folder
            name = $_.BaseName
            reference = $_.FullName.Substring($referenceRoot.Length + 1).Replace('\', '/')
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            status = 'pending'
        }
    }
}
$result = [ordered]@{
    schemaVersion = 1
    referenceVersion = '5.7.4'
    plugin = 'Niagara'
    coverage = 'reference-file-inventory'
    entries = $entries
}
$destination = [System.IO.Path]::GetFullPath($OutputPath)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($destination)) | Out-Null
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $destination -Encoding UTF8
Write-Output ("Exported {0} reference entries to {1}" -f $entries.Count, $destination)
