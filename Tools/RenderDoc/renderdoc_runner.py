#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EDITOR_PROJECT = REPO_ROOT / "TestProject" / "TestProject.nullus"
WINDOWS_DETACHED_FLAGS = 0
if os.name == "nt":
    WINDOWS_DETACHED_FLAGS = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Launch Nullus with RenderDoc capture settings.")
    parser.add_argument("--target", choices=("editor", "game"), default="editor")
    parser.add_argument("--backend", choices=("opengl", "vulkan", "dx12", "dx11"), default="vulkan")
    parser.add_argument("--launch-mode", choices=("auto", "direct", "renderdoccmd"), default="auto")
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--project", help="Path to a .nullus project file for Editor launches.")
    parser.add_argument("--exe", help="Explicit executable path. Bypasses auto-discovery.")
    parser.add_argument("--capture", action="store_true", help="Queue a startup capture.")
    parser.add_argument("--capture-after-frames", type=int, default=120)
    parser.add_argument("--open-capture-ui", action="store_true", help="Open qrenderdoc after capture.")
    parser.add_argument("--timeout", type=int, default=180, help="Seconds to wait for a capture file.")
    parser.add_argument("--no-wait", action="store_true", help="Do not wait for capture completion.")
    return parser.parse_args()


def iter_candidate_executables(target: str, config: str) -> Iterable[Path]:
    executable_name = "Editor.exe" if target == "editor" else "Game.exe"
    config_lower = config.lower()
    app_root = REPO_ROOT / "App"

    candidates = sorted(
        app_root.rglob(executable_name),
        key=lambda path: path.stat().st_mtime if path.exists() else 0,
        reverse=True,
    )

    def score(path: Path) -> tuple[int, float]:
        score_value = 0
        parent_name = path.parent.name.lower()
        if config_lower in parent_name:
            score_value += 2
        if "runtime" in parent_name:
            score_value += 1
        return score_value, path.stat().st_mtime

    return [path for path in sorted(candidates, key=score, reverse=True)]


def resolve_executable(target: str, config: str, explicit_path: str | None) -> Path:
    if explicit_path:
        path = Path(explicit_path).resolve()
        if not path.exists():
            raise FileNotFoundError(f"Executable not found: {path}")
        return path

    candidates = list(iter_candidate_executables(target, config))
    if not candidates:
        raise FileNotFoundError(f"Could not find {target} executable under {REPO_ROOT / 'App'}")
    return candidates[0]


def resolve_project(project_argument: str | None) -> Path:
    if project_argument:
        path = Path(project_argument).resolve()
    else:
        path = DEFAULT_EDITOR_PROJECT.resolve()

    if not path.exists():
        raise FileNotFoundError(f"Project file not found: {path}")
    return path


def resolve_renderdoc_ui() -> Path | None:
    renderdoc_path = os.environ.get("RENDERDOC_PATH")
    if renderdoc_path:
        configured = Path(os.path.expandvars(renderdoc_path)).expanduser()
        if configured.is_file() and configured.name.lower() == "qrenderdoc.exe":
            return configured
        if configured.is_dir():
            candidate = configured / "qrenderdoc.exe"
            if candidate.exists():
                return candidate

    for variable in ("ProgramFiles", "ProgramFiles(x86)"):
        root = os.environ.get(variable)
        if root:
            candidate = Path(root) / "RenderDoc" / "qrenderdoc.exe"
            if candidate.exists():
                return candidate

    discovered = shutil.which("qrenderdoc.exe")
    return Path(discovered) if discovered else None


