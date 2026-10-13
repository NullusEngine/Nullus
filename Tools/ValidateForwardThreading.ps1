[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo')]
    [string]$Configuration = 'RelWithDebInfo',
    [string]$BuildDirectory = 'build',
    [ValidateSet('DX12', 'Vulkan')]
    [string]$Backend = 'DX12',
    [ValidateSet('Tasks', 'DedicatedThread', 'None')]
    [string[]]$RhiModes = @('Tasks', 'DedicatedThread', 'None'),
    [string]$ProjectDirectory = 'build/threading-forward-geometry-project-20261009',
    [string]$OutputDirectory,
    [ValidateRange(1, 10000)]
    [int]$CaptureAfterFrames = 60,
    [switch]$CaptureSwapchain,
    [switch]$RequireImportedMaterials,
    [string]$VulkanDriverManifest,
    [string]$VulkanLayerPath,
    [switch]$VulkanSynchronizationValidation,
    [ValidateRange(10, 3600)]
    [int]$TimeoutSeconds = 180,
    [string]$CdbPath = 'C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else { Join-Path $repoRoot $BuildDirectory }))
$ProjectDirectory = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($ProjectDirectory)) {
    $ProjectDirectory
} else { Join-Path $repoRoot $ProjectDirectory }))
$appDirectory = Join-Path $repoRoot "App/Win64_${Configuration}_Runtime_Shared"
$game = Join-Path $appDirectory 'Game.exe'
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot "build/validation/forward-threading-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
foreach ($path in @($CdbPath, $game)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing executable: $path" }
}
$builtGame = Join-Path $buildRoot "app-binaries/$Configuration/Game.exe"
if (!(Test-Path -LiteralPath $builtGame -PathType Leaf)) {
    throw "Missing build-owned Game executable; rebuild Game in $buildRoot before validation."
}
if ((Get-FileHash -LiteralPath $builtGame).Hash -ne (Get-FileHash -LiteralPath $game).Hash) {
    throw "Game deployment is stale or from another build: $game"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$binaryHashes = @(Get-ChildItem -LiteralPath $appDirectory -File |
    Where-Object { $_.Extension -eq '.dll' -or $_.FullName -eq $game } |
    ForEach-Object { [pscustomobject]@{
        Path = $_.FullName; Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    } })
$runtimeManifest = Join-Path $buildRoot "runtime-manifests/Game-$Configuration.txt"
foreach ($name in Get-Content -LiteralPath $runtimeManifest) {
    if (!$name) { continue }
    $built = Join-Path $buildRoot "runtime/$Configuration/$name"
    $deployed = Join-Path $appDirectory $name
    if ((Get-FileHash -LiteralPath $built).Hash -ne (Get-FileHash -LiteralPath $deployed).Hash) {
        throw "Runtime deployment is stale: $deployed"
    }
}
$binaryHashes | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'forward-binaries.json')
Add-Type -AssemblyName System.Drawing
$results = [Collections.Generic.List[object]]::new()
$referenceHash = $null
$fatalPattern = '^\s*NLS_FORWARD_NATIVE_EXCEPTION_|D3D12 (ERROR|CORRUPTION):|Validation Error:|VUID-|' +
    'Assertion Failed|\[ERROR\]|device lost detected|unsafe GPU work quarantined'
