#!/usr/bin/env python3
from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def resolve_rdc() -> Path | None:
    discovered = shutil.which("rdc")
    if discovered:
        return Path(discovered)
    return None


def resolve_renderdoc_python_module() -> Path | None:
    configured = os.environ.get("RENDERDOC_PYTHON_PATH")
    if configured:
        candidate = Path(configured).expanduser()
        if candidate.exists():
            return candidate
    candidate = Path.home() / "AppData" / "Local" / "rdc" / "renderdoc"
    if candidate.exists():
        return candidate
    return None


def main() -> int:
    rdc_path = resolve_rdc()
    python_module = resolve_renderdoc_python_module()

    print("[rdc_selfcheck] repo_root=" + str(REPO_ROOT))
    print("[rdc_selfcheck] rdc_cli=" + (str(rdc_path) if rdc_path else "not found"))
    print("[rdc_selfcheck] renderdoc_python=" + (str(python_module) if python_module else "not found"))

    if python_module:
        sys.path.insert(0, str(python_module))
        try:
            import renderdoc as rd  # type: ignore
            print("[rdc_selfcheck] renderdoc_version=" + rd.GetVersionString())
        except Exception as exc:
            print("[rdc_selfcheck] renderdoc_import_failed=" + repr(exc))

    if rdc_path is None and python_module is None:
        print("[rdc_selfcheck] no usable RenderDoc CLI or Python replay found")
        return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
