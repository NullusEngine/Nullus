param(
    [string]$OutputPath = (Join-Path $PSScriptRoot 'nullus-script-debugger-1.0.0.vsix')
)

$ErrorActionPreference = 'Stop'
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ('nullus-vsix-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $stage 'extension') -Force | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'package.json') (Join-Path $stage 'extension\package.json')
Copy-Item (Join-Path $PSScriptRoot 'extension.js') (Join-Path $stage 'extension\extension.js')
Copy-Item (Join-Path $PSScriptRoot 'README.md') (Join-Path $stage 'extension\README.md')

$package = @'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.microsoft.com/sharepoint/vsix-package/2010">
  <Type Id="Microsoft.VisualStudio.Code"/>
</Types>
'@
$manifest = @'
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/vsx-schema/2011">
  <Metadata>
    <Identity Id="nullus.nullus-script-debugger" Version="1.0.0" Language="en-US" Publisher="nullus" />
    <DisplayName>Nullus Script Debugger</DisplayName>
    <Description xml:space="preserve">Project-scoped Nullus C# and Lua F5 debug integration.</Description>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code" Version="^1.85.0" />
  </Installation>
  <Dependencies />
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension\package.json" />
  </Assets>
</PackageManifest>
'@
[System.IO.File]::WriteAllText((Join-Path $stage '[Content_Types].xml'), "<?xml version=`"1.0`" encoding=`"utf-8`"?><Types xmlns=`"http://schemas.openxmlformats.org/package/2006/content-types`"><Default Extension=`"json`" ContentType=`"application/json`"/><Default Extension=`"js`" ContentType=`"application/javascript`"/><Default Extension=`"md`" ContentType=`"text/markdown`"/><Override PartName=`"/extension.vsixmanifest`" ContentType=`"text/xml`"/></Types>", [System.Text.Encoding]::UTF8)
[System.IO.File]::WriteAllText((Join-Path $stage 'extension.vsixmanifest'), $manifest, [System.Text.Encoding]::UTF8)
if (Test-Path $OutputPath) { Remove-Item -LiteralPath $OutputPath -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $OutputPath -Force
Remove-Item -LiteralPath $stage -Recurse -Force
Write-Output $OutputPath
