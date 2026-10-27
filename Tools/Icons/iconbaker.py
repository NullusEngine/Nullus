#!/usr/bin/env python3
"""IconBaker: manifest -> multi-size PNGs + generated C++.

The repository intentionally keeps this tool dependency-free (Python standard
library only) so CI and every workstation produce byte-identical output. The
built-in rasterizer supports the flat SVG subset used by editor icons:
rect(+rx), circle, ellipse(+rotate), polygon, and path M/L/C/Z with multiple
subpaths. Strokes, text and gradients are rejected with a diagnostic; use fills.

Usage:
  python Tools/Icons/iconbaker.py            # build assets + generated code
  python Tools/Icons/iconbaker.py --check    # rebuild into a temp dir and compare
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import shutil
import struct
import sys
import tempfile
import zlib
from pathlib import Path

SS = 8  # supersampling factor (fixed; part of the deterministic toolchain)
ROOT = Path(__file__).resolve().parents[2]
ICON_ROOT = ROOT / "Tools" / "Icons"
MANIFEST = ICON_ROOT / "icons.manifest.json"
SOURCE_ROOT = ICON_ROOT / "Source"
ASSET_OUT = ROOT / "App" / "Assets" / "Editor" / "Icons"
GENERATED_OUT = ROOT / "Project" / "Editor" / "Icons" / "Generated"

ATTR_RE = re.compile(r'([\w:-]+)="([^"]*)"')
ELEM_RE = re.compile(r'<(rect|circle|ellipse|polygon|path)\b([^>]*?)/?>', re.S)
NUM = r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?'


class BakeError(RuntimeError):
    pass


def parse_color(text: str) -> tuple[int, int, int]:
    m = re.fullmatch(r"#([0-9a-fA-F]{6})", text.strip())
    if not m:
        raise BakeError(f"unsupported fill {text!r} (only #RRGGBB is supported)")
    value = int(m.group(1), 16)
    return (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF


def arc_points(cx, cy, rx, ry, a0, a1, steps):
    return [
        (cx + rx * math.cos(a0 + (a1 - a0) * i / steps),
         cy + ry * math.sin(a0 + (a1 - a0) * i / steps))
        for i in range(steps + 1)
    ]


def rect_polygon(x, y, w, h, rx):
    if rx <= 0:
        return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
    rx = min(rx, w / 2.0, h / 2.0)
    points = []
    points += arc_points(x + w - rx, y + rx, rx, rx, -math.pi / 2, 0, 24)
    points += arc_points(x + w - rx, y + h - rx, rx, rx, 0, math.pi / 2, 24)
    points += arc_points(x + rx, y + h - rx, rx, rx, math.pi / 2, math.pi, 24)
    points += arc_points(x + rx, y + rx, rx, rx, math.pi, math.pi * 1.5, 24)
    return points


def ellipse_loop(cx, cy, rx, ry, rotate_deg=0.0, steps=256):
    points = [
        (cx + rx * math.cos(2 * math.pi * i / steps),
         cy + ry * math.sin(2 * math.pi * i / steps))
        for i in range(steps)
    ]
    if rotate_deg:
        angle = math.radians(rotate_deg)
        ca, sa = math.cos(angle), math.sin(angle)
        points = [
            ((px - cx) * ca - (py - cy) * sa + cx,
             (px - cx) * sa + (py - cy) * ca + cy)
            for px, py in points
        ]
    return points


def path_loops(d: str, steps=64):
    tokens = re.findall(r"[MCLZHVmclzhv]|" + NUM, d)
    loops = []
    current = (0.0, 0.0)
    start = (0.0, 0.0)
    loop = []
    index = 0

    def number():
        nonlocal index
        value = float(tokens[index])
        index += 1
        return value

    while index < len(tokens):
        command = tokens[index]
        index += 1
        if command in ("M", "m"):
            x, y = number(), number()
            if command == "m":
                x, y = current[0] + x, current[1] + y
            if loop:
                loops.append(loop)
                loop = []
            current = (x, y)
            start = current
            loop.append(current)
        elif command in ("L", "l"):
            x, y = number(), number()
            if command == "l":
                x, y = current[0] + x, current[1] + y
            current = (x, y)
            loop.append(current)
        elif command in ("H", "h"):
            x = number()
            if command == "h":
                x = current[0] + x
            current = (x, current[1])
            loop.append(current)
        elif command in ("V", "v"):
            y = number()
            if command == "v":
                y = current[1] + y
            current = (current[0], y)
            loop.append(current)
        elif command in ("C", "c"):
            x1, y1, x2, y2, x3, y3 = (number() for _ in range(6))
            if command == "c":
                x1, y1 = current[0] + x1, current[1] + y1
                x2, y2 = current[0] + x2, current[1] + y2
                x3, y3 = current[0] + x3, current[1] + y3
            for step in range(1, steps + 1):
                t = step / steps
                u = 1.0 - t
                loop.append((
                    u ** 3 * current[0] + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t ** 3 * x3,
                    u ** 3 * current[1] + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t ** 3 * y3,
                ))
            current = (x3, y3)
        elif command in ("Z", "z"):
            if loop:
                loops.append(loop)
                loop = []
            current = start
        else:
            raise BakeError(f"unsupported path command {command!r}")
    if loop:
        loops.append(loop)
    return loops


def element_loops(tag, attrs):
    if tag == "rect":
        return [rect_polygon(
            float(attrs.get("x", 0)), float(attrs.get("y", 0)),
            float(attrs["width"]), float(attrs["height"]), float(attrs.get("rx", 0)))]
    if tag == "circle":
        r = float(attrs["r"])
        return [ellipse_loop(float(attrs["cx"]), float(attrs["cy"]), r, r)]
    if tag == "ellipse":
        rotate = 0.0
        match = re.match(
            r"rotate\(\s*(" + NUM + r")\s+" + NUM + r"\s+" + NUM + r"\s*\)",
            attrs.get("transform", ""))
        if match:
            rotate = float(match.group(1))
        return [ellipse_loop(
            float(attrs["cx"]), float(attrs["cy"]),
            float(attrs["rx"]), float(attrs["ry"]), rotate)]
    if tag == "polygon":
        values = [float(v) for v in re.findall(NUM, attrs["points"])]
        return [list(zip(values[0::2], values[1::2]))]
    if tag == "path":
        return path_loops(attrs["d"])
    raise BakeError(f"unsupported element {tag!r}")


def fill_loops(canvas, loops, color):
    red, green, blue = color
    height = len(canvas[0])
    width = len(canvas)
    edges = []
    min_y = height
    max_y = 0
    for loop in loops:
        points = [(x * SS, y * SS) for x, y in loop]
        for i, (xa, ya) in enumerate(points):
            xb, yb = points[(i + 1) % len(points)]
            if ya != yb:
                if ya > yb:
                    xa, ya, xb, yb = xb, yb, xa, ya
                edges.append((xa, ya, xb, yb))
                min_y = min(min_y, int(ya))
                max_y = max(max_y, int(math.ceil(yb)))
    for y in range(max(0, min_y), min(height, max_y)):
        yc = y + 0.5
        intersections = []
        for (xa, ya, xb, yb) in edges:
            if ya <= yc < yb:
                intersections.append(xa + (yc - ya) / (yb - ya) * (xb - xa))
        intersections.sort()
        for i in range(0, len(intersections) - 1, 2):
            x_start = max(0, int(math.ceil(intersections[i] - 0.5)))
            x_end = min(width, int(math.ceil(intersections[i + 1] - 0.5)))
            for x in range(x_start, x_end):
                canvas[x][y] = (red, green, blue)


def rasterize(svg: str, width: int, height: int):
    svg = re.sub(r"<!--.*?-->", "", svg, flags=re.S)
    if "<text" in svg or "stroke=" in svg or "<linearGradient" in svg:
        raise BakeError("SVG uses text/stroke/gradient; the built-in rasterizer only supports filled shapes")

    view_box = re.search(r'viewBox="([^"]+)"', svg)
    if view_box:
        values = [float(v) for v in re.findall(NUM, view_box.group(1))]
        if len(values) != 4:
            raise BakeError("invalid viewBox")
        scale_x = width / values[2]
        scale_y = height / values[3]
    else:
        scale_x = scale_y = 1.0

    canvas = [[(0, 0, 0) for _ in range(height * SS)] for _ in range(width * SS)]
    for tag, attr_text in ELEM_RE.findall(svg):
        attrs = dict(ATTR_RE.findall(attr_text))
        fill = attrs.get("fill")
        if fill is None or fill == "none":
            continue
        loops = [
            [(x * scale_x, y * scale_y) for x, y in loop]
            for loop in element_loops(tag, attrs)
        ]
        fill_loops(canvas, loops, parse_color(fill))

    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            red = green = blue = alpha = 0
            for dy in range(SS):
                sy = y * SS + dy
                for dx in range(SS):
                    sx = x * SS + dx
                    r, g, b = canvas[sx][sy]
                    if r or g or b:
                        red += r
                        green += g
                        blue += b
                        alpha += 255
            count = SS * SS
            if alpha == 0:
                pixels += b"\x00\x00\x00\x00"
            else:
                pixels.append(red * 255 // alpha)
                pixels.append(green * 255 // alpha)
                pixels.append(blue * 255 // alpha)
                pixels.append(alpha // count)
    return bytes(pixels)


def encode_png(rgba: bytes, width: int, height: int) -> bytes:
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    scanlines = bytearray()
    stride = width * 4
    for y in range(height):
        scanlines.append(0)  # PNG filter: none
        scanlines += rgba[y * stride:(y + 1) * stride]
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(bytes(scanlines), 9)) + chunk(b"IEND", b""))


def blit_frame(page, page_size, x, y, size, rgba):
    """Copies one RGBA frame into the page and bleeds its edges by 1 px."""
    for row in range(size):
        src = rgba[row * size * 4:(row + 1) * size * 4]
        dst = ((y + row) * page_size + x) * 4
        page[dst:dst + size * 4] = src
    for row in range(size):
        src = rgba[(row * size) * 4:(row * size + 1) * 4]
        if x > 0:
            dst = ((y + row) * page_size + x - 1) * 4
            page[dst:dst + 4] = src
        src = rgba[(row * size + size - 1) * 4:(row * size + size) * 4]
        if x + size < page_size:
            dst = ((y + row) * page_size + x + size) * 4
            page[dst:dst + 4] = src
    for col in range(size):
        src = rgba[col * 4:col * 4 + 4]
        if y > 0:
            dst = ((y - 1) * page_size + x + col) * 4
            page[dst:dst + 4] = src
        src = rgba[((size - 1) * size + col) * 4:((size - 1) * size + col) * 4 + 4]
        if y + size < page_size:
            dst = ((y + size) * page_size + x + col) * 4
            page[dst:dst + 4] = src


def build_atlas(manifest, output_root: Path):
    config = manifest.get("atlas", {})
    if not config.get("enabled", False):
        return {"pages": [], "frames": [], "pageSize": 0}

    page_size = int(config.get("pageSize", 512))
    padding = int(config.get("padding", 1))

    items = []
    for icon in manifest["icons"]:
        if icon["kind"] not in {"monochrome", "color"}:
            continue
        # Scene billboards are sampled by the renderer as standalone textures.
        if "scene" in icon.get("targets", ["ui"]):
            continue
        source = (SOURCE_ROOT / icon["source"]).resolve()
        svg = source.read_text(encoding="utf-8")
        for size in icon["sizes"]:
            items.append((int(size), icon["id"], rasterize(svg, int(size), int(size))))

    items.sort(key=lambda item: (-item[0], item[1]))

    pages = []
    frames = []
    page = bytearray(page_size * page_size * 4)
    x = y = row_height = 0
    page_index = 0
    for size, icon_id, rgba in items:
        if x + size + padding > page_size:
            x = 0
            y += row_height + padding
            row_height = 0
        if y + size + padding > page_size:
            pages.append(bytes(page))
            page = bytearray(page_size * page_size * 4)
            page_index += 1
            x = y = row_height = 0

        blit_frame(page, page_size, x, y, size, rgba)
        frames.append({
            "id": icon_id,
            "size": size,
            "page": page_index,
            "pageSize": page_size,
            "u0": x / page_size,
            "v0": y / page_size,
            "u1": (x + size) / page_size,
            "v1": (y + size) / page_size,
        })
        x += size + padding
        row_height = max(row_height, size)

    if items:
        pages.append(bytes(page))

    page_names = []
    for index, data in enumerate(pages):
        name = f"editor-icons-atlas-{index}.png"
        (output_root / name).write_bytes(encode_png(data, page_size, page_size))
        page_names.append(name)

    return {"pages": page_names, "frames": frames, "pageSize": page_size}


def icon_file_stem(icon_id: str) -> str:
    return icon_id[len("editor.icon."):].replace(".", "-")


def cpp_identifier(icon_id: str) -> str:
    parts = re.split(r"[.\-]", icon_id[len("editor.icon."):])
    return "".join(part[:1].upper() + part[1:] for part in parts if part)


def cpp_string(value: str) -> str:
    return json.dumps(value)


def load_manifest():
    with MANIFEST.open("r", encoding="utf-8") as handle:
        manifest = json.load(handle)
    if manifest.get("schemaVersion") != 2:
        raise BakeError("manifest schemaVersion must be 2")

    ids = set()
    names = set()
    for icon in manifest["icons"]:
        icon_id = icon["id"]
        if not icon_id.startswith("editor.icon."):
            raise BakeError(f"invalid icon id {icon_id!r}")
        if icon_id in ids:
            raise BakeError(f"duplicate icon id {icon_id}")
        ids.add(icon_id)
        names.add(icon_id)
        for alias in icon.get("aliases", []):
            if alias in names:
                raise BakeError(f"alias {alias!r} clashes")
            names.add(alias)
        if "license" not in icon:
            raise BakeError(f"missing license for {icon_id}")
        if icon.get("kind") not in {"monochrome", "color", "glyph", "generated", "raster"}:
            raise BakeError(f"invalid kind for {icon_id}")
        if icon["kind"] in {"monochrome", "color", "raster"}:
            if not icon.get("source"):
                raise BakeError(f"missing source for {icon_id}")
            if not icon.get("sizes"):
                raise BakeError(f"missing sizes for {icon_id}")
    return manifest


def build_pngs(manifest, output_root: Path):
    for icon in manifest["icons"]:
        if icon["kind"] not in {"monochrome", "color"}:
            continue
        source = (SOURCE_ROOT / icon["source"]).resolve()
        if not source.is_file():
            raise BakeError(f"missing SVG source {source}")
        svg = source.read_text(encoding="utf-8")
        for size in icon["sizes"]:
            rgba = rasterize(svg, int(size), int(size))
            path = output_root / f"{icon_file_stem(icon['id'])}@{size}.png"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(encode_png(rgba, int(size), int(size)))


def build_generated(manifest, output_root: Path, atlas):
    output_root.mkdir(parents=True, exist_ok=True)

    lines = [
        "// Generated by Tools/Icons/iconbaker.py. Do not edit.",
        "#pragma once",
        "",
        '#include "Icons/EditorIconCatalog.h"',
        "",
        "namespace NLS::Editor::Icons::EditorIcons",
        "{",
    ]
    for icon in manifest["icons"]:
        lines.append(f'    inline constexpr IconId {cpp_identifier(icon["id"])} {{ {cpp_string(icon["id"])} }};')
    lines += ["}", ""]
    (output_root / "IconIds.generated.h").write_text("\n".join(lines), encoding="utf-8")

    catalog = [
        "// Generated by Tools/Icons/iconbaker.py. Do not edit.",
        '#include "Icons/Generated/IconCatalogGenerated.h"',
        "",
        '#include "Icons/EditorIconCatalog.h"',
        "",
        "namespace NLS::Editor::Icons",
        "{",
        "const std::vector<IconDefinition>& GeneratedIconDefinitions()",
        "{",
        "    static const std::vector<IconDefinition> definitions {",
    ]
    kind_map = {
        "monochrome": "IconKind::Monochrome",
        "color": "IconKind::Color",
        "glyph": "IconKind::Glyph",
        "generated": "IconKind::Generated",
        "raster": "IconKind::Raster",
    }
    for icon in manifest["icons"]:
        aliases = ", ".join(cpp_string(alias) for alias in icon.get("aliases", []))
        tags = ", ".join(cpp_string(tag) for tag in icon.get("tags", []))
        tiers = ", ".join(str(size) for size in icon.get("sizes", []))
        glyph = icon.get("codepoint", "0")
        catalog += [
            "        [] {",
            "            IconDefinition definition;",
            f"            definition.id = {cpp_string(icon['id'])};",
            f"            definition.kind = {kind_map[icon['kind']]};",
            f"            definition.defaultToken = IconToken::{icon.get('defaultToken', 'Small')};",
            f"            definition.themeable = {'true' if icon.get('themeable', False) else 'false'};",
            f"            definition.module = {cpp_string(icon.get('module', 'editor'))};",
            f"            definition.glyph = {glyph};",
            f"            definition.aliases = {{ {aliases} }};",
            f"            definition.tags = {{ {tags} }};",
            f"            definition.tiers = {{ {tiers} }};",
            f"            definition.deprecatedBy = {cpp_string(icon.get('deprecatedBy', ''))};",
            "            return definition;",
            "        }(),",
        ]
    catalog += [
        "    };",
        "    return definitions;",
        "}",
        "}",
        "",
    ]
    (output_root / "IconCatalog.generated.cpp").write_text("\n".join(catalog), encoding="utf-8")

    resources = [
        "// Generated by Tools/Icons/iconbaker.py. Do not edit.",
        '#include "Icons/Generated/IconCatalogGenerated.h"',
        "",
        '#include "Core/EditorResourceCatalog.h"',
        "",
        "namespace NLS::Editor::Core",
        "{",
        "std::vector<EditorResourceRecord> GeneratedIconRecords()",
        "{",
        "    return {",
    ]
    for icon in manifest["icons"]:
        if icon["kind"] in {"glyph", "generated"}:
            continue
        stem = icon_file_stem(icon["id"])
        size = max(int(s) for s in icon["sizes"])
        resource_type = "SceneTexture" if "scene" in icon.get("targets", ["ui"]) else "Icon"
        resources += [
            "        {",
            f"            {cpp_string(icon['id'])},",
            f"            EditorResourceType::{resource_type},",
            "            EditorResourceScope::Editor,",
            f"            {cpp_string(f'Editor/Icons/{stem}@{size}.png')},",
            f"            {cpp_string(f'editor/icons/{stem}@{size}.png')},",
            "        },",
        ]
    for index, _ in enumerate(atlas.get("pages", [])):
        resources += [
            "        {",
            f"            {cpp_string(f'editor.icon.atlas.page{index}')},",
            "            EditorResourceType::Texture,",
            "            EditorResourceScope::Editor,",
            f"            {cpp_string(f'Editor/Icons/editor-icons-atlas-{index}.png')},",
            f"            {cpp_string(f'editor/icons/editor-icons-atlas-{index}.png')},",
            "        },",
        ]
    resources += ["    };", "}", "}", ""]
    (output_root / "IconResources.generated.cpp").write_text("\n".join(resources), encoding="utf-8")

    atlas_header = [
        "// Generated by Tools/Icons/iconbaker.py. Do not edit.",
        "#pragma once",
        "",
        '#include "Icons/IconService.h"',
        "",
        "namespace NLS::Editor::Icons",
        "{",
        "const std::vector<IconAtlasEntry>& GeneratedIconAtlasEntries();",
        "}",
        "",
    ]
    (output_root / "IconAtlas.generated.h").write_text("\n".join(atlas_header), encoding="utf-8")

    atlas_cpp = [
        "// Generated by Tools/Icons/iconbaker.py. Do not edit.",
        '#include "Icons/Generated/IconAtlas.generated.h"',
        "",
        "namespace NLS::Editor::Icons",
        "{",
        "const std::vector<IconAtlasEntry>& GeneratedIconAtlasEntries()",
        "{",
        "    static const std::vector<IconAtlasEntry> entries {",
    ]
    for frame in atlas.get("frames", []):
        page_id = f"editor.icon.atlas.page{frame['page']}"
        atlas_cpp += [
            "        {",
            f"            {cpp_string(frame['id'])}, {frame['size']},",
            f"            {{ {cpp_string(page_id)}, {frame['pageSize']}, "
            f"{frame['u0']:.8f}f, {frame['v0']:.8f}f, {frame['u1']:.8f}f, {frame['v1']:.8f}f }},",
            "        },",
        ]
    atlas_cpp += ["    };", "    return entries;", "}", "}", ""]
    (output_root / "IconAtlas.generated.cpp").write_text("\n".join(atlas_cpp), encoding="utf-8")


def write_handwritten_header(output_root: Path):
    output_root.mkdir(parents=True, exist_ok=True)
    header = output_root / "IconCatalogGenerated.h"
    header.write_text(
        "// Hand-written declarations for IconBaker-generated tables.\n"
        "#pragma once\n\n"
        "#include <vector>\n\n"
        '#include "Core/EditorResourceCatalog.h"\n'
        '#include "Icons/EditorIconCatalog.h"\n\n'
        "namespace NLS::Editor::Icons\n"
        "{\n"
        "    const std::vector<IconDefinition>& GeneratedIconDefinitions();\n"
        "}\n\n"
        "namespace NLS::Editor::Core\n"
        "{\n"
        "    std::vector<EditorResourceRecord> GeneratedIconRecords();\n"
        "}\n",
        encoding="utf-8")


def run(output_root: Path, generated_root: Path):
    manifest = load_manifest()
    build_pngs(manifest, output_root)
    atlas = build_atlas(manifest, output_root)
    write_handwritten_header(generated_root)
    build_generated(manifest, generated_root, atlas)


def files_equal(left: Path, right: Path) -> bool:
    if not left.is_file() or not right.is_file():
        return False
    return left.read_bytes() == right.read_bytes()


def check() -> int:
    with tempfile.TemporaryDirectory(prefix="nullus-iconbaker-") as temp:
        temp_root = Path(temp)
        assets = temp_root / "assets"
        generated = temp_root / "generated"
        run(assets, generated)

        differences = []
        for source_dir, target_dir in ((assets, ASSET_OUT), (generated, GENERATED_OUT)):
            if not source_dir.is_dir():
                continue
            for path in source_dir.iterdir():
                target = target_dir / path.name
                if not files_equal(path, target):
                    differences.append(str(target.relative_to(ROOT)))
        if differences:
            print("iconbaker: generated files are stale:", file=sys.stderr)
            for difference in differences:
                print(f"  {difference}", file=sys.stderr)
            print("Run: python Tools/Icons/iconbaker.py", file=sys.stderr)
            return 1
        print("iconbaker: generated assets and code are up to date")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        if args.check:
            return check()

        if ASSET_OUT.exists():
            for path in ASSET_OUT.glob("*@*.png"):
                path.unlink()
        run(ASSET_OUT, GENERATED_OUT)
        print(f"iconbaker: wrote {len(list(ASSET_OUT.glob('*.png')))} PNGs and generated C++")
        return 0
    except BakeError as error:
        print(f"iconbaker: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
