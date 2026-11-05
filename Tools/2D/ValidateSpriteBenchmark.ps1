param(
    [Parameter(Mandatory=$true)][string]$ProductDirectory,
    [Parameter(Mandatory=$true)][string]$BenchmarkScene,
    [Parameter(Mandatory=$true)][string]$EvidenceDirectory,
    [ValidateRange(1,100000)][int]$ExpectedSprites=10000,
    [ValidateRange(1,10000)][int]$Frames=60
)
$ErrorActionPreference='Stop'
if([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT){throw 'This runner requires Windows/DX12.'}
$sourceProduct=(Resolve-Path -LiteralPath $ProductDirectory).Path
$scene=(Resolve-Path -LiteralPath $BenchmarkScene).Path
$evidence=[IO.Path]::GetFullPath($EvidenceDirectory)
if(Test-Path -LiteralPath $evidence){throw 'Choose a fresh evidence directory.'}
[void][IO.Directory]::CreateDirectory($evidence)
$temporaryRoot=Join-Path ([IO.Path]::GetTempPath()) ('NullusSpriteBenchmark_'+[guid]::NewGuid().ToString('N'))
$product=Join-Path $temporaryRoot 'Product'
[void][IO.Directory]::CreateDirectory($temporaryRoot)
Copy-Item -LiteralPath $sourceProduct -Destination $product -Recurse
$descriptor=Get-Content -LiteralPath (Join-Path $product 'Player.info') -Raw | ConvertFrom-Json
$assets=[IO.Path]::GetFullPath((Join-Path $product 'Data/User/Assets'))
$runtimeScene=[IO.Path]::GetFullPath((Join-Path $assets $descriptor.startScene))
if(-not $runtimeScene.StartsWith($assets+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){
    throw 'The product scene path escapes its Assets directory.'
}
if(-not (Test-Path -LiteralPath $runtimeScene -PathType Leaf)){throw 'The product has no loose runtime start scene.'}
Copy-Item -LiteralPath $scene -Destination $runtimeScene
$image=Join-Path $evidence 'presented.png'
$summary=Join-Path $evidence 'readback-report.txt'
$stdout=Join-Path $evidence 'player.log'
$stderr=Join-Path $evidence 'player-error.log'
$watch=[Diagnostics.Stopwatch]::StartNew()
$owned=Start-Process -FilePath (Join-Path $product 'Game.exe') -WorkingDirectory $product -WindowStyle Hidden -PassThru `
    -ArgumentList @('--backend','dx12','--no-renderdoc','--sprite-template-validation-output',('"'+$image+'"'),
        '--material-validation-summary',('"'+$summary+'"'),'--material-validation-after-frames',$Frames,'--material-validation-swapchain') `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$deadline=[DateTime]::UtcNow.AddMinutes(5)
while(-not $owned.WaitForExit(1000)){
    if([DateTime]::UtcNow -gt $deadline){
        [void]$owned.CloseMainWindow()
        throw "Owned benchmark Player exceeded five minutes (PID $($owned.Id)); normal close requested."
    }
}
$watch.Stop()
$owned.Refresh()
$log=Get-Content -LiteralPath $stdout -Raw
$statistics=[regex]::Match($log,'\[SpriteValidation\] submitted=(\d+) visible=(\d+) culled=(\d+) batches=(\d+) drawCalls=(\d+) cpuExpanded=(\d+) prepared=(\d+) activeLeases=(\d+)')
$summaryText=if(Test-Path -LiteralPath $summary){Get-Content -LiteralPath $summary -Raw}else{''}
$engineFiles=@(Get-ChildItem -LiteralPath $product -Filter 'NLS_Engine*.dll' -File)
if($engineFiles.Count -ne 1){throw 'The product must contain one Engine DLL.'}
$passed=$owned.ExitCode -eq 0 -and $statistics.Success -and
    [int]$statistics.Groups[1].Value -eq $ExpectedSprites -and [int]$statistics.Groups[2].Value -eq $ExpectedSprites -and
    $statistics.Groups[3].Value -eq '0' -and $statistics.Groups[4].Value -eq '1' -and $statistics.Groups[5].Value -eq '1' -and
    $statistics.Groups[7].Value -eq '1' -and $statistics.Groups[8].Value -eq '2' -and
    (Test-Path -LiteralPath $image) -and $summaryText.Contains('PASS')
$record=[ordered]@{
    Result=$(if($passed){'PASSED'}else{'FAILED'})
    Scope='Functional Sprite batch/readback acceptance; no performance threshold or percentile assertion'
    Product=$product; ExpectedSprites=$ExpectedSprites; Frames=$Frames; ExitCode=$owned.ExitCode
    TotalSeconds=$watch.Elapsed.TotalSeconds
    TimingScope='Includes process startup, scene loading, rendering, readback and exit; not steady-state frame time'
    SpriteStatistics=$statistics.Value
    SceneSha256=(Get-FileHash -LiteralPath $scene -Algorithm SHA256).Hash
    EngineSha256=(Get-FileHash -LiteralPath $engineFiles[0].FullName -Algorithm SHA256).Hash
}
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidence 'evidence.json') -Encoding utf8NoBOM
$record
if(-not $passed){throw 'Sprite benchmark functional acceptance failed; see the evidence directory.'}
