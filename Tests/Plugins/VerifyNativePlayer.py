"""Publish and run the actual Game executable outside its development tree."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

tool, project, game, manifest, output = map(lambda value: Path(value).resolve(), sys.argv[1:6])
if not (project / ".nullus-plugin-acceptance").exists():
    raise SystemExit("Use a dedicated PrepareAcceptance.py fixture; no user project will be moved")
if output.exists() and any(output.iterdir()):
    raise SystemExit("Player validation requires a fresh output directory")
result = subprocess.run([str(tool), str(project), "stage-player", str(output)], capture_output=True, text=True)
if result.returncode:
    raise SystemExit(result.stdout + result.stderr)
files = [line.strip() for line in manifest.read_text().splitlines() if line.strip()]
for filename in files:
    if any(word in filename.lower() for word in ("nls_packages", "nullusplugins", "nls_build", "nls_cook", "editor")):
        raise SystemExit("Game binary closure contains tool/editor code: " + filename)
    shutil.copy2(game.parent / filename, output / filename)
shutil.copy2(game, output / "Game.exe")
offline = project.with_name(project.name + ".offline-" + uuid.uuid4().hex)
project.rename(offline)
try:
    # No developer PATH, project working directory, SDK lookup or PackageCache.
    windows = os.environ["SystemRoot"]
    environment = dict(SystemRoot=windows, PATH=str(Path(windows) / "System32"), TEMP=str(output), TMP=str(output))
    run = subprocess.run([str(output / "Game.exe"), "--plugin-self-test"], cwd=output, env=environment,
                         capture_output=True, text=True, timeout=30)
    report = dict(executable=str(output / "Game.exe"), gameSha256=hashlib.sha256((output / "Game.exe").read_bytes()).hexdigest(),
                  exitCode=run.returncode, stdout=run.stdout, stderr=run.stderr, sourceProjectUnavailable=True,
                  dependencyFiles=files, plugins=json.loads((output / "Plugins/runtime-plugins.json").read_text()))
    (output / "plugin-validation.json").write_text(json.dumps(report, indent=2))
    print(run.stdout, run.stderr)
    if run.returncode or "[plugin-self-test] OK" not in run.stdout:
        raise SystemExit("Real deployed Game plugin validation failed")
finally:
    offline.rename(project)
