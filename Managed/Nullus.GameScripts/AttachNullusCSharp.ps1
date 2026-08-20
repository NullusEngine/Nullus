[CmdletBinding()]
param(
    [int]$EditorPid = 0,
    [string]$SolutionPath = ""
)

$ErrorActionPreference = "Stop"

function Get-VisualStudioDte([int]$ProcessId) {
    foreach ($version in @("17.0", "16.0")) {
        try {
            $moniker = "VisualStudio.DTE.${version}:$ProcessId"
            return [Runtime.InteropServices.Marshal]::GetActiveObject($moniker)
        } catch {
            # The VS instance may not expose automation until its solution is ready.
        }
    }
    return $null
}

function Resolve-EditorProcess {
    if ($EditorPid -gt 0) {
        $process = Get-Process -Id $EditorPid -ErrorAction Stop
        if ($process.ProcessName -ne "Editor") {
            throw "Process $EditorPid is not a Nullus Editor process."
        }
        return $process
    }

    $candidates = @(Get-Process -Name Editor -ErrorAction SilentlyContinue)
    if ($candidates.Count -eq 0) {
        throw "No running Editor.exe was found. Start Nullus Editor and enter Play first."
    }
    if ($candidates.Count -gt 1) {
        $ids = ($candidates | ForEach-Object { $_.Id }) -join ", "
        throw "More than one Editor.exe is running ($ids). Pass -EditorPid explicitly."
    }
    return $candidates[0]
}

$editor = Resolve-EditorProcess
$expectedSolution = if ([string]::IsNullOrWhiteSpace($SolutionPath)) {
    ""
} else {
    [IO.Path]::GetFullPath($SolutionPath)
}

$visualStudioProcesses = @(Get-Process -Name devenv -ErrorAction SilentlyContinue)
foreach ($visualStudio in $visualStudioProcesses) {
    $dte = Get-VisualStudioDte $visualStudio.Id
    if ($null -eq $dte) {
        continue
    }

    $openedSolution = ""
    try { $openedSolution = [IO.Path]::GetFullPath([string]$dte.Solution.FullName) } catch { }
    if ($expectedSolution -and $openedSolution -and
        -not [StringComparer]::OrdinalIgnoreCase.Equals($openedSolution, $expectedSolution)) {
        continue
    }

    $localProcess = @($dte.Debugger.LocalProcesses) |
        Where-Object { $_.ProcessID -eq $editor.Id } |
        Select-Object -First 1
    if ($null -eq $localProcess) {
        continue
    }

    try {
        $localProcess.Attach()
    } catch {
        throw "Visual Studio found Editor.exe but could not attach Managed/CoreCLR debugging: $($_.Exception.Message)"
    }
    Write-Host "Attached Visual Studio $($visualStudio.Id) to Editor.exe ($($editor.Id))."
    exit 0
}

throw "No open Visual Studio instance owns the requested solution. Open the generated Nullus.GameScripts project first."