def resolve_renderdoc_cmd() -> Path | None:
    renderdoc_path = os.environ.get("RENDERDOC_PATH")
    if renderdoc_path:
        configured = Path(os.path.expandvars(renderdoc_path)).expanduser()
        if configured.is_file() and configured.name.lower() == "renderdoccmd.exe":
            return configured
        if configured.is_dir():
            candidate = configured / "renderdoccmd.exe"
            if candidate.exists():
                return candidate

    for variable in ("ProgramFiles", "ProgramFiles(x86)"):
        root = os.environ.get(variable)
        if root:
            candidate = Path(root) / "RenderDoc" / "renderdoccmd.exe"
            if candidate.exists():
                return candidate

    discovered = shutil.which("renderdoccmd.exe")
    return Path(discovered) if discovered else None


def find_newest_capture(capture_dir: Path, launched_at: float) -> Path | None:
    search_roots = [capture_dir]
    temp_renderdoc_dir = Path(os.environ.get("TEMP", "")) / "RenderDoc" if os.environ.get("TEMP") else None
    if temp_renderdoc_dir is not None:
        search_roots.append(temp_renderdoc_dir)

    captures: list[Path] = []
    for root in search_roots:
        if root is None or not root.exists():
            continue
        captures.extend(path for path in root.rglob("*.rdc") if path.stat().st_mtime >= launched_at)

    if not captures:
        return None

    newest_capture = max(captures, key=lambda path: path.stat().st_mtime)
    if newest_capture.parent != capture_dir:
        capture_dir.mkdir(parents=True, exist_ok=True)
        copied_capture = capture_dir / newest_capture.name
        shutil.copy2(newest_capture, copied_capture)
        return copied_capture

    return newest_capture


def snapshot_capture_set(search_roots: Iterable[Path]) -> dict[Path, float]:
    captures: dict[Path, float] = {}
    for root in search_roots:
        if root is None or not root.exists():
            continue
        for path in root.rglob("*.rdc"):
            try:
                captures[path] = path.stat().st_mtime
            except FileNotFoundError:
                continue
    return captures


def discover_new_capture(
    capture_dir: Path,
    launched_at: float,
    baseline: dict[Path, float],
) -> Path | None:
    search_roots = [capture_dir]
    temp_renderdoc_dir = Path(os.environ.get("TEMP", "")) / "RenderDoc" if os.environ.get("TEMP") else None
    if temp_renderdoc_dir is not None:
        search_roots.append(temp_renderdoc_dir)

    current = snapshot_capture_set(search_roots)
    delta: list[Path] = []
    for path, mtime in current.items():
        baseline_mtime = baseline.get(path)
        if baseline_mtime is None or mtime > baseline_mtime:
            if mtime >= launched_at:
                delta.append(path)
    if not delta:
        return None

    newest_capture = max(delta, key=lambda path: path.stat().st_mtime)
    if newest_capture.parent != capture_dir:
        capture_dir.mkdir(parents=True, exist_ok=True)
        copied_capture = capture_dir / newest_capture.name
        shutil.copy2(newest_capture, copied_capture)
        return copied_capture

    return newest_capture


def open_capture_ui(qrenderdoc_path: Path, capture_path: Path) -> None:
    subprocess.Popen(
        [str(qrenderdoc_path), str(capture_path)],
        cwd=str(qrenderdoc_path.parent),
        env=os.environ.copy(),
        creationflags=WINDOWS_DETACHED_FLAGS,
        close_fds=True,
    )


def spawn_process(command: list[str], cwd: Path, env: dict[str, str]) -> subprocess.Popen[bytes]:
    return subprocess.Popen(
        command,
        cwd=str(cwd),
        env=env,
        creationflags=WINDOWS_DETACHED_FLAGS,
        close_fds=True,
    )


