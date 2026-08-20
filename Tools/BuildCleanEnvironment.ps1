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
    # The Visual Studio generator creates additional MSBuild nodes. With the
    # malformed parent environment those nodes reintroduce both casings, so
    # keep this wrapper deterministic until the host environment is corrected.
    $separatorIndex = [System.Array]::IndexOf($normalizedArguments, "--")
    if ($separatorIndex -ge 0) {
        $cmakeArguments = @($normalizedArguments[0..($separatorIndex - 1)])
        $buildArguments = @($normalizedArguments[($separatorIndex + 1)..($normalizedArguments.Count - 1)]) |
            Where-Object { $_ -notmatch "^(?:/|-)(?:m|maxcpucount)(?::|=)?\d*$" }
    }
    else {
        $cmakeArguments = $normalizedArguments
        $buildArguments = @()
    }
    $normalizedArguments = @($cmakeArguments + "--" + "/m:1" + $buildArguments)
}

& $resolvedFilePath @normalizedArguments
exit $LASTEXITCODE
