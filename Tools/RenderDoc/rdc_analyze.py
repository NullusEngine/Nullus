#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import traceback
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RDC_SCRIPTS = (
    Path.home() / "AppData" / "Roaming" / "Python" / "Python311" / "Scripts" / "rdc.exe",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze a RenderDoc capture for Nullus using rdc-cli."
    )
    parser.add_argument("capture", help="Path to the .rdc capture file.")
    parser.add_argument(
        "--focus-eid",
        type=int,
        help="Explicit event ID to inspect. Defaults to the first sampled draw if any.",
    )
    parser.add_argument("--draw-limit", type=int, default=20, help="Number of draws to sample.")
    parser.add_argument(
        "--event-limit", type=int, default=50, help="Number of events to sample for overview."
    )
    parser.add_argument(
        "--session",
        default=f"nullus-rdc-{os.getpid()}",
        help="rdc session name. Use a custom value to avoid colliding with another open session.",
    )
    parser.add_argument(
        "--skip-doctor",
        action="store_true",
        help="Skip `rdc doctor` before analysis.",
    )
    parser.add_argument(
        "--json-out",
        help="Optional path for the full structured JSON output.",
    )
    return parser.parse_args()


def resolve_rdc() -> Path:
    discovered = shutil.which("rdc")
    if discovered:
        return Path(discovered)

    for candidate in DEFAULT_RDC_SCRIPTS:
        if candidate.exists():
            return candidate

    raise FileNotFoundError(
        "rdc executable not found. Install rdc-cli and ensure it is on PATH or in the user Python scripts directory."
    )


def default_renderdoc_python_path() -> Path | None:
    configured = os.environ.get("RENDERDOC_PYTHON_PATH")
    if configured:
        path = Path(configured).expanduser()
        if path.exists():
            return path

    candidate = Path.home() / "AppData" / "Local" / "rdc" / "renderdoc"
    if candidate.exists():
        return candidate
    return None


def build_env() -> dict[str, str]:
    env = os.environ.copy()
    renderdoc_python_path = default_renderdoc_python_path()
    if renderdoc_python_path is not None:
        env.setdefault("RENDERDOC_PYTHON_PATH", str(renderdoc_python_path))
    return env


