[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo')]
    [string]$Configuration = 'RelWithDebInfo',
    [ValidateRange(1, 1000000)]
    [int]$StressFrames = 256,
    [ValidateRange(1, 86400)]
    [int]$StressTimeoutSeconds = 2400,
    [ValidateRange(0, 86400)]
    [int]$MinimumStressSeconds = 0,
    [switch]$StressOnly,
    [ValidateSet('DX12', 'Vulkan')]
    [string]$Backend = 'DX12',
    [ValidateSet('tasks', 'dedicated_thread', 'none')]
    [string[]]$RhiModes = @('tasks', 'dedicated_thread', 'none'),
    [string]$VulkanDriverManifest,
    [string]$VulkanLayerPath,
    [string]$CdbPath = 'C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe',
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
if ($MinimumStressSeconds -ge $StressTimeoutSeconds) {
    throw 'MinimumStressSeconds must be less than StressTimeoutSeconds.'
}
$repoRoot = Split-Path -Parent $PSScriptRoot
$binaryDirectory = Join-Path $repoRoot "build/bin/$Configuration"
$unit = Join-Path $binaryDirectory 'NullusUnitTests.exe'
$performance = Join-Path $binaryDirectory 'NullusPerformanceTests.exe'
foreach ($path in @($CdbPath, $unit, $performance)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing executable: $path" }
}
if (!$OutputDirectory) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $repoRoot "build/validation/rendering-threading-$Configuration-$stamp"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$regressionSuites = @(
    'TaskGraphTests', 'RenderCommand*Tests', 'RenderingThread*Tests',
    'RhiCommandListExecutorTests', 'RhiCommandListRecorderTests',
    'RhiSubmitLaneSourceGuardTests', 'FrameResourceRingTests', 'FrameLatency*Tests',
    'DriverFlushTests', 'DriverRhiThreadModeTests', 'DriverSwapchainResizeTests', 'DriverNullDeviceReadinessTests',
    'RenderFrameExecutionTests', '*/FrameTextureReadbackTests', 'UE5RenderArchitectureContractTests',
    'RendererFrameObjectBindingTests', 'CompositeRendererExplicitDrawOrderTests',
    'FrameGraphSceneTargetsTests', 'RenderFrameworkContractTests', 'RenderSceneCacheTests',
    'SceneProxyTests', 'SceneProxySourceRegistryTests', 'SceneRenderStore*Tests',
    'RenderSceneHandoffTests', 'LightingDataProviderTests', 'DebugSceneLifecycleTests', 'RendererStatsTests',
    'RendererStatsConcurrencyTests', 'AssetMaterialConversionTests', 'ShaderLabMaterialTests',
    'MaterialVariantKeyTests', 'DeferredSceneRendererMaterialCacheTests',
    'TextureResourceLifecycleTests', 'TextureResourceUpdateUtilsTests', 'UploadContextTests',
    'RHIUiOverlayPassTests', 'RHIUiOverlaySourceGuardTests',
    'RhiThreadModePolicyTests', 'EditorLaunchArgsTests', 'EditorSettingsPersistenceTests',
    'ProjectLaunchOptionsTests', 'GameLaunchArgsTests', 'LauncherLocalizationTests', 'TimelineProfilerGpuLifecycleTests',
    'DX12ReadbackUtilsTests', 'DX12TextureUploadUtilsTests', 'DX12SubmitLaneTests',
    'ProfilerDestinationTest', 'VulkanSubmitLaneTests'
)
$regressionFilter = ($regressionSuites | ForEach-Object { "$($_).*" }) -join ':'
$repeatFilter = @(
    'RenderCommandFailureTests.*',
    '*/FrameTextureReadbackTests.*',
    'RenderFrameExecutionTests.CancellingQueuedFramesWithoutADeviceReturnsResourceLeasesAndLatencyBudget',
    'DriverNullDeviceReadinessTests.DestructionUnregistersTheLocatedDriverBeforeBackendFailureNotifications',
    'RenderFrameExecutionTests.UiOnlyFramePresentUsesTheCapturedCompositionBoundary',
    'FrameGraphSceneTargetsTests.VisibilityTransitionsPreferTrackerStateOverResourceReportedState',
    'DriverFlushTests.MainPipe*',
    'RenderFrameExecutionTests.OffscreenPreparedBuilderLeavesPendingUiSnapshotForUiOnlyFrame',
    'RenderFrameExecutionTests.ForwardSceneRendererKeepsFrozenViewAndResolvesObjectCountOnRenderThread',
    'RenderFrameExecutionTests.DeferredSceneRendererQueuesFrozenViewBeforeResolvingAnEmptyNativeScene',
    'RenderFrameExecutionTests.DeferredSceneRendererPublishesPreparedRenderScenePackageWhenThreadedWorkersArePaused',
    'RenderFrameExecutionTests.SwapchainAcquire*',
    'RenderFrameExecutionTests.UnavailableSwapchainImageLeavesTheExecutorUsableInEveryMode',
    'RenderFrameExecutionTests.RejectedSwapchainAcquireClosesAllocatorsWithoutTouchingTheSwapchain',
    'LightingDataProviderTests.RenderFrameLightingSurvivesNextGameThreadCollection',
    'SceneRenderStoreOwnershipTests.*'
) -join ':'
$stressFilter = 'ThreadedRenderingStressPerformanceTests.*:ThreadedRenderingSwapchainPerformanceTests.*'
$fatalPattern = '^\s*NLS_RENDERING_NATIVE_EXCEPTION_|D3D12 (ERROR|CORRUPTION):|' +
    '\[Vulkan\]\[Validation\]\[Error\]|^\[  FAILED|Assertion Failed'
$results = [Collections.Generic.List[object]]::new()

function Invoke-VerifiedTests {
    param([string]$Name, [string]$Executable, [string]$Filter, [int]$Repeat = 1)

    $listPath = Join-Path $OutputDirectory "$Name-tests.log"
    & $Executable "--gtest_filter=$Filter" --gtest_list_tests *> $listPath
    if ($LASTEXITCODE -ne 0) { throw "$Name could not list tests" }
    $expectedCount = @(Select-String -LiteralPath $listPath -Pattern '^  \S').Count
    if ($expectedCount -eq 0) { throw "$Name selected no tests" }
    $xmlPath = Join-Path $OutputDirectory "$Name.xml"
    $consolePath = Join-Path $OutputDirectory "$Name-console.log"
    $nativeLog = Join-Path $OutputDirectory "$Name-cdb.log"
    $capturePath = Join-Path $OutputDirectory "$Name.cdb"
    $symbolPath = $binaryDirectory.Replace('\', '/')
    $dumpPath = (Join-Path $OutputDirectory "$Name-failure.dmp").Replace('\', '/')
    $capture = @(".sympath `"$symbolPath`"")
    foreach ($exception in @('87d', 'av', 'bpe', 'c0000374', 'c0000409')) {
        $capture += "sxe -c `".echo NLS_RENDERING_NATIVE_EXCEPTION_$exception; .exr -1; .ecxr; kv 60; ~* k 24; .dump /m /u \`"$dumpPath\`"; q`" $exception"
    }
    $capture += 'g'
    $capture | Set-Content -LiteralPath $capturePath -Encoding ascii
    $started = [DateTime]::UtcNow
    Write-Output "Starting $Name ($expectedCount tests, repeat=$Repeat)"
    & $CdbPath -G -cf $capturePath -logo $nativeLog $Executable "--gtest_filter=$Filter" `
        "--gtest_output=xml:$xmlPath" "--gtest_repeat=$Repeat" *> $consolePath
    if ($LASTEXITCODE -ne 0) { throw "$Name debugger returned $LASTEXITCODE; see $consolePath" }
    if (!(Test-Path -LiteralPath $xmlPath) -or (Get-Item -LiteralPath $xmlPath).LastWriteTimeUtc -lt $started) {
        throw "$Name did not produce fresh XML; see $consolePath"
    }
    [xml]$xml = Get-Content -LiteralPath $xmlPath
    $suite = $xml.testsuites
    if ([int]$suite.tests -ne $expectedCount -or [int]$suite.failures -ne 0 -or
        [int]$suite.errors -ne 0 -or [int]$suite.disabled -ne 0 -or
        $xml.SelectNodes('//testcase/skipped').Count -ne 0) {
        throw "$Name did not pass every selected test; see $xmlPath"
    }
    if (Select-String -LiteralPath $consolePath, $nativeLog -Pattern $fatalPattern) {
        throw "$Name captured an assertion or native/GPU validation error; see $nativeLog"
    }
    $validationDevices = 0
    if ($Backend -eq 'Vulkan' -and $Name.StartsWith('stress-')) {
        $validationDevices = @(Select-String -LiteralPath $consolePath -Pattern `
            'Inserted device layer "VK_LAYER_KHRONOS_validation"').Count
        # The loader logs every layer insertion more than once per device, so the
        # raw line count is not the device count. Tie the check to the devices the
        # run actually created instead: one Vulkan device per case, and the layer
        # present on each of them.
        $vulkanDeviceCreations = @(Select-String -LiteralPath $consolePath -Pattern `
            'CreateRhiDevice: requested backend = Vulkan').Count
        if ($vulkanDeviceCreations -lt $expectedCount) {
            throw "$Name created $vulkanDeviceCreations Vulkan devices for $expectedCount cases; see $consolePath"
        }
        if ($validationDevices -lt $vulkanDeviceCreations) {
            throw "$Name did not load validation for every device; see $consolePath"
        }
    }
    $passed = @(Select-String -LiteralPath $consolePath -Pattern '^\[  PASSED  \] \d+ tests?\.$')
    if ($passed.Count -ne $Repeat) { throw "$Name has incomplete passing summaries; see $consolePath" }
    foreach ($summary in $passed) {
        if ([int]([regex]::Match($summary.Line, '\d+').Value) -ne $expectedCount) {
            throw "$Name has an incomplete iteration; see $consolePath"
        }
    }
    $results.Add([pscustomobject]@{
        Name = $Name; Tests = $expectedCount; Repeat = $Repeat
        LastIterationSeconds = [double]$suite.time
        XmlPath = $xmlPath
        Backend = $Backend; MinimumStressSeconds = $MinimumStressSeconds
        VulkanValidationDevices = $validationDevices
        VulkanDriverManifest = $VulkanDriverManifest; VulkanLayerPath = $VulkanLayerPath
    })
    $results | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $OutputDirectory 'results.json')
    Write-Output "$Name passed $expectedCount tests (repeat=$Repeat)"
}

$environmentNames = @(
    'NLS_THREADED_STRESS_FRAMES', 'NLS_THREADED_STRESS_TIMEOUT_SECONDS',
    'NLS_THREADED_STRESS_RHI_MODE', 'NLS_THREADED_STRESS_HEAP_CHECK',
    'NLS_THREADED_STRESS_BACKEND', 'NLS_THREADED_STRESS_DISABLE_CHURN',
    'NLS_THREADED_STRESS_DISABLE_SWAP', 'NLS_THREADED_STRESS_DISABLE_RESIZE',
    'NLS_THREADED_STRESS_STEP_MARKERS'
    'NLS_THREADED_STRESS_MIN_SECONDS', 'NLS_VULKAN_VALIDATION', 'VK_DRIVER_FILES',
    'VK_ICD_FILENAMES', 'VK_LAYER_PATH', 'VK_LOADER_DEBUG', 'VK_INSTANCE_LAYERS'
)
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$hashes = @(Get-ChildItem -LiteralPath $binaryDirectory -File |
    Where-Object { $_.Extension -eq '.dll' -or $_.FullName -in @($unit, $performance) } |
    ForEach-Object { [pscustomobject]@{
        Path = $_.FullName; Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    } })
$hashes | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'binaries.json')
Push-Location $repoRoot
try {
    # Recent PowerShell/.NET can bind null as an empty value, which disables Vulkan ICD discovery.
    foreach ($name in $environmentNames) {
        if (Test-Path -LiteralPath "Env:$name") { Remove-Item -LiteralPath "Env:$name" }
    }
    if ($Backend -eq 'Vulkan') {
        $env:NLS_VULKAN_VALIDATION = '1'
        $env:VK_LOADER_DEBUG = 'layer'
        if ($VulkanDriverManifest) {
            $env:VK_DRIVER_FILES = (Resolve-Path -LiteralPath $VulkanDriverManifest).Path
        }
        if ($VulkanLayerPath) {
            $env:VK_LAYER_PATH = (Resolve-Path -LiteralPath $VulkanLayerPath).Path
        }
    }
    if (!$StressOnly) {
        Invoke-VerifiedTests 'regression' $unit $regressionFilter
        Invoke-VerifiedTests 'ownership-failure-repeat' $unit $repeatFilter 5
    }
    $env:NLS_THREADED_STRESS_FRAMES = "$StressFrames"
    $env:NLS_THREADED_STRESS_TIMEOUT_SECONDS = "$StressTimeoutSeconds"
    $env:NLS_THREADED_STRESS_MIN_SECONDS = "$MinimumStressSeconds"
    $env:NLS_THREADED_STRESS_BACKEND = $Backend.ToLowerInvariant()
    $env:NLS_THREADED_STRESS_HEAP_CHECK = if ($Configuration -eq 'Debug') { '1' } else { '0' }
    foreach ($mode in $RhiModes) {
        $env:NLS_THREADED_STRESS_RHI_MODE = $mode
        Invoke-VerifiedTests "stress-$mode-$StressFrames" $performance $stressFilter
    }
    foreach ($binary in $hashes) {
        if ((Get-FileHash -LiteralPath $binary.Path -Algorithm SHA256).Hash -ne $binary.Sha256) {
            throw "Binary changed during validation: $($binary.Path)"
        }
    }
    Write-Output "All verification complete; binary hashes unchanged. Results: $OutputDirectory"
}
finally {
    foreach ($name in $environmentNames) {
        if ($null -eq $savedEnvironment[$name]) {
            if (Test-Path -LiteralPath "Env:$name") { Remove-Item -LiteralPath "Env:$name" }
        }
        else {
            [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
        }
    }
    Pop-Location
}