$modes = @(
    @{ Project = 'Tasks'; Setting = 'tasks'; Resolved = 'Tasks' },
    @{ Project = 'Dedicated'; Setting = 'dedicated_thread'; Resolved = 'DedicatedThread' },
    @{ Project = 'None'; Setting = 'none'; Resolved = 'None' }
)
foreach ($mode in ($modes | Where-Object { $_.Resolved -in $RhiModes })) {
    $project = Join-Path $ProjectDirectory "$($mode.Project).nullus"
    $settings = Get-Content -LiteralPath $project
    if ($settings -notcontains "rhi_thread_mode=$($mode.Setting)" -or
        $settings -notcontains "graphics_backend=$Backend") {
        throw "Project must select $Backend and $($mode.Setting): $project"
    }
    $prefix = Join-Path $OutputDirectory "forward-$($mode.Setting)"
    $png = "$prefix.png"
    $report = "$prefix.txt"
    $console = "$prefix-console.log"
    $nativeLog = "$prefix-cdb.log"
    $commands = @(".sympath `"$buildRoot/runtime/$Configuration;$buildRoot/Project/Game/$Configuration`"")
    $dumpPath = "$prefix-failure.dmp".Replace('\', '/')
    foreach ($exception in @('87d', 'av', 'bpe', 'c0000374', 'c0000409')) {
        $commands += "sxe -c `".echo NLS_FORWARD_NATIVE_EXCEPTION_$exception; .exr -1; .ecxr; kv 60; ~* k 24; .dump /m /u \`"$dumpPath\`"; q`" $exception"
    }
    $commands += 'g'
    $commands | Set-Content -LiteralPath "$prefix.cdb" -Encoding ascii
    $info = [Diagnostics.ProcessStartInfo]::new($CdbPath)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.WorkingDirectory = $repoRoot
    foreach ($name in @('VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_LAYER_PATH',
        'VK_LOADER_DEBUG', 'VK_INSTANCE_LAYERS', 'NLS_VULKAN_VALIDATION')) {
        $info.Environment.Remove($name) | Out-Null
    }
    if ($Backend -eq 'Vulkan') {
        $info.Environment['NLS_VULKAN_VALIDATION'] = '1'
        $info.Environment['VK_LOADER_DEBUG'] = 'layer'
        if ($VulkanSynchronizationValidation) {
            $info.Environment['VK_LAYER_VALIDATE_SYNC'] = '1'
        }
        if ($VulkanDriverManifest) {
            $info.Environment['VK_DRIVER_FILES'] = (Resolve-Path -LiteralPath $VulkanDriverManifest).Path
        }
        if ($VulkanLayerPath) {
            $info.Environment['VK_LAYER_PATH'] = (Resolve-Path -LiteralPath $VulkanLayerPath).Path
        }
    }
    foreach ($argument in @('-G', '-cf', "$prefix.cdb", '-logo', $nativeLog, $game, $project,
        '--backend', $Backend.ToLowerInvariant(), '--no-renderdoc', '--material-validation-output', $png,
        '--material-validation-summary', $report, '--material-validation-after-frames', "$CaptureAfterFrames")) {
        $info.ArgumentList.Add($argument)
    }
    if ($CaptureSwapchain) { $info.ArgumentList.Add('--material-validation-swapchain') }
    $started = [DateTime]::UtcNow
    Write-Output "Starting Forward $($mode.Resolved), capture at frame $CaptureAfterFrames"
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        while (!$process.WaitForExit(1000)) {
            if (([DateTime]::UtcNow - $started).TotalSeconds -ge $TimeoutSeconds) {
                $process.Kill($true)
                $process.WaitForExit()
                $stdout.Result, $stderr.Result | Set-Content -LiteralPath $console
                throw "Forward $($mode.Resolved) timed out; see $console"
            }
        }
        $stdout.Result, $stderr.Result | Set-Content -LiteralPath $console
        if ($process.ExitCode -ne 0) { throw "Forward debugger returned $($process.ExitCode); see $console" }
    }
    finally { $process.Dispose() }
    if (Select-String -LiteralPath $console, $nativeLog -Pattern $fatalPattern) {
        throw "Forward $($mode.Resolved) captured a native/engine/GPU error; see $nativeLog"
    }
    if (!(Select-String -LiteralPath $console -Pattern "RHI thread mode backend=$Backend requested=Force$($mode.Resolved) resolved=$($mode.Resolved) \(forced\)")) {
        throw "Forward did not confirm the requested RHI mode; see $console"
    }
    if ($Backend -eq 'Vulkan' -and !(Select-String -LiteralPath $console -Pattern 'Insert(?:ed)? device layer.*VK_LAYER_KHRONOS_validation')) {
        throw "Forward did not confirm the loaded Vulkan validation layer; see $console"
    }
    foreach ($artifact in @($png, $report)) {
        if (!(Test-Path -LiteralPath $artifact) -or (Get-Item -LiteralPath $artifact).LastWriteTimeUtc -lt $started) {
            throw "Forward did not produce fresh readback evidence: $artifact"
        }
    }
    $bitmap = [Drawing.Bitmap]::new($png)
    try {
        if ($bitmap.Width -ne 1600 -or $bitmap.Height -ne 900) {
            throw "Forward geometry fixture must render at 1600x900: $png"
        }
        $background = $bitmap.GetPixel(0, 0).ToArgb()
        foreach ($x in @(400, 800, 1200)) {
            if ($bitmap.GetPixel($x, 450).ToArgb() -eq $background) {
                throw "Forward is missing a fixture cube: $png (x=$x)"
            }
        }
        if ($RequireImportedMaterials) {
            foreach ($probe in @(@{ X = 400; Channel = 'R' }, @{ X = 800; Channel = 'G' }, @{ X = 1200; Channel = 'B' })) {
                $pixel = $bitmap.GetPixel($probe.X, 450)
                $dominant = [int]$pixel.($probe.Channel)
                foreach ($channel in @('R', 'G', 'B')) {
                    if ($channel -ne $probe.Channel -and $dominant -le [int]$pixel.$channel * 1.2) {
                        throw "Forward fixture material is not $($probe.Channel)-dominant: $png (x=$($probe.X))"
                    }
                }
            }
        }
        if ($bitmap.GetPixel(1075, 450).ToArgb() -eq $bitmap.GetPixel(1200, 450).ToArgb()) {
            throw "Forward fixture faces do not have distinct lighting: $png"
        }
    }
    finally { $bitmap.Dispose() }
    $imageHash = (Get-FileHash -LiteralPath $png -Algorithm SHA256).Hash
    if ($null -eq $referenceHash) { $referenceHash = $imageHash }
    elseif ($imageHash -ne $referenceHash) { throw "Forward image differs between RHI modes: $png" }
    $probeResult = (Select-String -LiteralPath $report -Pattern '^Material Visual Evidence: (PASS|FAIL)$').Matches.Groups[1].Value
    if (!$probeResult) { throw "Forward readback report is incomplete: $report" }
    $results.Add([pscustomobject]@{
        Backend = $Backend; Mode = $mode.Resolved; CaptureAfterFrames = $CaptureAfterFrames
        SynchronizationValidation = $Backend -eq 'Vulkan' -and $info.Environment['VK_LAYER_VALIDATE_SYNC'] -eq '1'
        CaptureTarget = if ($CaptureSwapchain) { 'Swapchain' } else { 'Offscreen' }
        ImagePath = $png; ImageSha256 = $imageHash
        GeometryReadback = 'PASS'; ImportedMaterialProbes = $probeResult
        DurationSeconds = ([DateTime]::UtcNow - $started).TotalSeconds
        ConsolePath = $console; NativeLogPath = $nativeLog
    })
    $results | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $OutputDirectory 'forward-results.json')
    if ($RequireImportedMaterials -and $probeResult -ne 'PASS') {
        throw "Forward $($mode.Resolved) imported material probes failed; see $report"
    }
    Write-Output "Forward $($mode.Resolved) geometry/readback passed; imported material probes: $probeResult"
}
foreach ($binary in $binaryHashes) {
    if ((Get-FileHash -LiteralPath $binary.Path -Algorithm SHA256).Hash -ne $binary.Sha256) {
        throw "Binary changed during validation: $($binary.Path)"
    }
}
Write-Output "Forward geometry/readback verification complete; images match and binary hashes are unchanged: $OutputDirectory"