def run_command(
    rdc_path: Path,
    args: list[str],
    env: dict[str, str],
    expect_json: bool = False,
) -> Any:
    command = [str(rdc_path), *args]
    result = subprocess.run(
        command,
        cwd=str(REPO_ROOT),
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )

    if result.returncode != 0:
        raise RuntimeError(
            f"rdc command failed: {' '.join(args)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )

    output = result.stdout.strip()
    if not expect_json:
        return output

    if not output:
        raise RuntimeError(f"rdc command produced no JSON output: {' '.join(args)}")
    return json.loads(output)


def run_doctor(rdc_path: Path, env: dict[str, str], session: str) -> str:
    _ = session
    return run_command(rdc_path, ["doctor"], env, expect_json=False)


def run_session_command(
    rdc_path: Path,
    session: str,
    args: list[str],
    env: dict[str, str],
    expect_json: bool,
) -> Any:
    return run_command(
        rdc_path,
        ["--session", session, *args],
        env,
        expect_json=expect_json,
    )


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


def analyze_with_python_replay(
    capture_path: Path,
    focus_eid: int | None,
    draw_limit: int,
    event_limit: int,
) -> dict[str, Any]:
    renderdoc_path = resolve_renderdoc_python_module()
    if renderdoc_path is None:
        raise RuntimeError("RenderDoc Python module not found; set RENDERDOC_PYTHON_PATH to enable fallback replay.")

    sys.path.insert(0, str(renderdoc_path))
    import renderdoc as rd  # type: ignore

    cap = rd.OpenCaptureFile()
    ret = cap.OpenFile(str(capture_path), "", None)
    if ret != rd.ResultCode.Succeeded:
        cap.Shutdown()
        raise RuntimeError(f"RenderDoc OpenFile failed: {ret}")

    replay_status = cap.LocalReplaySupport()
    if replay_status != rd.ResultCode.Succeeded:
        analysis = analyze_with_python_metadata(
            capture_path=capture_path,
            replay_status=replay_status,
            renderdoc_module=rd,
            capture_file=cap,
        )
        cap.Shutdown()
        return analysis

    status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded or controller is None:
        analysis = analyze_with_python_metadata(
            capture_path=capture_path,
            replay_status=status,
            renderdoc_module=rd,
            capture_file=cap,
        )
        cap.Shutdown()
        return analysis

    def flatten_draws(draws: list[Any], out: list[Any]) -> None:
        for d in draws:
            out.append(d)
            if getattr(d, "children", None):
                flatten_draws(list(d.children), out)

    drawcalls = list(controller.GetDrawcalls())
    flat_draws: list[Any] = []
    flatten_draws(drawcalls, flat_draws)

    def build_row(draw: Any) -> dict[str, Any]:
        return {
            "eid": int(getattr(draw, "eventId", 0)),
            "type": str(getattr(draw, "name", "")),
            "triangles": int(getattr(draw, "numIndices", 0)),
            "pass": str(getattr(draw, "parent", "") or ""),
            "marker": str(getattr(draw, "name", "")),
        }

    draws_payload = [build_row(draw) for draw in flat_draws[: max(1, draw_limit)]]
    events_payload = [{"eid": int(getattr(draw, "eventId", 0)), "name": str(getattr(draw, "name", ""))} for draw in flat_draws[: max(1, event_limit)]]

    if focus_eid is None and draws_payload:
        focus_eid = int(draws_payload[0]["eid"])

    pipeline_payload: dict[str, Any] | None = None
    bindings_payload: list[dict[str, Any]] = []
    shaders_payload: dict[str, Any] = {}
    if focus_eid is not None:
        try:
            pipe = controller.GetPipelineState()
            topology = pipe.GetPrimitiveTopology()
            pipeline_payload = {
                "api": str(controller.GetAPIProperties().pipelineType),
                "topology": str(topology),
            }
        except Exception:
            pipeline_payload = {}

        try:
            texs = controller.GetPipelineState().GetReadOnlyResources(rd.ShaderStage.Pixel)
            bindings_payload = [
                {"stage": "ps", "name": getattr(tex, "name", ""), "resourceId": int(getattr(tex, "resourceId", 0))}
                for tex in texs
            ]
        except Exception:
            bindings_payload = []

        try:
            vs = controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Vertex)
            ps = controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Pixel)
            if vs:
                shaders_payload["vs"] = {"shader": int(getattr(vs, "resourceId", 0))}
            if ps:
                shaders_payload["ps"] = {"shader": int(getattr(ps, "resourceId", 0))}
        except Exception:
            shaders_payload = {}

    info = {
        "API": str(controller.GetAPIProperties().pipelineType),
        "Events": len(flat_draws),
        "Draw Calls": len(flat_draws),
        "Clears": "n/a",
        "Copies": "n/a",
        "Capture": str(capture_path),
    }
    stats = {}
    controller.Shutdown()
    cap.Shutdown()

    return build_analysis(
        capture_path=capture_path,
        doctor_output=None,
        info=info,
        stats=stats,
        passes=[],
        draws=draws_payload,
        events=events_payload,
        focus_eid=focus_eid,
        pipeline=pipeline_payload,
        bindings=bindings_payload,
        shaders=shaders_payload,
    )


def analyze_with_python_metadata(
    capture_path: Path,
    replay_status: Any,
    renderdoc_module: Any,
    capture_file: Any,
) -> dict[str, Any]:
    thumb_path: str | None = None
    try:
        analysis_dir = REPO_ROOT / "Build" / "RenderDocAnalysis"
        analysis_dir.mkdir(parents=True, exist_ok=True)
        thumb = capture_file.GetThumbnail(renderdoc_module.FileType.PNG, 512)
        if thumb and getattr(thumb, "data", None):
            thumb_name = capture_path.stem + "_thumb.png"
            thumb_path = str((analysis_dir / thumb_name).resolve())
            with open(thumb_path, "wb") as handle:
                handle.write(thumb.data)
    except Exception:
        thumb_path = None

    section_count = 0
    section_names: list[str] = []
    try:
        section_count = capture_file.GetSectionCount()
        for idx in range(section_count):
            props = capture_file.GetSectionProperties(idx)
            name = getattr(props, "name", "")
            if name:
                section_names.append(name)
    except Exception:
        section_names = []

    info = {
        "API": "Unknown",
        "Events": 0,
        "Draw Calls": 0,
        "Clears": "n/a",
        "Copies": "n/a",
        "Capture": str(capture_path),
    }
    stats = {
        "replay_status": str(replay_status),
        "sections": section_count,
        "section_names": section_names[:8],
        "thumbnail": thumb_path or "unavailable",
    }
    return build_analysis(
        capture_path=capture_path,
        doctor_output=None,
        info=info,
        stats=stats,
        passes=[],
        draws=[],
        events=[],
        focus_eid=None,
        pipeline=None,
        bindings=[],
        shaders={},
    )


def choose_focus_eid(explicit: int | None, draws: list[dict[str, Any]]) -> int | None:
    if explicit is not None:
        return explicit
    if not draws:
        return None
    return int(draws[0]["eid"])


