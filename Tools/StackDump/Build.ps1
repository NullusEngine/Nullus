<#
.SYNOPSIS
    Builds StackDump.exe, a cross-thread native stack dumper.

.DESCRIPTION
    A hung or deadlocked Nullus process cannot be diagnosed from its logs alone:
    the faulting thread is usually blocked inside a synchronisation primitive and
    never reports anything. This tool walks every thread of a live process with
    DbgHelp and resolves symbols against the build's own PDBs, so the blocking
    call chain - including source file and line - can be read directly.

    It needs no debugger product installed. It links against the Windows SDK's
    DbgHelp, which is already present wherever the SDK is.

.EXAMPLE
    # Sample every thread of process 1234, 60 frames each.
    .\Build.ps1
    .\StackDump.exe 1234 "$PWD\..\..\build\Runtime\Debug;$PWD\..\..\build\bin\Debug"
#>
[CmdletBinding()]
param(
    [string] $Configuration = 'Debug',
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir '..\..')).Path
$source = Join-Path $scriptDir 'StackDump.cpp'
$output = Join-Path $scriptDir 'StackDump.exe'

if ($Clean -and (Test-Path -LiteralPath $output)) {
    Remove-Item -LiteralPath $output -Force
}

# Locate a Visual Studio installation that carries the C++ toolset.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found; install Visual Studio with the C++ toolset."
}

$vsRoot = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if ([string]::IsNullOrWhiteSpace($vsRoot)) {
    throw "No Visual Studio installation provides the x64 C++ toolset."
}

$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found under '$vsRoot'."
}

# Newest installed Windows SDK provides DbgHelp.h / DbgHelp.lib and vcvars
# already places them on the include and library paths.
$compile = 'call "{0}" >nul 2>&1 && cl /nologo /EHsc /W4 /O2 /Fe:"{1}" /Fo:"{2}\\" "{3}" /link dbghelp.lib' -f `
    $vcvars, $output, $scriptDir, $source

Write-Host "Building StackDump.exe ($Configuration)..."
& cmd /c $compile
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

if (-not (Test-Path -LiteralPath $output)) {
    throw "Build reported success but '$output' was not produced."
}

Write-Host "Built: $output"
Write-Host "Symbol path for Nullus Debug builds:"
Write-Host "  $repoRoot\build\Runtime\Debug;$repoRoot\build\bin\Debug"
