#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)

if command -v python3 >/dev/null 2>&1; then
    PYTHON=python3
elif command -v python >/dev/null 2>&1; then
    PYTHON=python
else
    echo "Python 3.8 or newer is required to run SetupDependencies. Install Python 3.8+ or add it to PATH." >&2
    exit 1
fi

exec "$PYTHON" "$SCRIPT_DIR/Tools/SetupDependencies/setup_dependencies.py" "$@"
