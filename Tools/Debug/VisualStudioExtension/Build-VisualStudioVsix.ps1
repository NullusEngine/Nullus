param(
    [string]$Configuration = 'Release',
    [string]$OutputPath = (Join-Path $PSScriptRoot 'Nullus.ScriptDebugger.vsix')
)

$ErrorActionPreference = 'Stop'
$vsRoot = $null
$registryRoots = @(
    'HKLM:\SOFTWARE\Microsoft\VisualStudio\SxS\VS7',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\SxS\VS7')
foreach ($registryRoot in $registryRoots) {
    try {
        $vsRoot = (Get-ItemProperty -Path $registryRoot -Name '17.0' -ErrorAction Stop).'17.0'
        if ($vsRoot) { break }
    } catch { }
}
if (-not $vsRoot) {
    $roots = @()
    foreach ($name in @('ProgramFiles', 'ProgramFiles(x86)')) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($value) { $roots += $value }
    }
    foreach ($drive in [Environment]::GetLogicalDrives()) {
        $roots += (Join-Path $drive 'Program Files')
        $roots += (Join-Path $drive 'Program Files (x86)')
    }
    foreach ($root in ($roots | Select-Object -Unique)) {
        foreach ($edition in @('Community', 'Professional', 'Enterprise', 'BuildTools')) {
            $candidate = Join-Path $root "Microsoft Visual Studio\2022\$edition"
            if (Test-Path (Join-Path $candidate 'MSBuild\Current\Bin\MSBuild.exe')) {
                $vsRoot = $candidate
                break
            }
        }
        if ($vsRoot) { break }
    }
}
if (-not $vsRoot) { throw 'Visual Studio 2022 installation was not found.' }
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) { throw "MSBuild was not found: $msbuild" }
$publicAssemblies = Join-Path $vsRoot 'Common7\IDE\PublicAssemblies'
if (-not (Test-Path -LiteralPath $publicAssemblies)) {
    throw "Visual Studio public assemblies were not found: $publicAssemblies"
}
$projectSystemPath = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\Project'
if (-not (Test-Path -LiteralPath (Join-Path $projectSystemPath 'Microsoft.VisualStudio.ProjectSystem.VS.dll'))) {
    throw "Visual Studio Project System assemblies were not found: $projectSystemPath"
}
$managedProjectSystemPath = Join-Path $vsRoot 'Common7\IDE\Extensions\Microsoft\ManagedProjectSystem'
if (-not (Test-Path -LiteralPath (Join-Path $managedProjectSystemPath 'Microsoft.VisualStudio.ProjectSystem.Managed.VS.dll'))) {
    throw "Visual Studio managed Project System assemblies were not found: $managedProjectSystemPath"
}
$roslynPath = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\VBCSharp\LanguageServices'
$compositionPath = Join-Path $vsRoot 'Common7\IDE\PublicAssemblies'
if (-not (Test-Path -LiteralPath (Join-Path $roslynPath 'Microsoft.CodeAnalysis.Features.dll'))) {
    throw "Visual Studio Roslyn workspace assemblies were not found: $roslynPath"
}
$requiredAssemblies = @(
    'envdte.dll',
    'envdte80.dll',
    'Microsoft.VisualStudio.Shell.15.0.dll',
    'Microsoft.VisualStudio.Shell.Framework.dll',
    'Microsoft.VisualStudio.Interop.dll',
    'Microsoft.VisualStudio.OLE.Interop.dll',
    'Microsoft.VisualStudio.Shell.Interop.8.0.dll',
    'Microsoft.VisualStudio.Shell.Interop.10.0.dll')
foreach ($assembly in $requiredAssemblies) {
    $assemblyPath = Join-Path $publicAssemblies $assembly
    if (-not (Test-Path -LiteralPath $assemblyPath)) {
        throw "Visual Studio public assembly was not found: $assemblyPath"
    }
}
$threadingPath = Join-Path $publicAssemblies 'Microsoft.VisualStudio.Threading.17.x\Microsoft.VisualStudio.Threading.dll'
if (-not (Test-Path -LiteralPath $threadingPath)) {
    $threadingPath = Join-Path $vsRoot 'Common7\IDE\PrivateAssemblies\Microsoft.VisualStudio.Threading.dll'
}
if (-not (Test-Path -LiteralPath $threadingPath)) {
    throw "Visual Studio threading assembly was not found below: $vsRoot"
}
$project = Join-Path $PSScriptRoot 'Nullus.ScriptDebugger.csproj'
$vsct = Join-Path $vsRoot 'VSSDK\VisualStudioIntegration\Tools\Bin\vsct.exe'
$vsctInclude = Join-Path $vsRoot 'VSSDK\VisualStudioIntegration\Common\Inc'
if (-not (Test-Path -LiteralPath $vsct) -or -not (Test-Path -LiteralPath $vsctInclude)) {
    throw "Visual Studio command table compiler was not found below: $vsRoot"
}
$vsixTargets = Join-Path $vsRoot 'MSBuild\Microsoft\VisualStudio\v17.0\VSSDK\Microsoft.VsSDK.targets'
if (-not (Test-Path -LiteralPath $vsixTargets)) {
    throw "Visual Studio VSIX SDK targets were not found: $vsixTargets"
}
& $msbuild $project '/t:Build;CopyPkgDef;CreateVsixContainer' '/restore' "/p:Configuration=$Configuration" "/p:VSInstallRoot=$vsRoot" "/p:VSPublicAssembliesPath=$publicAssemblies" "/p:VSThreadingPath=$threadingPath" "/p:VSProjectSystemPath=$projectSystemPath" "/p:VSManagedProjectSystemPath=$managedProjectSystemPath" "/p:VSRoslynPath=$roslynPath" "/p:VSCompositionPath=$compositionPath" "/p:VSCTPath=$vsct" "/p:VSCTIncludePath=$vsctInclude" '/p:DeployExtension=false' '/p:RestoreIgnoreFailedSources=true' '/nologo'
if ($LASTEXITCODE -ne 0) { throw "Visual Studio extension build/package failed with exit code $LASTEXITCODE." }
$built = Join-Path $PSScriptRoot "bin\$Configuration\net48\Nullus.ScriptDebugger.vsix"
if (-not (Test-Path -LiteralPath $built)) { throw "Visual Studio did not produce the VSIX package: $built" }
$output = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Parent $output
if ($outputDirectory) { New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null }
Copy-Item -LiteralPath $built -Destination $output -Force
Write-Output $output
