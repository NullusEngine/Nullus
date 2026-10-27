#!/usr/bin/env python3
"""Validate the icon manifest, generated PNGs and generated C++ headers."""

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ICON_ROOT = ROOT / "Tools" / "Icons"
MANIFEST = ICON_ROOT / "icons.manifest.json"
SOURCE_ROOT = ICON_ROOT / "Source"
ASSET_ROOT = ROOT / "App" / "Assets" / "Editor" / "Icons"
GENERATED = ROOT / "Project" / "Editor" / "Icons" / "Generated"


def fail(message: str) -> None:
    print(f"validate_icons: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_png_size(path: Path) -> tuple[int, int]:
    header = path.read_bytes()[:24]
    if header[:8] != b"\x89PNG\r\n\x1a\n":
        fail(f"{path} is not a PNG")
    width, height = struct.unpack(">II", header[16:24])
    return width, height


def icon_stem(icon_id: str) -> str:
    return icon_id[len("editor.icon."):].replace(".", "-")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--baseline-ref",
        default=None,
        help="Git ref (e.g. origin/main) whose manifest ids must still exist or be aliased")
    args = parser.parse_args()

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if manifest.get("schemaVersion") != 2:
        fail("manifest schemaVersion must be 2")

    ids: set[str] = set()
    names: set[str] = set()
    expected_pngs: set[str] = set()
    for icon in manifest.get("icons", []):
        icon_id = icon.get("id", "")
        if not icon_id.startswith("editor.icon."):
            fail(f"invalid id {icon_id!r}")
        if icon_id in ids:
            fail(f"duplicate id {icon_id}")
        ids.add(icon_id)
        names.add(icon_id)
        for alias in icon.get("aliases", []):
            if alias in names:
                fail(f"alias {alias!r} clashes with an existing id")
            names.add(alias)
        if not icon.get("license"):
            fail(f"missing license for {icon_id}")
        if icon.get("kind") not in {"monochrome", "color", "glyph", "generated", "raster"}:
            fail(f"invalid kind for {icon_id}")
        if icon.get("deprecatedBy") and icon["deprecatedBy"] not in {i.get("id") for i in manifest["icons"]}:
            fail(f"deprecatedBy target not found for {icon_id}: {icon['deprecatedBy']}")

        if icon["kind"] in {"monochrome", "color"}:
            source = SOURCE_ROOT / icon.get("source", "")
            if not source.is_file():
                fail(f"missing source for {icon_id}: {source}")
            sizes = icon.get("sizes") or []
            if not sizes:
                fail(f"missing sizes for {icon_id}")
            for size in sizes:
                name = f"{icon_stem(icon_id)}@{size}.png"
                path = ASSET_ROOT / name
                if not path.is_file():
                    fail(f"missing generated PNG {path}")
                width, height = read_png_size(path)
                if width != size or height != size:
                    fail(f"{path} is {width}x{height}, expected {size}x{size}")
                expected_pngs.add(name)

        if icon["kind"] == "glyph":
            try:
                codepoint = int(str(icon.get("codepoint", "0")), 16)
            except ValueError:
                fail(f"invalid codepoint for {icon_id}")
            if codepoint == 0:
                fail(f"missing codepoint for {icon_id}")

    actual_pngs = {path.name for path in ASSET_ROOT.glob("*.png")}
    atlas = manifest.get("atlas", {})
    if atlas.get("enabled", False):
        page_size = int(atlas.get("pageSize", 512))
        atlas_pages = sorted(ASSET_ROOT.glob("editor-icons-atlas-*.png"))
        if not atlas_pages:
            fail("atlas is enabled but no atlas pages were generated")
        for page in atlas_pages:
            width, height = read_png_size(page)
            if width != page_size or height != page_size:
                fail(f"{page} is {width}x{height}, expected {page_size}x{page_size}")
            expected_pngs.add(page.name)
        if not (GENERATED / "IconAtlas.generated.cpp").is_file():
            fail("missing IconAtlas.generated.cpp")

    orphans = sorted(actual_pngs - expected_pngs)
    if orphans:
        fail("orphan PNGs without manifest entry: " + ", ".join(orphans))

    ids_header = (GENERATED / "IconIds.generated.h").read_text(encoding="utf-8")
    for icon_id in ids:
        if f'"{icon_id}"' not in ids_header:
            fail(f"{icon_id} missing from IconIds.generated.h")

    catalog_source = (GENERATED / "IconCatalog.generated.cpp").read_text(encoding="utf-8")
    for icon_id in ids:
        if f'"{icon_id}"' not in catalog_source:
            fail(f"{icon_id} missing from IconCatalog.generated.cpp")

    if args.baseline_ref:
        try:
            subprocess.check_output(
                ["git", "-C", str(ROOT), "rev-parse", "--verify", args.baseline_ref],
                text=True,
                stderr=subprocess.DEVNULL)
        except Exception:
            fail(f"unknown baseline ref {args.baseline_ref}")

        try:
            baseline_text = subprocess.check_output(
                ["git", "-C", str(ROOT), "show",
                 f"{args.baseline_ref}:Tools/Icons/icons.manifest.json"],
                text=True,
                stderr=subprocess.DEVNULL)
            baseline = json.loads(baseline_text)
        except Exception:
            # The manifest may not exist on the baseline yet (bootstrap commit).
            print(f"validate_icons: baseline {args.baseline_ref} has no manifest; skipping id protection")
            baseline = None

        if baseline is not None:
            missing = sorted(
                icon["id"] for icon in baseline.get("icons", [])
                if icon["id"] not in names)
            if missing:
                fail("published icon ids were removed without an alias: " + ", ".join(missing))

    print(
        f"validate_icons: ok ({len(ids)} icons, {len(expected_pngs)} PNGs, "
        f"{len(manifest.get('icons', []))} manifest entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
