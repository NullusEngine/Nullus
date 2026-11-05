param(
    [Parameter(Mandatory=$true)][string]$ProductDirectory,
    [Parameter(Mandatory=$true)][string]$EvidenceDirectory,
    [switch]$HideSpriteAuthoringSources,
    [string]$ExpectedStartScene='Scenes/Main.scene',
    [ValidateRange(1,1000000)][int]$ExpectedSubmittedSprites=1,
    [ValidateRange(1,1000000)][int]$ExpectedVisibleSprites=1,
    [ValidateRange(0.001,1.0)][double]$MaximumBlueCoverage=0.05
)
$ErrorActionPreference='Stop'
if([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT){throw 'This acceptance runner requires Windows/DX12.'}
$productRoot=(Resolve-Path -LiteralPath $ProductDirectory).Path
$evidenceRoot=[IO.Path]::GetFullPath($EvidenceDirectory)
[void][IO.Directory]::CreateDirectory($evidenceRoot)
$descriptor=Get-Content -LiteralPath (Join-Path $productRoot 'Player.info') -Raw | ConvertFrom-Json
if($descriptor.startScene -ne $ExpectedStartScene){throw "Unexpected product start scene: $($descriptor.startScene)"}
if($ExpectedVisibleSprites -gt $ExpectedSubmittedSprites){throw 'Visible Sprite count cannot exceed submitted count.'}
if([string]::IsNullOrEmpty($descriptor.scripts.scriptApiDirectory)){throw 'The product has no explicit Script API contract.'}
$apiRoot=Join-Path $productRoot $descriptor.scripts.scriptApiDirectory
foreach($contractFile in @('ScriptApi.json','ScriptApi.bin')){
    if(-not (Test-Path -LiteralPath (Join-Path $apiRoot $contractFile))){throw "Missing Script API contract file: $contractFile"}
}
$manifest=Get-Content -LiteralPath (Join-Path $productRoot $descriptor.runtimeManifestPath) -Raw | ConvertFrom-Json
foreach($loader in @('sprite','texture','shader')){
    if(-not @($manifest.entries | Where-Object loaderId -eq $loader).Count){throw "Missing $loader artifact."}
}
if(Test-Path -LiteralPath (Join-Path $productRoot 'Data/User/Assets/Textures/Hero.png')){throw 'Source Hero.png was shipped.'}
$usesCSharp=@($descriptor.scriptBackends) -contains 'csharp'
if($usesCSharp -and [string]::IsNullOrEmpty($descriptor.scripts.dotnetRoot)){throw 'The selected C# host has no bundled runtime.'}
$imagePath=Join-Path $evidenceRoot 'presented.png'
$reportPath=Join-Path $evidenceRoot 'readback-report.txt'
$stdout=Join-Path $evidenceRoot 'player.log'
$stderr=Join-Path $evidenceRoot 'player-error.log'
foreach($path in @($imagePath,$reportPath,$stdout,$stderr,(Join-Path $evidenceRoot 'evidence.json'))){
    if(Test-Path -LiteralPath $path){throw 'Evidence files already exist; choose a fresh EvidenceDirectory.'}
}
$hiddenFiles=@()
$player=$null
try {
    if($HideSpriteAuthoringSources){
        foreach($relative in @('Assets/Engine/Shaders/ShaderLab/SpriteUnlit.shader','Assets/Engine/Shaders/NullusShaderLibrary/SpriteUnlit.hlsl')){
            $source=Join-Path $productRoot $relative
            if(Test-Path -LiteralPath $source){
                $hidden=$source+'.sprite-validation-disabled'
                if(Test-Path -LiteralPath $hidden){throw "Validation backup already exists: $hidden"}
                Move-Item -LiteralPath $source -Destination $hidden
                $hiddenFiles+=@{Source=$source;Hidden=$hidden}
            }
        }
    }
    $arguments=@('--backend','dx12','--no-renderdoc','--sprite-template-validation-output',('"'+$imagePath+'"'),
        '--material-validation-summary',('"'+$reportPath+'"'),'--material-validation-after-frames','300','--material-validation-swapchain')
    $player=Start-Process -FilePath (Join-Path $productRoot 'Game.exe') -ArgumentList $arguments -WorkingDirectory $productRoot `
        -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $modules=@{}
    $deadline=[DateTime]::UtcNow.AddSeconds(40)
    do {
        if($player.HasExited){break}
        try {
            foreach($module in (Get-Process -Id $player.Id).Modules){$modules[$module.ModuleName]=$module.FileName}
        } catch [System.InvalidOperationException] {}
        Start-Sleep -Milliseconds 100
    } while([DateTime]::UtcNow -lt $deadline)
    if(-not $player.WaitForExit(120000)){throw 'The owned validation Player did not finish within 120 seconds.'}
    if($player.ExitCode -ne 0){throw "Player exited with $($player.ExitCode)."}
    $playerLog=Get-Content -LiteralPath $stdout -Raw
    $spriteStats=[regex]::Match($playerLog,'\[SpriteValidation\] submitted=(\d+) visible=(\d+) culled=(\d+) batches=(\d+) drawCalls=(\d+) cpuExpanded=(\d+) prepared=(\d+) activeLeases=(\d+)')
    if(-not $spriteStats.Success -or [int]$spriteStats.Groups[1].Value -ne $ExpectedSubmittedSprites -or
       [int]$spriteStats.Groups[2].Value -ne $ExpectedVisibleSprites -or
       [int]$spriteStats.Groups[3].Value -ne ($ExpectedSubmittedSprites-$ExpectedVisibleSprites) -or
       $spriteStats.Groups[4].Value -ne '1' -or $spriteStats.Groups[5].Value -ne '1' -or $spriteStats.Groups[7].Value -ne '1'){
        throw 'The product did not match its expected Sprite visibility and single-draw batching counts.'
    }
    if($usesCSharp -and -not $playerLog.Contains('C#=on')){throw 'The selected C# host did not initialize.'}
    if(@($descriptor.scriptBackends) -contains 'lua' -and -not $playerLog.Contains('Lua=on')){throw 'The selected Lua host did not initialize.'}
    $engineModules=@($modules.Keys | Where-Object { $_ -in @('NLS_Engine.dll','NLS_Engined.dll') })
    if($engineModules.Count -ne 1){throw 'Native module capture did not observe exactly one Engine library.'}
    if($usesCSharp -and -not $modules.ContainsKey('coreclr.dll')){throw 'The selected C# runtime was not observed.'}
    $windowsRoot=[Environment]::GetFolderPath([Environment+SpecialFolder]::Windows)+[IO.Path]::DirectorySeparatorChar
    foreach($modulePath in $modules.Values){
        if(-not $modulePath.StartsWith($productRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -and
           -not $modulePath.StartsWith($windowsRoot,[StringComparison]::OrdinalIgnoreCase)){
            throw "Native dependency outside the product and Windows directories: $modulePath"
        }
    }
    Add-Type -AssemblyName System.Drawing
    $bitmap=[Drawing.Bitmap]::new($imagePath)
    try {
        $rect=[Drawing.Rectangle]::new(0,0,$bitmap.Width,$bitmap.Height)
        $locked=$bitmap.LockBits($rect,[Drawing.Imaging.ImageLockMode]::ReadOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            if($locked.Stride -le 0){throw 'Unexpected readback row layout.'}
            $bytes=[byte[]]::new($locked.Stride*$bitmap.Height)
            [Runtime.InteropServices.Marshal]::Copy($locked.Scan0,$bytes,0,$bytes.Length)
        } finally {$bitmap.UnlockBits($locked)}
        if(-not ('NullusSpritePixelEvidence' -as [type])) { Add-Type -TypeDefinition @'
public static class NullusSpritePixelEvidence {
    public static int CountBlue(byte[] bytes) {
        int count=0;
        for(int i=0;i+3<bytes.Length;i+=4)
            if(bytes[i]>150 && bytes[i+1]>100 && bytes[i+2]<100 && bytes[i+3]>200) ++count;
        return count;
    }
}
'@
        }
        $bluePixels=[NullusSpritePixelEvidence]::CountBlue($bytes)
        if($bluePixels -lt 500 -or $bluePixels -gt $bitmap.Width*$bitmap.Height*$MaximumBlueCoverage){throw "Unexpected template Sprite coverage: $bluePixels blue pixels."}
        $evidence=[ordered]@{Result='PASSED';Fixture='Existing 2D project';Width=$bitmap.Width;Height=$bitmap.Height;
            ExpectedSubmittedSprites=$ExpectedSubmittedSprites;ExpectedVisibleSprites=$ExpectedVisibleSprites;
            BluePixels=$bluePixels;AuthoringSourcesHidden=[bool]$HideSpriteAuthoringSources;
            MaximumBlueCoverage=$MaximumBlueCoverage;
            RuntimeManifestEntries=@($manifest.entries).Count;SpriteStatistics=$spriteStats.Value;
            EngineModule=$engineModules[0];NativeModules=$modules}
        $evidence | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'evidence.json') -Encoding utf8
        [pscustomobject]$evidence | Select-Object Result,Width,Height,BluePixels,AuthoringSourcesHidden
    } finally {$bitmap.Dispose()}
} finally {
    try {
        if($player -and -not $player.HasExited){$player.Kill();$player.WaitForExit()}
    } finally {
        foreach($pair in $hiddenFiles){Move-Item -LiteralPath $pair.Hidden -Destination $pair.Source}
    }
}
