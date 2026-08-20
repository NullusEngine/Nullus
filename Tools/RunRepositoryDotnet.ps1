# Invoke the repository-pinned SDK with a canonical Windows environment.
# The Codex/IDE host can provide both Path and PATH. .NET/MSBuild rejects that
# duplicate pair when it materializes a child environment, so normalize it at
# the last repository-local SDK boundary instead of changing the user's PATH.
$invocationArguments = @($args)
if ($invocationArguments.Count -lt 1) {
    throw "Repository dotnet path is required."
}

$dotnetPath = [string]$invocationArguments[0]
$dotnetArguments = @()
if ($invocationArguments.Count -gt 1) {
    $dotnetArguments = @($invocationArguments[1..($invocationArguments.Count - 1)])
}

if (-not [System.IO.Path]::IsPathRooted($dotnetPath) -or
    -not (Test-Path -LiteralPath $dotnetPath -PathType Leaf)) {
    throw "Repository dotnet executable was not found: $dotnetPath"
}

$effectivePath = [System.Environment]::GetEnvironmentVariable("Path", "Process")
if ([string]::IsNullOrWhiteSpace($effectivePath)) {
    $effectivePath = [System.Environment]::GetEnvironmentVariable("PATH", "Process")
}
if ([string]::IsNullOrWhiteSpace($effectivePath)) {
    throw "The process has no PATH/Path environment value."
}

[System.Environment]::SetEnvironmentVariable("Path", $null, "Process")
[System.Environment]::SetEnvironmentVariable("PATH", $effectivePath, "Process")
Get-ChildItem Env: |
    Where-Object { $_.Name -match "^(?i:path)$" } |
    Remove-Item -Force
$env:PATH = $effectivePath

$dotnetRoot = Split-Path -Parent $dotnetPath
$env:DOTNET_ROOT = $dotnetRoot
$env:DOTNET_ROOT_X64 = $dotnetRoot
$env:DOTNET_MULTILEVEL_LOOKUP = "0"
$env:DOTNET_CLI_HOME = Join-Path $dotnetRoot ".cli-home"
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"
$env:DOTNET_NOLOGO = "1"
$env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = "0"

& $dotnetPath @dotnetArguments
exit $LASTEXITCODE