def main() -> int:
    args = parse_args()
    executable_path = resolve_executable(args.target, args.config, args.exe)
    qrenderdoc_path = resolve_renderdoc_ui()
    renderdoccmd_path = resolve_renderdoc_cmd()

    capture_dir = (REPO_ROOT / "Build" / "RenderDocCaptures" / args.target / args.backend).resolve()
    capture_dir.mkdir(parents=True, exist_ok=True)
    temp_renderdoc_dir = Path(os.environ.get("TEMP", "")) / "RenderDoc" if os.environ.get("TEMP") else None
    search_roots = [capture_dir]
    if temp_renderdoc_dir is not None:
        search_roots.append(temp_renderdoc_dir)

    launch_env = os.environ.copy()
    launch_env["NLS_RENDERDOC_ENABLE"] = "1"
    launch_env["NLS_RENDERDOC_CAPTURE_DIR"] = str(capture_dir)
    launch_env["NLS_RENDERDOC_CAPTURE_LABEL"] = f"{args.target}_{args.backend}"
    launch_env["NLS_RENDERDOC_AUTO_OPEN"] = "1" if args.open_capture_ui else "0"

    command = [str(executable_path), "--backend", args.backend]
    if args.target == "editor":
        command.append(str(resolve_project(args.project)))

    if args.capture:
        launch_env["NLS_RENDERDOC_CAPTURE"] = "1"
        launch_env["NLS_RENDERDOC_CAPTURE_AFTER_FRAMES"] = str(max(0, args.capture_after_frames))

    launched_at = time.time()
    baseline_captures = snapshot_capture_set(search_roots)
    # Force direct launch on all backends.
    # renderdoccmd injection may retain RenderDoc default hotkeys (F12) before app-level override.
    # We explicitly disable this path so capture hotkey control stays in Nullus (F11 only).
    if args.launch_mode == "renderdoccmd":
        print("[renderdoc_runner] launch_mode=renderdoccmd requested, but renderdoccmd path is disabled to avoid F12 capture path; using direct")
    use_renderdoccmd = False

    if use_renderdoccmd:
        capture_template = str((capture_dir / f"{args.target}_{args.backend}").resolve())
        renderdoc_command = [
            str(renderdoccmd_path),
            "capture",
            "-d",
            str(executable_path.parent),
            "-c",
            capture_template,
            *command,
        ]
        process = spawn_process(renderdoc_command, executable_path.parent, launch_env)
    else:
        process = spawn_process(command, executable_path.parent, launch_env)

    print(f"[renderdoc_runner] launched {args.target} pid={process.pid}")
    print(f"[renderdoc_runner] executable={executable_path}")
    print(f"[renderdoc_runner] backend={args.backend}")
    print(f"[renderdoc_runner] capture_dir={capture_dir}")
    if use_renderdoccmd and renderdoccmd_path is not None:
        print(f"[renderdoc_runner] launch_mode=renderdoccmd")
        print(f"[renderdoc_runner] renderdoccmd={renderdoccmd_path}")
    elif renderdoccmd_path is not None:
        print("[renderdoc_runner] launch_mode=direct")
        print(f"[renderdoc_runner] renderdoccmd_available={renderdoccmd_path}")
    else:
        print("[renderdoc_runner] launch_mode=direct")
        print("[renderdoc_runner] renderdoccmd not found, using direct launch")

    if not args.capture or args.no_wait:
        return 0

    deadline = launched_at + max(1, args.timeout)
    capture_path: Path | None = None
    while time.time() < deadline:
        capture_path = discover_new_capture(capture_dir, launched_at, baseline_captures)
        if capture_path is None:
            capture_path = find_newest_capture(capture_dir, launched_at)
        if capture_path is not None:
            break
        time.sleep(1.0)

    if capture_path is None:
        print("[renderdoc_runner] capture not found before timeout", file=sys.stderr)
        return 2

    print(f"[renderdoc_runner] latest_capture={capture_path}")
    if args.open_capture_ui and qrenderdoc_path is not None:
        open_capture_ui(qrenderdoc_path, capture_path)
        print(f"[renderdoc_runner] opened qrenderdoc={qrenderdoc_path}")
    elif args.open_capture_ui:
        print("[renderdoc_runner] qrenderdoc.exe not found; relying on shell association", file=sys.stderr)

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FileNotFoundError as error:
        print(f"[renderdoc_runner] {error}", file=sys.stderr)
        raise SystemExit(1)
