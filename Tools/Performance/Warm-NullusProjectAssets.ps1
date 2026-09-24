[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectPath,

    [string]$EditorPath = '',

    [ValidateRange(1, 120)]
    [int]$MinimumFreeSpaceGB = 40,

    [ValidateRange(30, 3600)]
    [int]$TimeoutSeconds = 1800
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$projectFile = [IO.Path]::GetFullPath($ProjectPath)
if (-not (Test-Path -LiteralPath $projectFile -PathType Leaf)) {
    throw "Project file does not exist: $projectFile"
}

if ([string]::IsNullOrWhiteSpace($EditorPath)) {
    $EditorPath = Join-Path $repositoryRoot 'App\Win64_RelWithDebInfo_Runtime_Shared\Editor.exe'
}
$EditorPath = [IO.Path]::GetFullPath($EditorPath)
if (-not (Test-Path -LiteralPath $EditorPath -PathType Leaf)) {
    throw "Editor executable does not exist: $EditorPath"
}

if (Get-Process -Name Editor -ErrorAction SilentlyContinue) {
    throw 'Editor.exe is already running. Stop it before warming project assets.'
}

$artifactsRoot = Join-Path (Split-Path -Parent $projectFile) 'Library\Artifacts'
$artifactsBytes = 0L
if (Test-Path -LiteralPath $artifactsRoot -PathType Container) {
    $artifactsBytes = [long](Get-ChildItem -LiteralPath $artifactsRoot -Recurse -File -ErrorAction SilentlyContinue |
        Measure-Object Length -Sum).Sum
}
$requiredBytes = [long]([math]::Max($MinimumFreeSpaceGB * 1GB, [math]::Ceiling($artifactsBytes * 1.25)))
$drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($projectFile))
if ($drive.AvailableFreeSpace -lt $requiredBytes) {
    throw ("Insufficient free space. Required={0:N1} GB, available={1:N1} GB." -f `
        ($requiredBytes / 1GB), ($drive.AvailableFreeSpace / 1GB))
}

$runRoot = Join-Path $repositoryRoot ('Build\AssetPrewarm\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$stdoutPath = Join-Path $runRoot 'prewarm.stdout.log'
$stderrPath = Join-Path $runRoot 'prewarm.stderr.log'
$startedAt = Get-Date

$process = Start-Process -FilePath $EditorPath `
    -ArgumentList @('--editor-prewarm-assets', $projectFile) `
    -WorkingDirectory (Split-Path -Parent $EditorPath) `
    -RedirectStandardOutput $stdoutPath `
    -RedirectStandardError $stderrPath `
    -PassThru

if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    Stop-Process -Id $process.Id -Force
    throw "Asset prewarm timed out after $TimeoutSeconds seconds. Logs: $runRoot"
}

$stdout = if (Test-Path -LiteralPath $stdoutPath) { Get-Content -LiteralPath $stdoutPath -Raw } else { '' }
$stderr = if (Test-Path -LiteralPath $stderrPath) { Get-Content -LiteralPath $stderrPath -Raw } else { '' }
if ($process.ExitCode -ne 0) {
    throw "Asset prewarm failed with exit code $($process.ExitCode). Logs: $runRoot`n$stderr"
}
if ($stdout -notmatch 'Asset prewarm completed:') {
    throw "Asset prewarm did not report completion. Logs: $runRoot"
}

$artifactBytesAfter = 0L
if (Test-Path -LiteralPath $artifactsRoot -PathType Container) {
    $artifactBytesAfter = [long](Get-ChildItem -LiteralPath $artifactsRoot -Recurse -File -ErrorAction SilentlyContinue |
        Measure-Object Length -Sum).Sum
}

[pscustomobject]@{
    Project = $projectFile
    Editor = $EditorPath
    StartedAt = $startedAt
    ElapsedSeconds = [math]::Round(((Get-Date) - $startedAt).TotalSeconds, 3)
    ArtifactsBeforeGB = [math]::Round($artifactsBytes / 1GB, 2)
    ArtifactsAfterGB = [math]::Round($artifactBytesAfter / 1GB, 2)
    Logs = $runRoot
} | Format-List
