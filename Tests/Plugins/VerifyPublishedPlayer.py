"""Run the normal published Game loop with its source fixture unavailable.

Uses the existing bounded renderer validation launch mode solely to exit after
frames. A scene without a camera can exercise plugins but cannot pass GPU image
validation; the JSON records that distinction explicitly.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

parser = argparse.ArgumentParser()
parser.add_argument("player", type=Path)
parser.add_argument("project", type=Path)
parser.add_argument("--scripts", action="store_true")
parser.add_argument("--shipping", action="store_true")
args = parser.parse_args()
player, project = args.player.resolve(), args.project.resolve()
if not (project / ".nullus-plugin-acceptance").is_file():
    raise SystemExit("Only dedicated acceptance fixtures may be temporarily moved")
if not (player / "Player.info").is_file() or not (player / "Game.exe").is_file():
    raise SystemExit("Use a complete real BuildPipeline output")
descriptor = json.loads((player / "Player.info").read_text(encoding="utf-8"))
if args.shipping and descriptor.get("buildMode") != "Shipping":
    raise SystemExit("Shipping verification requires a Shipping Player descriptor")
forbidden = ("nls_packages", "nls_build", "nls_cook", "nullusplugins", "nullusbuild",
             "nullus.editor", "gamescriptseditor", "nullus.scriptgenerator", "nullus.scriptanalysis")
leaks = [str(path.relative_to(player)) for path in player.rglob("*")
         if path.is_file() and (any(word in path.name.lower() for word in forbidden)
                               or "editor" in [part.lower() for part in path.relative_to(player).parts[:-1]])]
if leaks:
    raise SystemExit("Published Player contains editor/tool files: " + repr(leaks))
image = player / "plugin-gameplay-frame.png"
if image.exists():
    raise SystemExit("Use fresh output; an old frame must not count as validation")
offline = project.with_name(project.name + ".offline-" + uuid.uuid4().hex)
project.rename(offline)
try:
    windows = Path(os.environ["SystemRoot"])
    env = dict(SystemRoot=str(windows), PATH=str(windows / "System32"), TEMP=str(player), TMP=str(player))
    run = subprocess.run([str(player / "Game.exe"), "--material-validation-output", str(image),
                          "--material-validation-after-frames", "8"],
                         cwd=player, env=env, capture_output=True, text=True, errors="replace", timeout=90)
    output = run.stdout + "\n" + run.stderr
    expected = ["RuntimeSample runtime session begin", "RuntimeSample received third runtime session tick",
                "RuntimeSample runtime session end"]
    if args.scripts:
        for language in ("Lua", "C#"):
            expected.extend([language + " runtime session begin", language + " received third runtime session tick",
                             language + " runtime session end"])
    checks = {message: message in output for message in expected}
    report = dict(executable=str(player / "Game.exe"), exitCode=run.returncode,
                  gameSha256=hashlib.sha256((player / "Game.exe").read_bytes()).hexdigest(),
                  sourceProjectUnavailable=True, developerPathRemoved=True, shipping=args.shipping,
                  lifecycleChecks=checks, frameCaptured=image.is_file(), forbiddenFiles=leaks)
    (player / "plugin-gameplay.log").write_text(output, encoding="utf-8")
    (player / "plugin-gameplay.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    if run.returncode or not all(checks.values()):
        raise SystemExit("Published Game plugin lifecycle verification failed; see plugin-gameplay.log")
finally:
    offline.rename(project)
