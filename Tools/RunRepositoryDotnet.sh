#!/usr/bin/env sh
set -eu

if [ "$#" -lt 1 ]; then
    echo "Repository dotnet path is required." >&2
    exit 2
fi

dotnet_path="$1"
shift
if [ ! -x "$dotnet_path" ]; then
    echo "Repository dotnet executable was not found: $dotnet_path" >&2
    exit 2
fi

dotnet_root=$(CDPATH= cd -- "$(dirname -- "$dotnet_path")" && pwd)
export DOTNET_ROOT="$dotnet_root"
export DOTNET_MULTILEVEL_LOOKUP=0
export DOTNET_CLI_HOME="$dotnet_root/.cli-home"
export DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
export DOTNET_NOLOGO=1
export DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=0

exec "$dotnet_path" "$@"
