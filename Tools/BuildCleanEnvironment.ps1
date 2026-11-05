# Do not use a named PowerShell parameter block here.  CMake has many
# dash-prefixed options (for example -S and -B), and Windows PowerShell tries
# to bind those options to this script before ValueFromRemainingArguments can
# forward them.  Reading the raw argument array keeps this wrapper a
# transparent command launcher.
$invocationArguments = @($args)
if ($invocationArguments.Count -eq 0) {
    throw "Build command is required."
}

$FilePath = [string]$invocationArguments[0]
$ArgumentList = @()
if ($invocationArguments.Count -gt 1) {
    $ArgumentList = @($invocationArguments[1..($invocationArguments.Count - 1)])
}

$resolvedCommand = Get-Command -Name $FilePath -ErrorAction SilentlyContinue
if ($null -eq $resolvedCommand) {
    # IsPathFullyQualified is unavailable in Windows PowerShell/.NET Framework
    # (the shell used by the .cmd entry point). IsPathRooted is sufficient here
    # because the path is validated as an existing file immediately afterwards.
    if (-not [System.IO.Path]::IsPathRooted($FilePath) -or -not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        throw "Build command was not found: $FilePath"
    }
    $resolvedFilePath = $FilePath
}
else {
    $resolvedFilePath = $resolvedCommand.Source
}

# Windows environment names are case-insensitive, but .NET Framework's
# ProcessStartInfo can retain both PATH and Path as distinct keys.  Keep the
# current effective value and replace the process environment with one key.
$effectivePath = [System.Environment]::GetEnvironmentVariable("PATH", "Process")
if ([string]::IsNullOrWhiteSpace($effectivePath)) {
    $effectivePath = [System.Environment]::GetEnvironmentVariable("Path", "Process")
}
if ([string]::IsNullOrWhiteSpace($effectivePath)) {
    throw "The process has no PATH/Path environment value."
}

[System.Environment]::SetEnvironmentVariable("Path", $null, "Process")
[System.Environment]::SetEnvironmentVariable("PATH", $effectivePath, "Process")

# PowerShell also keeps an Env: provider snapshot for native command
# launches. Remove both spellings there as well, otherwise it can re-add the
# stale casing even after the .NET process environment was normalized.
Get-ChildItem Env: |
    Where-Object { $_.Name -match "^(?i:path)$" } |
    Remove-Item -Force
$env:PATH = $effectivePath

$normalizedArguments = @($ArgumentList)
$isCMakeBuild =
    ([System.IO.Path]::GetFileNameWithoutExtension($resolvedFilePath) -ieq "cmake") -and
    ($normalizedArguments -contains "--build")

if ($isCMakeBuild) {
    # PowerShell 7 can consume the native "--" separator before this script
    # receives it. Restore that boundary for native MSBuild switches, while
    # preserving the caller's parallelism and generator arguments.
    $separatorIndex = [System.Array]::IndexOf($normalizedArguments, "--")
    if ($separatorIndex -ge 0) {
        $cmakeArguments = @($normalizedArguments[0..($separatorIndex - 1)])
        $buildArguments = @()
        if ($separatorIndex + 1 -lt $normalizedArguments.Count) {
            $buildArguments = @($normalizedArguments[($separatorIndex + 1)..($normalizedArguments.Count - 1)])
        }
    }
    else {
        # PowerShell 7 treats a bare "--" as its own end-of-parameters marker
        # and removes it before this script sees the arguments, so native
        # switches can arrive blended into the CMake list.  MSBuild switches
        # start with "/" (or "-m"/"-maxcpucount"); CMake options never do.
        # Split them apart so exactly one separator is injected below.
        $cmakeArguments = @()
        $buildArguments = @()
        for ($argumentIndex = 0; $argumentIndex -lt $normalizedArguments.Count; ++$argumentIndex) {
            $argument = $normalizedArguments[$argumentIndex]
            # Windows PowerShell -File can split -m:4 into "-m", "4" before
            # forwarding the script arguments. Rejoin this known native pair.
            if ($argument -match "^-(?:m|maxcpucount)$" -and
                $argumentIndex + 1 -lt $normalizedArguments.Count -and
                $normalizedArguments[$argumentIndex + 1] -match "^[1-9][0-9]*$") {
                ++$argumentIndex
                $buildArguments += "${argument}:$($normalizedArguments[$argumentIndex])"
                continue
            }
            if ($argument.StartsWith('/') -or
                $argument -match "^-(?:m|maxcpucount)(?::|=)?\d*$") {
                $buildArguments += $argument
            }
            else {
                $cmakeArguments += $argument
            }
        }
    }
    if ($separatorIndex -ge 0 -or @($buildArguments).Count -gt 0) {
        $normalizedArguments = @($cmakeArguments + "--" + $buildArguments)
    }
    else {
        $normalizedArguments = @($cmakeArguments)
    }
}

& $resolvedFilePath @normalizedArguments
exit $LASTEXITCODE