def load_shader_state(
    rdc_path: Path, session: str, focus_eid: int | None, env: dict[str, str]
) -> dict[str, Any]:
    if focus_eid is None:
        return {}

    shaders: dict[str, Any] = {}
    for stage in ("vs", "ps", "cs"):
        try:
            payload = run_session_command(
                rdc_path,
                session,
                ["shader", str(focus_eid), stage, "--json"],
                env,
                expect_json=True,
            )
        except RuntimeError:
            continue

        row = payload.get("row")
        if row is None:
            continue
        shader_id = row.get("shader", 0)
        if shader_id in (0, "0", None):
            continue
        shaders[stage] = row

    return shaders


def summarize_bindings(bindings: list[dict[str, Any]]) -> dict[str, Any]:
    by_stage: dict[str, int] = {}
    names: list[str] = []
    for binding in bindings:
        stage = str(binding.get("stage", "?"))
        by_stage[stage] = by_stage.get(stage, 0) + 1
        name = str(binding.get("name", ""))
        if name and name not in names:
            names.append(name)
    return {
        "total": len(bindings),
        "by_stage": by_stage,
        "sample_names": names[:8],
    }


def build_analysis(
    capture_path: Path,
    doctor_output: str | None,
    info: dict[str, Any],
    stats: dict[str, Any],
    passes: list[dict[str, Any]],
    draws: list[dict[str, Any]],
    events: list[dict[str, Any]],
    focus_eid: int | None,
    pipeline: dict[str, Any] | None,
    bindings: list[dict[str, Any]],
    shaders: dict[str, Any],
) -> dict[str, Any]:
    return {
        "capture_path": str(capture_path),
        "doctor_ran": doctor_output is not None,
        "doctor_output": doctor_output,
        "overall": {
            "api": info.get("API"),
            "capture": info.get("Capture"),
            "events": info.get("Events"),
            "draw_calls": info.get("Draw Calls"),
            "clears": info.get("Clears"),
            "copies": info.get("Copies"),
            "stats": stats,
        },
        "passes": passes,
        "draws": draws,
        "events": events,
        "focus": {
            "eid": focus_eid,
            "pipeline": pipeline,
            "bindings": bindings,
            "binding_summary": summarize_bindings(bindings),
            "shaders": shaders,
        },
    }


