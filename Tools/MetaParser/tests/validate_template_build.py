"""Verify incremental template preprocessing and build-time template diagnostics."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

from validate_parallel import write_text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--dotnet", type=Path, required=True)
    parser.add_argument("--templates", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    root = args.artifacts.resolve() / str(time.monotonic_ns())
    templates = root / "Templates"
    shutil.copytree(args.templates, templates)
    source = args.project.read_text(encoding="utf-8-sig")
    # The configured SDK project already contains absolute repository inputs.
    project = root / "MetaParser.csproj"
    source = source.replace(args.templates.resolve().as_posix(), templates.as_posix())
    source = source.replace(str(args.templates.resolve()), str(templates))
    write_text(project, source)
    environment = dict(os.environ, DOTNET_ROOT=str(args.dotnet.resolve().parent), DOTNET_MULTILEVEL_LOOKUP="0",
                       DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1", DOTNET_NOLOGO="1")

    def build(name):
        result = subprocess.run([str(args.dotnet.resolve()), "build", str(project), "--configuration", "Release",
                                 "--nologo", "/m:1", "/nr:false"], env=environment, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=180)
        write_text(root / f"{name}.log", result.stdout + result.stderr)
        return result

    initial = build("initial")
    if initial.returncode:
        raise RuntimeError(initial.stdout + initial.stderr)
    assert "Preprocessed 5" in initial.stdout
    generated = list((root / "obj/MetaTemplates").rglob("*.g.cs"))
    assert len(generated) == 5
    before = {path: path.stat().st_mtime_ns for path in generated}
    unchanged = build("unchanged")
    assert unchanged.returncode == 0, unchanged.stdout + unchanged.stderr
    assert "Preprocessed 5" not in unchanged.stdout
    assert before == {path: path.stat().st_mtime_ns for path in generated}
    template = templates / "MetaGenerated.h.tt"
    write_text(template, template.read_text(encoding="utf-8") + "\n// PRECOMPILED_TEMPLATE_MARKER\n")
    changed = build("changed")
    assert changed.returncode == 0, changed.stdout + changed.stderr
    assert "Preprocessed 5" in changed.stdout
    assert any("PRECOMPILED_TEMPLATE_MARKER" in path.read_text(encoding="utf-8") for path in generated)
    write_text(template, template.read_text(encoding="utf-8") + "\n<# THIS_IS_INVALID_CSHARP #>\n")
    invalid = build("invalid")
    assert invalid.returncode != 0
    assert "MetaGenerated.h.tt" in invalid.stdout + invalid.stderr
    write_text(args.artifacts / "test-results.json", json.dumps({"success": True, "checks": 4, "logs": str(root)}, indent=2))
    print("Template build checks passed: initial preprocessing, unchanged skip, edit rebuild, source diagnostics.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
