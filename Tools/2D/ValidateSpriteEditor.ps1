param(
    [Parameter(Mandatory=$true)][string]$EditorExecutable,
    [Parameter(Mandatory=$true)][string]$ProjectFile,
    [Parameter(Mandatory=$true)][string]$Texture,
    [Parameter(Mandatory=$true)][string]$EvidenceDirectory
)
$ErrorActionPreference='Stop'
$editor=(Resolve-Path -LiteralPath $EditorExecutable).Path
$project=(Resolve-Path -LiteralPath $ProjectFile).Path
if([IO.Path]::GetExtension($project) -ne '.nullus'){throw 'Select an existing .nullus project.'}
$projectRoot=Split-Path -Parent $project
$source=[IO.Path]::GetFullPath((Join-Path $projectRoot $Texture))
$assets=[IO.Path]::GetFullPath((Join-Path $projectRoot 'Assets'))
if(-not $source.StartsWith($assets+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){
    throw 'The validation texture must be inside the existing project Assets folder.'
}
$meta=$source+'.meta'
if(-not (Test-Path -LiteralPath $source -PathType Leaf) -or -not (Test-Path -LiteralPath $meta -PathType Leaf)){
    throw 'Import an existing validation texture before running this check.'
}
# Project locking is also enforced by Editor. Check first to avoid touching a
# project that is already open, even when it uses another build of Editor.
$running=Get-CimInstance Win32_Process -Filter "Name='Editor.exe'" | Where-Object {
    $_.CommandLine -and $_.CommandLine.Contains($project)
}
if($running){throw "This project already has a running Editor (PID $($running.ProcessId -join ', '))."}
$evidence=[IO.Path]::GetFullPath($EvidenceDirectory)
if(Test-Path -LiteralPath $evidence){throw 'Choose a fresh evidence directory.'}
[void][IO.Directory]::CreateDirectory($evidence)
$backup=Join-Path $evidence 'original.meta'
Copy-Item -LiteralPath $meta -Destination $backup
$originalHash=(Get-FileHash -LiteralPath $meta -Algorithm SHA256).Hash
$output=Join-Path $evidence 'workflow.json'
$oldSource=[Environment]::GetEnvironmentVariable('NLS_SPRITE_EDITOR_VALIDATION_SOURCE','Process')
$oldOutput=[Environment]::GetEnvironmentVariable('NLS_SPRITE_EDITOR_VALIDATION_OUTPUT','Process')
$owned=$null
$result=$null
$normalExit=$false
try{
    [Environment]::SetEnvironmentVariable('NLS_SPRITE_EDITOR_VALIDATION_SOURCE',$Texture.Replace('\','/'),'Process')
    [Environment]::SetEnvironmentVariable('NLS_SPRITE_EDITOR_VALIDATION_OUTPUT',$output,'Process')
    $owned=Start-Process -FilePath $editor -WorkingDirectory (Split-Path -Parent $editor) -WindowStyle Hidden -PassThru `
        -ArgumentList @(('"'+$project+'"'),'--backend','dx12','--no-renderdoc') `
        -RedirectStandardOutput (Join-Path $evidence 'editor.log') -RedirectStandardError (Join-Path $evidence 'editor-error.log')
    # Open the process handle while it exists so the exit status remains available.
    $processHandle=$owned.Handle
    @{ProcessId=$owned.Id;Project=$project;Texture=$Texture;Editor=$editor;StartedUtc=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'launch.json') -Encoding utf8NoBOM
    $deadline=[DateTime]::UtcNow.AddMinutes(4)
    while(-not (Test-Path -LiteralPath $output) -and -not $owned.WaitForExit(500)){
        if([DateTime]::UtcNow -gt $deadline){throw 'Sprite Editor validation timed out.'}
    }
    if(Test-Path -LiteralPath $output){$result=Get-Content -LiteralPath $output -Raw | ConvertFrom-Json}
    Add-Type -AssemblyName System.Drawing
    if(-not ('NullusSpriteEditorReadback' -as [type])){
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NullusSpriteEditorReadback {
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
 [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr context);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window,out Rect rect);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window,IntPtr dc,uint flags);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindow callback,IntPtr parameter);
 public delegate bool EnumWindow(IntPtr window,IntPtr parameter);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window,out uint process);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window,System.Text.StringBuilder title,int capacity);
 public static string WindowTitles(int process) {
   var result=new System.Text.StringBuilder();
   EnumWindows((window,parameter)=>{uint id;GetWindowThreadProcessId(window,out id);if(id==(uint)process){var title=new System.Text.StringBuilder(512);GetWindowText(window,title,512);result.AppendLine(window.ToString()+": "+title);}return true;},IntPtr.Zero);
   return result.ToString();
 }
}
'@
    }
    [void][NullusSpriteEditorReadback]::SetProcessDpiAwarenessContext([IntPtr](-4))
    function SaveSpriteEditorWindow([string]$destination){
        $owned.Refresh()
        $rect=New-Object NullusSpriteEditorReadback+Rect
        if([NullusSpriteEditorReadback]::GetWindowRect($owned.MainWindowHandle,[ref]$rect)){
            $bitmap=New-Object Drawing.Bitmap(($rect.Right-$rect.Left),($rect.Bottom-$rect.Top))
            $graphics=[Drawing.Graphics]::FromImage($bitmap)
            try{
                $dc=$graphics.GetHdc()
                try{$captured=[NullusSpriteEditorReadback]::PrintWindow($owned.MainWindowHandle,$dc,2)}
                finally{$graphics.ReleaseHdc($dc)}
                if($captured){$bitmap.Save($destination,[Drawing.Imaging.ImageFormat]::Png)}
            }finally{$graphics.Dispose();$bitmap.Dispose()}
        }
    }
    SaveSpriteEditorWindow (Join-Path $evidence 'workbench.png')
    if(-not $result -or -not $result.success){throw "Sprite Editor workflow failed: $($result.error)"}
}finally{
    [Environment]::SetEnvironmentVariable('NLS_SPRITE_EDITOR_VALIDATION_SOURCE',$oldSource,'Process')
    [Environment]::SetEnvironmentVariable('NLS_SPRITE_EDITOR_VALIDATION_OUTPUT',$oldOutput,'Process')
    if($owned){
        if(-not $owned.HasExited){
            $closeRequested=$owned.CloseMainWindow()
            @{ProcessId=$owned.Id;CloseRequested=$closeRequested;RequestedUtc=[DateTime]::UtcNow.ToString('o')} |
                ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'close-request.json') -Encoding utf8NoBOM
            $closeStart=[DateTime]::UtcNow
            $closeDeadline=$closeStart.AddSeconds(180)
            $capturedClose=$false
            do{
                $normalExit=$owned.WaitForExit(500)
                if(-not $normalExit -and -not $capturedClose -and ([DateTime]::UtcNow-$closeStart).TotalSeconds -ge 5 -and
                    (Get-Command SaveSpriteEditorWindow -ErrorAction SilentlyContinue)){
                    [NullusSpriteEditorReadback]::WindowTitles($owned.Id) | Set-Content -LiteralPath (Join-Path $evidence 'close-windows.txt') -Encoding utf8NoBOM
                    SaveSpriteEditorWindow (Join-Path $evidence 'close-window.png');$capturedClose=$true
                }
            }while(-not $normalExit -and [DateTime]::UtcNow -lt $closeDeadline)
        }else{$normalExit=$true}
        if($normalExit){
            $owned.Refresh()
            @{ProcessId=$owned.Id;ExitCode=$owned.ExitCode;EndedUtc=[DateTime]::UtcNow.ToString('o')} |
                ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'exit.json') -Encoding utf8NoBOM
        }else{Write-Warning "Normal close did not finish; owned Editor PID $($owned.Id) was left running. Original metadata is in $backup."}
    }
}
if(-not $normalExit -or $owned.ExitCode -ne 0){throw 'The owned Editor did not exit normally.'}
$restoredHash=(Get-FileHash -LiteralPath $meta -Algorithm SHA256).Hash
$importLog=Get-Content -LiteralPath (Join-Path $evidence 'editor.log') -Raw
$importErrors=[regex]::IsMatch($importLog,'Importer Apply (completion invalidated|completion stale|failed:)')
$record=@{
    success=($originalHash -eq $restoredHash -and -not $importErrors); originalMetadataHash=$originalHash; finalMetadataHash=$restoredHash
    importerCompletionErrors=$importErrors
    scope='Live Editor document, async Apply/reimport, published Sprite descriptors and global Undo/Redo. Desktop mouse acceptance is separate.'
}
$record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'evidence.json') -Encoding utf8NoBOM
if(-not $record.success){throw "Final importer/hash validation failed; original metadata is preserved at $backup."}
$record | ConvertTo-Json