def format_markdown(analysis: dict[str, Any]) -> str:
    overall = analysis["overall"]
    passes = analysis["passes"]
    draws = analysis["draws"]
    focus = analysis["focus"]
    lines: list[str] = []

    lines.append("## Overall Frame Summary")
    lines.append(
        f"- Capture: `{analysis['capture_path']}`"
    )
    lines.append(
        f"- API: `{overall.get('api', 'Unknown')}` | Events: `{overall.get('events', 'n/a')}` | Draws: `{overall.get('draw_calls', 'n/a')}`"
    )
    lines.append(
        f"- Clears: `{overall.get('clears', 'n/a')}` | Copies: `{overall.get('copies', 'n/a')}`"
    )
    if overall.get("stats"):
        stats = overall.get("stats", {})
        if isinstance(stats, dict) and stats:
            thumb = stats.get("thumbnail")
            replay_status = stats.get("replay_status")
            sections = stats.get("sections")
            section_names = stats.get("section_names")
            if replay_status:
                lines.append(f"- Replay status: `{replay_status}`")
            if sections is not None:
                lines.append(f"- Sections: `{sections}`")
            if section_names:
                lines.append(f"- Section names: `{', '.join(section_names)}`")
            if thumb:
                lines.append(f"- Thumbnail: `{thumb}`")

    lines.append("")
    lines.append("## Pass Summary")
    if not passes:
        lines.append("- No explicit passes were reported by `rdc passes`.")
    else:
        for render_pass in passes[:8]:
            lines.append(
                "- "
                f"`{render_pass.get('name', '?')}` "
                f"(EID `{render_pass.get('begin_eid', '?')}` -> `{render_pass.get('end_eid', '?')}`, "
                f"draws `{render_pass.get('draws', 0)}`, dispatches `{render_pass.get('dispatches', 0)}`, "
                f"triangles `{render_pass.get('triangles', 0)}`)"
            )

    lines.append("")
    lines.append("## Draw Sample")
    if not draws:
        lines.append("- No draws were reported in the sampled range.")
    else:
        for draw in draws[:10]:
            lines.append(
                "- "
                f"EID `{draw.get('eid', '?')}` | type `{draw.get('type', '?')}` | "
                f"triangles `{draw.get('triangles', 0)}` | pass `{draw.get('pass', '-')}` | marker `{draw.get('marker', '-')}`"
            )

    lines.append("")
    lines.append("## Focus Event")
    if focus.get("eid") is None:
        lines.append("- No focus EID was chosen because no draw was available.")
    else:
        lines.append(f"- Focus EID: `{focus['eid']}`")
        pipeline = focus.get("pipeline") or {}
        if pipeline:
            lines.append(
                f"- Topology: `{pipeline.get('topology', 'Unknown')}` | API: `{pipeline.get('api', 'Unknown')}`"
            )
        binding_summary = focus.get("binding_summary") or {}
        lines.append(
            f"- Binding count: `{binding_summary.get('total', 0)}` | By stage: `{binding_summary.get('by_stage', {})}`"
        )
        sample_names = binding_summary.get("sample_names", [])
        if sample_names:
            lines.append(f"- Sample bindings: `{', '.join(sample_names)}`")
        shaders = focus.get("shaders") or {}
        if shaders:
            shader_bits = []
            for stage, row in shaders.items():
                shader_bits.append(f"{stage}={row.get('shader', 0)}")
            lines.append(f"- Bound shaders: `{', '.join(shader_bits)}`")

    lines.append("")
    lines.append("## Next Questions")
    lines.append("- Which pass or event first diverges from the expected rendering result?")
    lines.append("- Does the focus event target the expected render target or editor panel texture?")
    lines.append("- Do pipeline state, bindings, and shader selection match the expected backend path?")

    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    capture_path = Path(args.capture).resolve()
    if not capture_path.exists():
        raise FileNotFoundError(f"Capture file not found: {capture_path}")

    rdc_path = resolve_rdc()
    env = build_env()

    doctor_output: str | None = None
    if not args.skip_doctor:
        doctor_output = run_doctor(rdc_path, env, args.session)

    try:
        run_session_command(
            rdc_path, args.session, ["open", str(capture_path)], env, expect_json=False
        )

        info = run_session_command(
            rdc_path, args.session, ["info", "--json"], env, expect_json=True
        )
        stats = run_session_command(
            rdc_path, args.session, ["stats", "--json"], env, expect_json=True
        )
        passes_payload = run_session_command(
            rdc_path, args.session, ["passes", "--json"], env, expect_json=True
        )
        draws_payload = run_session_command(
            rdc_path,
            args.session,
            ["draws", "--limit", str(max(1, args.draw_limit)), "--json"],
            env,
            expect_json=True,
        )
        events_payload = run_session_command(
            rdc_path,
            args.session,
            ["events", "--limit", str(max(1, args.event_limit)), "--json"],
            env,
            expect_json=True,
        )

        passes = passes_payload.get("passes", [])
        draws = draws_payload if isinstance(draws_payload, list) else draws_payload.get("rows", [])
        events = events_payload if isinstance(events_payload, list) else events_payload.get("rows", [])

        focus_eid = choose_focus_eid(args.focus_eid, draws)

        pipeline: dict[str, Any] | None = None
        bindings: list[dict[str, Any]] = []
        shaders: dict[str, Any] = {}
        if focus_eid is not None:
            pipeline = run_session_command(
                rdc_path,
                args.session,
                ["pipeline", str(focus_eid), "--json"],
                env,
                expect_json=True,
            )
            bindings_payload = run_session_command(
                rdc_path,
                args.session,
                ["bindings", str(focus_eid), "--json"],
                env,
                expect_json=True,
            )
            bindings = bindings_payload if isinstance(bindings_payload, list) else bindings_payload.get("rows", [])
            shaders = load_shader_state(rdc_path, args.session, focus_eid, env)

        analysis = build_analysis(
            capture_path=capture_path,
            doctor_output=doctor_output,
            info=info,
            stats=stats,
            passes=passes,
            draws=draws,
            events=events,
            focus_eid=focus_eid,
            pipeline=pipeline,
            bindings=bindings,
            shaders=shaders,
        )

        if args.json_out:
            output_path = Path(args.json_out).resolve()
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_text(json.dumps(analysis, indent=2), encoding="utf-8")

        print(format_markdown(analysis))
        return 0
    except Exception as error:
        sys.stdout.flush()
        print(f"[rdc_analyze] rdc-cli path failed: {error}", file=sys.stderr)
        print("[rdc_analyze] falling back to RenderDoc Python replay", file=sys.stderr)
        if os.environ.get("RDC_DEBUG", ""):
            print(traceback.format_exc(), file=sys.stderr)
        analysis = analyze_with_python_replay(
            capture_path=capture_path,
            focus_eid=args.focus_eid,
            draw_limit=args.draw_limit,
            event_limit=args.event_limit,
        )
        if args.json_out:
            output_path = Path(args.json_out).resolve()
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_text(json.dumps(analysis, indent=2), encoding="utf-8")
        print(format_markdown(analysis))
        return 0
    finally:
        try:
            run_session_command(rdc_path, args.session, ["close"], env, expect_json=False)
        except RuntimeError:
            pass


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FileNotFoundError as error:
        print(f"[rdc_analyze] {error}", file=sys.stderr)
        raise SystemExit(1)
    except RuntimeError as error:
        print(f"[rdc_analyze] {error}", file=sys.stderr)
        raise SystemExit(2)
