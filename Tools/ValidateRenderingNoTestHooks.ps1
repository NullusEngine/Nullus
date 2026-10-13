[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Configuration = 'RelWithDebInfo',
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDirectory))
if (!$OutputPath) { $OutputPath = Join-Path $buildRoot "rendering-no-test-hooks-$Configuration.log" }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsInstall -or $LASTEXITCODE -ne 0) { throw 'MSVC installation not found' }
$developerVariables = & $env:ComSpec /d /s /c `
    "call `"$vsInstall/Common7/Tools/VsDevCmd.bat`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'VC environment setup failed' }
$compileEnvironment = @{}
foreach ($entry in $developerVariables) {
    $separator = $entry.IndexOf('=')
    if ($separator -gt 0) {
        $compileEnvironment[$entry.Substring(0, $separator)] = $entry.Substring($separator + 1)
    }
}

$sourceGroups = @(
    @{
        Project = 'Runtime/Rendering/NLS_Render.vcxproj'
        Sources = @(
            'Runtime/Rendering/Context/RenderThreadCoordinator.cpp',
            'Runtime/Rendering/Context/Driver.cpp',
            'Runtime/Rendering/Context/FrameLatencyController.cpp',
            'Runtime/Rendering/Context/FrameResourceRing.cpp',
            'Runtime/Rendering/Context/RhiCommandListExecutorFrame.cpp',
            'Runtime/Rendering/Context/RenderCommandPipe.cpp',
            'Runtime/Rendering/Context/RenderingThreadSystem.cpp',
            'Runtime/Rendering/Context/RenderScenePackageBuilder.cpp',
            'Runtime/Rendering/Context/RenderScenePackage.cpp',
            'Runtime/Rendering/Core/ABaseRenderer.cpp'
        )
    },
    @{
        Project = 'Runtime/Engine/NLS_Engine.vcxproj'
        Sources = @(
            'Runtime/Engine/Rendering/BaseSceneRenderer.cpp',
            'Runtime/Engine/Rendering/RenderScene.cpp',
            'Runtime/Engine/Rendering/ForwardSceneRenderer.cpp',
            'Runtime/Engine/Rendering/DeferredSceneRenderer.cpp',
            'Runtime/Engine/Rendering/SceneLightingProvider.cpp',
            'Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.cpp'
        )
    },
    @{
        Project = 'Project/Editor/EditorProject.vcxproj'
        Sources = @('Project/Editor/Rendering/DebugSceneRenderer.cpp')
    }
)
foreach ($group in $sourceGroups) {
    [xml]$project = Get-Content -LiteralPath (Join-Path $buildRoot $group.Project)
    $condition = "'`$(Configuration)|`$(Platform)'=='$Configuration|x64'"
    $settings = $project.Project.ItemDefinitionGroup | Where-Object Condition -eq $condition |
        Select-Object -ExpandProperty ClCompile -First 1
    if (!$settings) { throw "Missing compiler settings for $Configuration" }
    $arguments = @('/nologo', '/Zs', '/std:c++20', '/MD', '/DNDEBUG', '/DWIN32',
        '/source-charset:utf-8', '/execution-charset:utf-8', '/Zc:__cplusplus', '/bigobj')
    foreach ($definition in $settings.PreprocessorDefinitions.Split(';')) {
        if ($definition -and !$definition.StartsWith('%(') -and !$definition.StartsWith('NLS_ENABLE_TEST_HOOKS')) {
            $arguments += "/D$definition"
        }
    }
    foreach ($includePath in $settings.AdditionalIncludeDirectories.Split(';')) {
        if ($includePath -and !$includePath.StartsWith('%(')) { $arguments += "/I$includePath" }
    }
    foreach ($include in [regex]::Matches($settings.AdditionalOptions, '/external:I\s+"([^"]+)"')) {
        $arguments += "/I$($include.Groups[1].Value)"
    }

    foreach ($source in $group.Sources) {
        $info = [Diagnostics.ProcessStartInfo]::new("$($compileEnvironment.VCToolsInstallDir)/bin/Hostx64/x64/cl.exe")
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.WorkingDirectory = $repoRoot
        $info.Environment.Clear()
        foreach ($key in $compileEnvironment.Keys) { $info.Environment[$key] = $compileEnvironment[$key] }
        foreach ($argument in $arguments) { $info.ArgumentList.Add($argument) }
        $info.ArgumentList.Add((Join-Path $repoRoot $source))
        $process = [Diagnostics.Process]::Start($info)
        try {
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            $process.WaitForExit()
            $stdout.Result, $stderr.Result | Add-Content -LiteralPath $OutputPath
            if ($process.ExitCode -ne 0) {
                throw "No-test-hooks syntax check failed: $source ($($process.ExitCode)); see $OutputPath"
            }
            Write-Output "No-test-hooks syntax passed: $source"
        }
        finally { $process.Dispose() }
    }
}
