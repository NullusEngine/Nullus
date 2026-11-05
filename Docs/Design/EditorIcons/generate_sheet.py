"""Generates the Unity-style editor icon design sheet (design-only, not wired to the manifest)."""

from __future__ import annotations

import base64
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
SVG_OUT = OUT / "svg"

C = {
    "mono": "#C9CED6",
    "mono_light": "#4A4F57",
    "neutral": "#A7AFBA",
    "neutral_dark": "#7C8591",
    "neutral_light": "#D6DBE2",
    "amber": "#F2B84B",
    "amber_dark": "#C98F2E",
    "amber_light": "#FFD37A",
    "teal": "#4FC3A1",
    "teal_dark": "#1F7F6B",
    "teal_light": "#D8FFF4",
    "blue": "#5B9CF5",
    "blue_light": "#A8CBFF",
    "blue_dark": "#3C72C4",
    "green": "#6FCB72",
    "green_mid": "#4E9E55",
    "green_dark": "#3C7F45",
    "green_light": "#B9EFB8",
    "purple": "#9B7CF0",
    "purple_light": "#D6C8FF",
    "purple_mid": "#8B6BEA",
    "purple_dark": "#6E4FC2",
    "orange": "#F08A4B",
    "orange_mid": "#D97A38",
    "orange_dark": "#B85E2E",
    "orange_light": "#FFD0A8",
    "pink": "#E07AB8",
    "pink_light": "#F7B8DC",
    "cyan": "#4FB3C7",
    "cyan_dark": "#2C7F90",
    "cyan_light": "#A9E6F0",
    "indigo": "#6F7FE8",
    "indigo_light": "#C9D0FF",
    "yellow": "#F2C94C",
    "yellow_light": "#FFE9A8",
    "yellow_dark": "#D9A62E",
    "red": "#D9534F",
    "white": "#FFFFFF",
}


def poly(points, fill):
    pts = " ".join(f"{x:.2f},{y:.2f}" for x, y in points)
    return f'<polygon points="{pts}" fill="{fill}"/>'


def rect(x, y, w, h, rx, fill):
    return f'<rect x="{x:.2f}" y="{y:.2f}" width="{w:.2f}" height="{h:.2f}" rx="{rx:.2f}" fill="{fill}"/>'


def circle(cx, cy, r, fill):
    return f'<circle cx="{cx:.2f}" cy="{cy:.2f}" r="{r:.2f}" fill="{fill}"/>'


def path(d, fill, extra=""):
    return f'<path d="{d}" fill="{fill}" {extra}/>'


def arc_points(cx, cy, r, a0, a1, steps=64):
    return [
        (cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / steps)),
         cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / steps)))
        for i in range(steps + 1)
    ]


def arc_band(cx, cy, ro, ri, a0, a1, fill, steps=64):
    outer = arc_points(cx, cy, ro, a0, a1, steps)
    inner = arc_points(cx, cy, ri, a1, a0, steps)
    return poly(outer + inner, fill)


def ring(cx, cy, ro, ri, fill):
    return arc_band(cx, cy, ro, ri, 0, 360, fill, 96)


def ellipse_band(cx, cy, rx, ry, width, fill, rot=0.0, a0=0.0, a1=360.0):
    rot_r = math.radians(rot)
    ox, oy = rx + width / 2, ry + width / 2
    ix, iy = max(rx - width / 2, 0.05), max(ry - width / 2, 0.05)

    def point(r_x, r_y, angle):
        x = r_x * math.cos(math.radians(angle))
        y = r_y * math.sin(math.radians(angle))
        return (cx + x * math.cos(rot_r) - y * math.sin(rot_r),
                cy + x * math.sin(rot_r) + y * math.cos(rot_r))

    steps = 96
    outer = [point(ox, oy, a0 + (a1 - a0) * i / steps) for i in range(steps + 1)]
    inner = [point(ix, iy, a1 - (a1 - a0) * i / steps) for i in range(steps + 1)]
    return poly(outer + inner, fill)


def bar(x1, y1, x2, y2, width, fill, round_caps=False):
    dx, dy = x2 - x1, y2 - y1
    length = math.hypot(dx, dy)
    if length == 0:
        return ""
    nx, ny = -dy / length * width / 2, dx / length * width / 2
    out = poly([(x1 + nx, y1 + ny), (x2 + nx, y2 + ny),
                (x2 - nx, y2 - ny), (x1 - nx, y1 - ny)], fill)
    if round_caps:
        out += circle(x1, y1, width / 2, fill) + circle(x2, y2, width / 2, fill)
    return out


def triangle(tip, base_a, base_b, fill):
    return poly([tip, base_a, base_b], fill)


def arrow(tip, direction, length, width, fill):
    dx, dy = direction
    norm = math.hypot(dx, dy)
    dx, dy = dx / norm, dy / norm
    bx, by = tip[0] - dx * length, tip[1] - dy * length
    nx, ny = -dy * width / 2, dx * width / 2
    return poly([tip, (bx + nx, by + ny), (bx - nx, by - ny)], fill)


def crescent(cx, cy, radius, inner_radius, offset, fill):
    d = offset
    x = (d * d + radius * radius - inner_radius * inner_radius) / (2 * d)
    y = math.sqrt(max(0.0, radius * radius - x * x))
    au = math.degrees(math.atan2(y, x))
    al = -au
    pu = math.degrees(math.atan2(y, x - d))
    pl = -pu
    outer = arc_points(cx, cy, radius, au, 360 + al, 64)
    inner = arc_points(cx + d, cy, inner_radius, pl, pu - 360, 64)
    return poly(outer + inner, fill)


def star(cx, cy, outer, inner, fill, points=4, rotation=-90):
    pts = []
    for i in range(points * 2):
        radius = outer if i % 2 == 0 else inner
        angle = math.radians(rotation + i * 180 / points)
        pts.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    return poly(pts, fill)


def doc(fill=None, fold=None):
    base = fill or C["neutral"]
    edge = fold or C["neutral_dark"]
    body = path(
        "M6.5 2.5 H15 L19.5 7 V18 C19.5 19.4 18.4 20.5 17 20.5 "
        "H6.5 C5.1 20.5 4 19.4 4 18 V5 C4 3.6 5.1 2.5 6.5 2.5 Z", base)
    return body + poly([(15, 2.5), (19.5, 7), (15, 7)], edge)


def script_doc():
    return doc(fill="#DDE3EB", fold="#AEB8C5")


def script_logo(color="#5B9CF5", slash="#A8CBFF"):
    return (bar(10.95, 15.8, 12.65, 8.2, 1.7, slash, True) +
            poly([(8.75, 8.0), (8.55, 9.4), (6.05, 12.0), (8.55, 14.6), (8.75, 16.0), (4.95, 12.0)], color) +
            poly([(14.75, 8.0), (14.95, 9.4), (17.45, 12.0), (14.95, 14.6), (14.75, 16.0), (18.55, 12.0)], color))


def script_logo_small(color="#5B9CF5"):
    return (bar(7.6, 7.4, 8.2, 4.2, 0.8, "#A8CBFF", True) +
            poly([(6.6, 4.2), (6.7, 5.0), (5.8, 5.9), (6.7, 6.8), (6.6, 7.4), (4.8, 5.9)], color) +
            poly([(9.4, 4.2), (9.3, 5.0), (10.2, 5.9), (9.3, 6.8), (9.4, 7.4), (11.2, 5.9)], color))


def script_badge_base():
    return circle(17.5, 18.4, 3.1, "#4A4F57")


# ---------------------------------------------------------------------------
# Asset-type icons
# ---------------------------------------------------------------------------

def asset_folder():
    return (poly([(3, 6.5), (9.8, 6.5), (11.8, 8.8), (21, 8.8), (21, 12), (3, 12)], C["amber_dark"]) +
            rect(2.5, 9.2, 19, 11.3, 2, C["amber"]) +
            rect(4.2, 10.8, 16, 1.3, 0.65, C["amber_light"]))


def asset_texture():
    return (rect(3, 4, 18, 16, 2, C["teal"]) +
            rect(5, 6, 14, 12, 1.2, C["teal_dark"]) +
            circle(8.5, 9.5, 1.6, C["teal_light"]) +
            poly([(5.5, 17.5), (10, 11.8), (13, 15), (15.5, 12.5), (18.5, 17.5)], C["teal_light"]))


def asset_material():
    return (circle(12, 12, 8, C["blue"]) +
            path("M4.8 15.5 C6.2 18.5 8.9 20 12 20 C15.1 20 17.8 18.5 19.2 15.5 "
                 "C17.6 17.3 15 18.2 12 18.2 C9 18.2 6.4 17.3 4.8 15.5 Z", C["blue_dark"]) +
            circle(9.2, 8.9, 2.4, C["blue_light"]))


def asset_mesh():
    return (poly([(12, 2.5), (20.5, 7.2), (12, 12), (3.5, 7.2)], C["green_light"]) +
            poly([(3.5, 7.2), (12, 12), (12, 21.5), (3.5, 16.7)], C["green_mid"]) +
            poly([(20.5, 7.2), (12, 12), (12, 21.5), (20.5, 16.7)], C["green_dark"]))


def asset_prefab():
    back = poly([(14.5, 5.0), (20.5, 8.3), (14.5, 11.6), (8.5, 8.3)], C["blue_light"])
    back += poly([(8.5, 8.3), (14.5, 11.6), (14.5, 19.0), (8.5, 15.7)], C["blue"])
    back += poly([(20.5, 8.3), (14.5, 11.6), (14.5, 19.0), (20.5, 15.7)], C["blue_dark"])
    front = poly([(9.5, 9.5), (15.5, 12.8), (9.5, 16.1), (3.5, 12.8)], C["blue_light"])
    front += poly([(3.5, 12.8), (9.5, 16.1), (9.5, 21.5), (3.5, 18.2)], C["blue"])
    front += poly([(15.5, 12.8), (9.5, 16.1), (9.5, 21.5), (15.5, 18.2)], C["blue_dark"])
    return back + front


def asset_scene():
    return (poly([(12, 3.5), (20, 8), (12, 12.5), (4, 8)], "#C9CED6") +
            poly([(4, 8), (12, 12.5), (12, 21), (4, 16.5)], "#8E96A1") +
            poly([(20, 8), (12, 12.5), (12, 21), (20, 16.5)], "#6E7681") +
            poly([(5.8, 9.9), (10.4, 12.4), (10.4, 17.6), (5.8, 15.1)], "#C9CED6") +
            poly([(6.8, 10.8), (9.4, 12.3), (9.4, 16.7), (6.8, 15.2)], "#3A3D44"))


def asset_shader():
    s_shape = (
        "M15.5 7.2 C15.5 5.4 13.8 4.4 11.7 4.4 C9.2 4.4 7.5 5.7 7.5 7.7 "
        "C7.5 9.4 8.7 10.4 10.9 10.8 L12.8 11.2 C14.3 11.5 14.9 12.0 14.9 12.9 "
        "C14.9 13.9 13.9 14.5 12.4 14.5 C10.7 14.5 9.7 13.8 9.7 12.5 L7.5 12.5 "
        "C7.5 14.9 9.4 16.3 12.3 16.3 C15.2 16.3 17.1 14.8 17.1 12.6 "
        "C17.1 10.8 15.9 9.9 13.7 9.5 L11.8 9.1 C10.5 8.8 9.8 8.4 9.8 7.6 "
        "C9.8 6.7 10.6 6.2 11.8 6.2 C13.0 6.2 13.6 6.8 13.6 7.8 Z")
    return doc(fold=C["purple_dark"]) + path(s_shape, C["purple"])


def asset_shader_graph():
    light = C["purple_light"]
    return (rect(3, 3, 18, 18, 3, "#4E3A8C") +
            ring(7.5, 6.5, 2.6, 1.2, light) +
            bar(7.5, 9.1, 7.5, 11.3, 2.1, light, True) +
            bar(7.5, 11.3, 15.3, 11.3, 2.1, light, True) +
            bar(15.3, 11.3, 15.3, 15.0, 2.1, light, True) +
            bar(15.3, 15.0, 18.0, 17.6, 2.1, light, True) +
            circle(15.3, 11.3, 1.0, light) +
            arrow((19.2, 18.8), (0.707, 0.707), 3.0, 4.2, light))


def asset_shader_function():
    light = C["purple_light"]
    return (rect(3, 3, 18, 18, 3, "#4E3A8C") +
            bar(12, 12, 14, 12, 1.8, light, True) +
            bar(14, 12, 14, 7.25, 1.8, light, True) +
            bar(14, 7.25, 16, 7.25, 1.8, light, True) +
            bar(14, 12, 14, 16.75, 1.8, light, True) +
            bar(14, 16.75, 16, 16.75, 1.8, light, True) +
            rect(4.5, 8, 7.5, 8, 1.5, "#B79CFF") +
            rect(16, 4.5, 4.5, 5.5, 1.2, C["purple_mid"]) +
            rect(16, 14, 4.5, 5.5, 1.2, C["purple_mid"]) +
            bar(7.2, 9.8, 9.4, 9.8, 1.4, "#4E3A8C", True) +
            bar(7.2, 9.8, 7.2, 15.0, 1.5, "#4E3A8C", True) +
            bar(5.8, 12.0, 9.2, 12.0, 1.3, "#4E3A8C", True) +
            circle(16, 7.25, 0.8, light) +
            circle(16, 16.75, 0.8, light))


def asset_script():
    return script_doc() + script_logo()


def asset_script_csharp():
    return (script_doc() + script_logo_small() +
            rect(9.75, 8.8, 1.8, 9.0, 0.35, "#3FAE6A") +
            rect(13.35, 8.8, 1.8, 9.0, 0.35, "#3FAE6A") +
            rect(8.45, 11.8, 6.6, 1.8, 0.35, "#3FAE6A") +
            rect(8.45, 14.8, 6.6, 1.8, 0.35, "#3FAE6A"))


def asset_script_lua():
    return (script_doc() + script_logo_small() +
            crescent(11.75, 13.5, 5.0, 4.0, 2.7, C["blue"]) +
            circle(16.3, 8.8, 1.0, C["blue_light"]))


def asset_font():
    return path(
        "M6.5 19 L10.6 4.5 L13.4 4.5 L17.5 19 L14.6 19 L13.7 15.8 L10.3 15.8 L9.4 19 Z "
        "M11.2 13.8 L12.8 13.8 L12 8.2 Z",
        C["neutral"], 'fill-rule="evenodd"')


def asset_audio():
    return (poly([(3, 9.4), (6.8, 9.4), (11, 5.4), (11, 18.6), (6.8, 14.6), (3, 14.6)], C["pink"]) +
            arc_band(11.2, 12, 7.1, 5.6, -55, 55, C["pink_light"]) +
            arc_band(11.2, 12, 4.7, 3.4, -52, 52, C["pink_light"]))


def asset_default():
    return (doc() +
            rect(7, 10, 8, 1.5, 0.75, C["neutral_dark"]) +
            rect(7, 13, 10, 1.5, 0.75, C["neutral_dark"]) +
            rect(7, 16, 6, 1.5, 0.75, C["neutral_dark"]))


def asset_ui_document():
    return (rect(2.5, 4, 19, 16, 2, C["cyan"]) +
            rect(4.5, 6, 15, 12, 1, C["cyan_dark"]) +
            rect(4.5, 6, 15, 3, 1, C["cyan_light"]) +
            rect(4.5, 9.8, 5, 8.2, 0.8, C["cyan_dark"]) +
            rect(11, 10.6, 8.5, 2, 0.6, C["cyan_light"]) +
            rect(11, 14, 8.5, 1.5, 0.6, C["cyan"]) +
            rect(11, 16.8, 5.5, 1.5, 0.6, C["cyan"]))


def asset_ui_style():
    return (poly([(15.5, 3.5), (20.5, 8.5), (12, 17), (7, 12)], C["cyan_light"]) +
            poly([(7, 12), (12, 17), (10, 19), (5, 14)], C["cyan"]) +
            poly([(5, 14), (10, 19), (7, 21.5), (2.8, 17.2)], C["cyan_dark"]) +
            circle(18.5, 18.5, 2, C["cyan"]))


def asset_localization():
    return (circle(12, 12, 8, C["indigo"]) +
            ellipse_band(12, 12, 3.2, 8, 1.5, C["indigo_light"]) +
            rect(4.3, 11.1, 15.4, 1.8, 0.9, C["indigo_light"]))


def asset_vfx_system():
    return (star(12, 12, 8.8, 2.7, C["orange"]) +
            circle(12, 12, 2.2, C["orange_light"]) +
            circle(19, 4.5, 1.5, C["yellow"]) +
            circle(4.8, 19, 1.1, C["yellow"]))


def asset_vfx_script():
    return (doc() +
            star(16, 15.5, 4.6, 1.3, C["orange"]) +
            circle(18.8, 19, 1, C["yellow"]))


def asset_vfx_emitter():
    return (rect(8.6, 2.8, 6.8, 3.6, 1, C["orange_dark"]) +
            rect(9.6, 3.6, 4.8, 1.1, 0.55, C["orange_light"]) +
            poly([(9.4, 6.4), (14.6, 6.4), (20, 21), (4, 21)], C["orange"]) +
            poly([(10.5, 8), (13.5, 8), (17.2, 19.2), (6.8, 19.2)], "#F6B07C") +
            circle(12, 10, 1.2, C["yellow_light"]) +
            circle(9.5, 13, 1, C["yellow_light"]) +
            circle(14.5, 13.5, 1.2, C["yellow_light"]) +
            circle(11.5, 16, 1.4, C["yellow_light"]) +
            circle(6.8, 18.5, 1, C["yellow_light"]) +
            circle(16.8, 19.5, 1.1, C["yellow_light"]))


# ---------------------------------------------------------------------------
# Transform toolbar (monochrome)
# ---------------------------------------------------------------------------

def toolbar_move():
    m = "MONO"
    return (rect(11, 4, 2, 16, 0.7, m) + rect(4, 11, 16, 2, 0.7, m) +
            triangle((12, 1.5), (16, 7.5), (8, 7.5), m) +
            triangle((12, 22.5), (8, 16.5), (16, 16.5), m) +
            triangle((1.5, 12), (7.5, 8), (7.5, 16), m) +
            triangle((22.5, 12), (16.5, 16), (16.5, 8), m))


def toolbar_rotate():
    m = "MONO"
    return (arc_band(12, 12, 8.5, 6.5, 200, 330, m) +
            arc_band(12, 12, 8.5, 6.5, 20, 150, m) +
            arrow((20.5, 10.3), (0.5, 0.866), 3.2, 4.6, m) +
            arrow((3.5, 13.7), (-0.5, -0.866), 3.2, 4.6, m) +
            circle(12, 12, 1.7, m))


def toolbar_local():
    m = "MONO"
    return (bar(12, 3.5, 19.5, 7.75, 2, m, True) +
            bar(19.5, 7.75, 12, 12, 2, m, True) +
            bar(12, 12, 4.5, 7.75, 2, m, True) +
            bar(4.5, 7.75, 12, 3.5, 2, m, True) +
            bar(4.5, 7.75, 4.5, 16.25, 2, m, True) +
            bar(12, 12, 12, 20.5, 2, m, True) +
            bar(19.5, 7.75, 19.5, 16.25, 2, m, True) +
            bar(4.5, 16.25, 12, 20.5, 2, m, True) +
            bar(19.5, 16.25, 12, 20.5, 2, m, True))


def toolbar_scale():
    m = "MONO"
    out = rect(3, 3, 4, 4, 0.8, m)
    out += rect(12, 12, 9, 2, 0.5, m)
    out += rect(12, 19, 9, 2, 0.5, m)
    out += rect(12, 14, 2, 5, 0.5, m)
    out += rect(19, 14, 2, 5, 0.5, m)
    out += bar(6, 6, 10.5, 10.5, 2, m, True)
    out += triangle((12.2, 12.2), (6.8, 11), (11, 6.8), m)
    return out


def toolbar_pivot():
    m = "MONO"
    return (ring(12, 12, 6.5, 4.5, m) +
            circle(12, 12, 1.8, m) +
            rect(11, 1.5, 2, 3.5, 0.5, m) +
            rect(11, 19, 2, 3.5, 0.5, m) +
            rect(1.5, 11, 3.5, 2, 0.5, m) +
            rect(19, 11, 3.5, 2, 0.5, m))


def toolbar_center():
    m = "MONO"
    return (rect(5, 5, 14, 2, 0.5, m) + rect(5, 17, 14, 2, 0.5, m) +
            rect(5, 7, 2, 10, 0.5, m) + rect(17, 7, 2, 10, 0.5, m) +
            circle(12, 12, 2.2, m))


def toolbar_global():
    m = "MONO"
    return (ring(12, 12, 8, 6.2, m) +
            rect(4, 11.1, 16, 1.8, 0.9, m) +
            ellipse_band(12, 12, 3.2, 8, 1.5, m))


# ---------------------------------------------------------------------------
# Playback, glyphs, fallback
# ---------------------------------------------------------------------------

def tool_play():
    return poly([(7.5, 4), (20, 12), (7.5, 20)], "MONO")


def tool_pause():
    return rect(6, 4, 4, 16, 1, "MONO") + rect(14, 4, 4, 16, 1, "MONO")


def tool_stop():
    return rect(5.5, 5.5, 13, 13, 2, "MONO")


def tool_step_forward():
    return (poly([(6, 4), (16.5, 12), (6, 20)], "MONO") +
            rect(17.5, 4, 3, 16, 1, "MONO"))


def tool_refresh():
    m = "MONO"
    return (arc_band(12, 12, 8.5, 6.5, 30, 270, m) +
            arrow((17.0, 4.5), (1.0, 0.0), 5.0, 6.0, m))


def glyph_caret_down():
    return poly([(4.5, 8), (12, 15.5), (19.5, 8), (18, 6.5), (12, 12.5), (6, 6.5)], "MONO")


def glyph_caret_right():
    return poly([(8, 4.5), (15.5, 12), (8, 19.5), (6.5, 18), (12.5, 12), (6.5, 6)], "MONO")


def glyph_close():
    m = "MONO"
    return (bar(5.5, 5.5, 18.5, 18.5, 2.6, m, True) +
            bar(18.5, 5.5, 5.5, 18.5, 2.6, m, True))


def glyph_search():
    m = "MONO"
    return (ring(10.5, 10.5, 6.3, 4.5, m) +
            bar(14.8, 14.8, 19.5, 19.5, 2.6, m, True))


def glyph_more():
    return (circle(5.5, 12, 1.8, "MONO") +
            circle(12, 12, 1.8, "MONO") +
            circle(18.5, 12, 1.8, "MONO"))


def generated_missing():
    return (rect(2.5, 2.5, 19, 19, 2.5, "#D92BD9") +
            arc_band(12, 9.0, 4.6, 2.3, 170, 380, C["white"]) +
            bar(16.1, 10.2, 12, 14.6, 2.4, C["white"], True) +
            circle(12, 18.2, 1.9, C["white"]))


# ---------------------------------------------------------------------------
# Scene billboards (color, 128 px targets)
# ---------------------------------------------------------------------------

def scene_light_point():
    out = ""
    for angle in (210, 240, 270, 300, 330):
        r1, r2 = 7.1, 10.0
        x1 = 12 + r1 * math.cos(math.radians(angle))
        y1 = 10 + r1 * math.sin(math.radians(angle))
        x2 = 12 + r2 * math.cos(math.radians(angle))
        y2 = 10 + r2 * math.sin(math.radians(angle))
        out += bar(x1, y1, x2, y2, 1.6, C["yellow_dark"], True)
    out += circle(12, 10, 5.2, C["yellow"])
    out += circle(10.2, 8.1, 1.8, C["yellow_light"])
    out += rect(9.6, 14.4, 4.8, 2.0, 0.8, C["amber_dark"])
    out += rect(10.2, 16.6, 3.6, 2.0, 0.8, C["amber_dark"])
    out += rect(10.8, 18.8, 2.4, 1.6, 0.6, C["amber_dark"])
    return out


def scene_light_spot():
    return (circle(12, 4.4, 2.4, C["amber_dark"]) +
            rect(9.4, 5.6, 5.2, 2.2, 0.8, C["amber_dark"]) +
            circle(12, 4.4, 1.2, C["yellow_light"]) +
            poly([(9.2, 7.5), (14.8, 7.5), (20.5, 21), (3.5, 21)], C["orange"]) +
            poly([(10.4, 8.4), (13.6, 8.4), (17.3, 19), (6.7, 19)], "#F6B07C"))


def scene_light_directional():
    out = ""
    for angle in range(0, 360, 45):
        r1, r2 = 6.5, 9.5
        x1 = 12 + r1 * math.cos(math.radians(angle))
        y1 = 12 + r1 * math.sin(math.radians(angle))
        x2 = 12 + r2 * math.cos(math.radians(angle))
        y2 = 12 + r2 * math.sin(math.radians(angle))
        out += bar(x1, y1, x2, y2, 1.8, C["yellow_dark"], True)
    out += circle(12, 12, 5.0, C["yellow"])
    out += circle(12, 12, 2.8, C["yellow_light"])
    return out


def scene_light_ambient_box():
    return (poly([(12, 5), (19, 9), (12, 13), (5, 9)], "#9CC4FF") +
            poly([(5, 9), (12, 13), (12, 21), (5, 17)], "#5B8FE0") +
            poly([(19, 9), (12, 13), (12, 21), (19, 17)], "#4777C4") +
            poly([(12, 10.2), (15.8, 12.8), (12, 15.4), (8.2, 12.8)], "#D6E8FF") +
            circle(12, 12.8, 1.2, C["white"]))


def scene_light_ambient_sphere():
    return (circle(12, 12, 8, "#4E86D8") +
            ellipse_band(12, 12, 3.4, 8, 1.2, "#BFD8FF") +
            rect(4, 11.4, 16, 1.2, 0.6, "#BFD8FF") +
            circle(8.8, 8.2, 2.2, "#BFD8FF") +
            circle(12, 12, 2.2, "#EAF3FF") +
            circle(12, 12, 0.9, C["white"]))


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------

ASSETS = [
    ("asset.folder", "文件夹", asset_folder),
    ("asset.texture", "纹理", asset_texture),
    ("asset.material", "材质", asset_material),
    ("asset.mesh", "网格", asset_mesh),
    ("asset.prefab", "预制体", asset_prefab),
    ("asset.scene", "场景", asset_scene),
    ("asset.shader", "着色器", asset_shader),
    ("asset.script", "脚本", asset_script),
    ("asset.script.csharp", "C# 脚本", asset_script_csharp),
    ("asset.script.lua", "Lua 脚本", asset_script_lua),
    ("asset.font", "字体", asset_font),
    ("asset.audio", "音频", asset_audio),
    ("asset.default", "默认资源", asset_default),
    ("asset.ui-document", "UI 文档", asset_ui_document),
    ("asset.ui-style", "UI 样式", asset_ui_style),
    ("asset.localization", "本地化", asset_localization),
    ("asset.vfx-system", "VFX 系统", asset_vfx_system),
    ("asset.vfx-script", "VFX 脚本", asset_vfx_script),
    ("asset.vfx-emitter", "VFX 发射器", asset_vfx_emitter),
    ("asset.shader-graph", "着色器图", asset_shader_graph),
    ("asset.shader-function", "着色器函数", asset_shader_function),
]

TOOLBAR = [
    ("toolbar.move", "移动", toolbar_move),
    ("toolbar.rotate", "旋转", toolbar_rotate),
    ("toolbar.scale", "缩放", toolbar_scale),
    ("toolbar.pivot", "轴心", toolbar_pivot),
    ("toolbar.center", "居中", toolbar_center),
    ("toolbar.global", "全局", toolbar_global),
    ("toolbar.local", "局部", toolbar_local),
]

PLAYBACK = [
    ("tool.play", "播放", tool_play),
    ("tool.pause", "暂停", tool_pause),
    ("tool.stop", "停止", tool_stop),
    ("tool.step.forward", "单步", tool_step_forward),
    ("tool.refresh", "刷新", tool_refresh),
]

GLYPHS = [
    ("glyph.caret-down", "下箭头", glyph_caret_down),
    ("glyph.caret-right", "右箭头", glyph_caret_right),
    ("glyph.close", "关闭", glyph_close),
    ("glyph.search", "搜索", glyph_search),
    ("glyph.more", "更多", glyph_more),
]

SCENE = [
    ("scene.light.point", "点光源", scene_light_point),
    ("scene.light.spot", "聚光灯", scene_light_spot),
    ("scene.light.directional", "平行光", scene_light_directional),
    ("scene.light.ambient.box", "环境盒", scene_light_ambient_box),
    ("scene.light.ambient.sphere", "环境球", scene_light_ambient_sphere),
]

ALL = ASSETS + TOOLBAR + PLAYBACK + GLYPHS + SCENE


def build_bodies():
    return {key: fn() for key, _, fn in ALL} | {"generated.missing": generated_missing()}


BODIES = build_bodies()


def body_for(key, mono=None):
    body = BODIES[key]
    if "MONO" in body:
        body = body.replace("MONO", mono or C["mono"])
    return body


def icon(key, size, mono=None, extra=""):
    return (f'<svg class="icon {extra}" width="{size}" height="{size}" viewBox="0 0 24 24" '
            f'fill="none" xmlns="http://www.w3.org/2000/svg">{body_for(key, mono)}</svg>')


def write_svg_files():
    SVG_OUT.mkdir(parents=True, exist_ok=True)
    entries = [(key, fn) for key, _, fn in ALL] + [("generated.missing", generated_missing)]
    for key, _ in entries:
        stem = key.replace(".", "-") + ".svg"
        mono = "#C0C0C0" if "MONO" in BODIES[key] else None
        text = ('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" '
                'viewBox="0 0 24 24" fill="none">\n' + body_for(key, mono) + "\n</svg>\n")
        (SVG_OUT / stem).write_text(text, encoding="utf-8")


def old_png(name):
    path = ROOT / "App" / "Assets" / "Editor" / "Icons" / name
    if not path.exists():
        return ""
    data = base64.b64encode(path.read_bytes()).decode("ascii")
    return f'<img class="old" src="data:image/png;base64,{data}" alt="current {name}"/>'


def tile(key, number, name, suffix, size=84, mono=None):
    return f"""
    <div class="tile">
      <span class="num">{number:02d}</span>
      <div class="iconbox" style="height:{size + 16}px">{icon(key, size, mono)}</div>
      <div class="name">{name}</div>
      <code>{suffix}</code>
    </div>"""


def small_tile(key, name, suffix, size=48, mono=None):
    return f"""
    <div class="tile small">
      <div class="dual">
        <div class="iconbox tiny">{icon(key, size, mono)}</div>
        <div class="iconbox tiny">{icon(key, 16, mono)}</div>
      </div>
      <div class="name">{name}</div>
      <code>{suffix}</code>
    </div>"""


def palette_html():
    items = [
        ("中性", C["neutral"]), ("琥珀", C["amber"]), ("青绿", C["teal"]),
        ("蓝色", C["blue"]), ("绿色", C["green"]), ("紫色", C["purple"]),
        ("橙色", C["orange"]), ("粉色", C["pink"]), ("青色", C["cyan"]),
        ("靛蓝", C["indigo"]), ("黄色", C["yellow"]), ("错误红", C["red"]),
    ]
    return "".join(
        f'<div class="sw"><i style="background:{hexv}"></i><span>{name}</span><code>{hexv}</code></div>'
        for name, hexv in items)


def grid_spec_html():
    lines = []
    for i in range(25):
        major = i % 4 == 0
        lines.append(
            f'<line x1="{i}" y1="0" x2="{i}" y2="24" stroke="#3B3E46" '
            f'stroke-width="{0.09 if major else 0.04}"/>')
        lines.append(
            f'<line x1="0" y1="{i}" x2="24" y2="{i}" stroke="#3B3E46" '
            f'stroke-width="{0.09 if major else 0.04}"/>')
    grid = (f'<svg class="grid" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg">'
            + "".join(lines) +
            '<rect x="2" y="2" width="20" height="20" fill="none" stroke="#F2B84B" '
            'stroke-width="0.18" stroke-dasharray="0.8 0.6"/></svg>')
    return f"""
    <div class="grid-wrap">
      {grid}
      <div class="grid-icon">{icon("asset.folder", 166)}</div>
    </div>
    <div class="spec-caption">24 × 24 网格 · 20 × 20 安全区<br/><code>1 单位 = 1 px @ 24 px</code></div>"""


def ladder_row(key, name):
    cells = "".join(
        f'<td><div class="ladder-cell">{icon(key, size)}</div><em>{size}</em></td>'
        for size in (16, 24, 32, 48, 64, 128))
    return f"<tr><th>{name}<code>{key}</code></th>{cells}</tr>"


def light_strip():
    assets = "".join(
        f'<div class="light-item">{icon(key, 36)}<span>{name}</span></div>'
        for key, name, _ in ASSETS)
    mono = "".join(
        f'<div class="light-mono">{icon(key, 24, C["mono_light"])}</div>'
        for key, _, _ in TOOLBAR + PLAYBACK + GLYPHS)
    return f'<div class="light-assets">{assets}</div><div class="light-monos">{mono}</div>'


def build_html():
    asset_tiles = "".join(
        tile(key, i + 1, name, key, 88) for i, (key, name, _) in enumerate(ASSETS))
    toolbar_tiles = "".join(
        small_tile(key, name, key, 48) for i, (key, name, _) in enumerate(TOOLBAR))
    playback_tiles = "".join(small_tile(key, name, key) for key, name, _ in PLAYBACK)
    glyph_tiles = "".join(small_tile(key, name, key) for key, name, _ in GLYPHS)
    missing_tile = (
        '<div class="tile small special"><div class="dual"><div class="iconbox tiny">'
        + icon("generated.missing", 48) +
        '</div><div class="iconbox tiny">' + icon("generated.missing", 16) +
        '</div></div><div class="name">缺失占位</div><code>generated.missing</code>'
        '<span class="tag">Unity 洋红占位</span></div>')
    scene_tiles = "".join(
        f"""<div class="tile scene">
          <div class="iconbox scene-box">{icon(key, 128)}</div>
          <div class="name">{name}</div><code>{key}</code>
        </div>""" for key, name, _ in SCENE)

    family_tiles = "".join(
        f"""<div class="family">
          <div class="dual family-dual">
            <div class="iconbox family-box">{icon(key, 64)}</div>
            <div class="iconbox family-box small">{icon(key, 32)}</div>
          </div>
          <div class="name">{name}</div><code>{key}</code>
        </div>""" for key, name in [
            ("asset.script", "脚本"),
            ("asset.script.csharp", "C# 脚本"),
            ("asset.script.lua", "Lua 脚本"),
            ("asset.shader", "着色器"),
            ("asset.shader-graph", "着色器图"),
            ("asset.shader-function", "着色器函数"),
        ])

    before_after = "".join(f"""
      <div class="ba-item">
        <div class="ba-name">{name}</div>
        <div class="ba-pair">
          <div class="ba-cell"><div class="ba-box">{old_png(old)}</div><span>现稿</span></div>
          <div class="ba-arrow">→</div>
          <div class="ba-cell"><div class="ba-box">{icon(key, 64)}</div><span>新稿</span></div>
        </div>
      </div>""" for key, name, old in [
        ("asset.folder", "文件夹", "asset-folder@128.png"),
        ("asset.material", "材质", "asset-material@128.png"),
        ("asset.script", "脚本", "asset-script@128.png"),
        ("toolbar.move", "移动", "toolbar-move@128.png"),
    ])

    return f"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>Nullus Editor · Unity 风格图标设计稿 v4</title>
<style>
  :root {{
    --bg:#17181b; --panel:#222429; --panel2:#1d1f23; --line:#30333a;
    --text:#e8ebf0; --muted:#98a0ac; --dim:#6f7682; --accent:#6ea8ff;
  }}
  * {{ box-sizing:border-box; }}
  html,body {{ margin:0; background:var(--bg); color:var(--text); }}
  body {{ font-family:"Segoe UI","Microsoft YaHei",system-ui,sans-serif; -webkit-font-smoothing:antialiased; }}
  code {{ font-family:ui-monospace,"Cascadia Mono",Consolas,monospace; }}
  .page {{ width:1600px; margin:0 auto; padding:54px 56px 64px; }}
  .eyebrow {{ color:var(--accent); font-size:12px; font-weight:700; letter-spacing:.18em; text-transform:uppercase; }}
  h1 {{ font-size:44px; line-height:1.08; margin:12px 0 10px; letter-spacing:-.02em; }}
  h1 span {{ color:#9b7cf0; }}
  .sub {{ color:var(--muted); font-size:16px; line-height:1.65; max-width:930px; margin:0; }}
  .revision {{ margin-top:14px; color:#9fb7d8; font-size:12px; line-height:1.6; background:#1d2530;
               border:1px solid #2c3b4f; border-radius:8px; padding:8px 11px; display:inline-block; }}
  .revision b {{ color:#cfe2ff; }}
  .head {{ display:flex; justify-content:space-between; gap:40px; align-items:flex-start; }}
  .badge {{ border:1px solid #3b4250; background:#20252e; color:#b9c6dd; padding:10px 14px; border-radius:999px;
           font-size:12px; font-weight:600; white-space:nowrap; margin-top:8px; }}
  .badge b {{ color:#ffd37a; }}
  .section {{ margin-top:44px; }}
  .section-head {{ display:flex; align-items:baseline; gap:14px; border-bottom:1px solid var(--line); padding-bottom:12px; margin-bottom:18px; }}
  h2 {{ font-size:22px; margin:0; letter-spacing:-.01em; }}
  .section-head .count {{ color:var(--dim); font-size:13px; font-family:ui-monospace,Consolas,monospace; }}
  .section-head .note {{ margin-left:auto; color:var(--muted); font-size:12px; }}
  .specs {{ display:grid; grid-template-columns:1.05fr 1fr 1.55fr; gap:16px; margin-top:26px; }}
  .card {{ background:var(--panel); border:1px solid var(--line); border-radius:14px; padding:18px 20px; }}
  .card h3 {{ margin:0 0 10px; font-size:14px; letter-spacing:.04em; color:#cfd6e2; }}
  .card p {{ margin:0; color:var(--muted); font-size:12px; line-height:1.65; }}
  .grid-wrap {{ position:relative; width:192px; height:192px; margin:4px auto 8px; }}
  .grid {{ width:100%; height:100%; display:block; }}
  .grid-icon {{ position:absolute; inset:0; display:flex; align-items:center; justify-content:center; opacity:.94; }}
  .spec-caption {{ text-align:center; color:var(--dim); font-size:11px; line-height:1.6; }}
  .rules {{ list-style:none; margin:0; padding:0; display:grid; gap:9px; }}
  .rules li {{ color:#c9d0da; font-size:12px; line-height:1.5; padding-left:18px; position:relative; }}
  .rules li:before {{ content:""; position:absolute; left:0; top:6px; width:7px; height:7px; border-radius:2px; background:var(--accent); }}
  .palette {{ display:grid; grid-template-columns:repeat(3,1fr); gap:9px 12px; }}
  .sw {{ display:grid; grid-template-columns:18px 1fr; grid-template-rows:auto auto; column-gap:8px; align-items:center; }}
  .sw i {{ grid-row:span 2; width:18px; height:30px; border-radius:5px; border:1px solid #ffffff22; }}
  .sw span {{ font-size:11px; color:#c9d0da; }}
  .sw code {{ font-size:9px; color:var(--dim); }}
  .asset-grid {{ display:grid; grid-template-columns:repeat(7,1fr); gap:14px; }}
  .tile {{ position:relative; background:var(--panel); border:1px solid var(--line); border-radius:12px;
          padding:12px 10px 14px; text-align:center; overflow:hidden; }}
  .tile .num {{ position:absolute; top:9px; left:10px; font-family:ui-monospace,Consolas,monospace; font-size:9px; color:#5d6470; }}
  .iconbox {{ display:flex; align-items:center; justify-content:center; background:var(--panel2);
             border:1px solid #2b2e35; border-radius:9px; margin-bottom:9px; }}
  .tile .name {{ font-size:14px; font-weight:600; letter-spacing:.01em; }}
  .tile code {{ display:block; color:var(--dim); font-size:9px; margin-top:3px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }}
  .toolbar-grid {{ display:grid; grid-template-columns:repeat(7,1fr); gap:14px; }}
  .family-grid {{ display:grid; grid-template-columns:repeat(6,1fr); gap:14px; }}
  .family {{ background:var(--panel); border:1px solid var(--line); border-radius:12px; padding:12px 10px 12px; text-align:center; }}
  .family .dual {{ display:flex; align-items:center; justify-content:center; gap:10px; margin-bottom:9px; }}
  .family-box {{ width:86px; height:86px; margin:0; }}
  .family-box.small {{ width:46px; height:46px; }}
  .family .name {{ font-size:13px; font-weight:600; }}
  .family code {{ display:block; color:var(--dim); font-size:9px; margin-top:3px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }}
  .small-grid {{ display:grid; grid-template-columns:repeat(6,1fr); gap:14px; }}
  .tile.small {{ padding:12px 10px 12px; }}
  .dual {{ display:flex; align-items:center; justify-content:center; gap:14px; margin-bottom:10px; }}
  .iconbox.tiny {{ width:72px; height:72px; margin:0; }}
  .tag {{ display:inline-block; margin-top:6px; font-size:9px; color:#ffd37a; background:#3b3424;
         border:1px solid #5a4b2b; padding:2px 6px; border-radius:999px; }}
  .special {{ border-color:#4a3d2a; }}
  .scene-grid {{ display:grid; grid-template-columns:repeat(5,1fr); gap:14px; }}
  .scene .scene-box {{ height:172px; }}
  .scene .name {{ font-size:15px; }}
  .ladder {{ width:100%; border-collapse:collapse; }}
  .ladder th {{ text-align:left; font-size:12px; color:#c9d0da; font-weight:600; width:190px; padding-right:16px; }}
  .ladder th code {{ display:block; color:var(--dim); font-size:9px; font-weight:400; margin-top:3px; }}
  .ladder td {{ padding:4px; }}
  .ladder-cell {{ display:flex; align-items:center; justify-content:center; height:138px; background:var(--panel2);
                 border:1px solid #2b2e35; border-radius:9px; }}
  .ladder em {{ display:block; text-align:center; color:var(--dim); font-size:9px; font-style:normal; margin-top:3px; }}
  .light-panel {{ background:#f1f2f5; color:#202126; border-radius:14px; padding:20px 22px; }}
  .light-panel h3 {{ margin:0 0 14px; font-size:13px; letter-spacing:.06em; color:#59606c; }}
  .light-assets {{ display:grid; grid-template-columns:repeat(11,1fr); gap:10px 8px; }}
  .light-item {{ display:flex; flex-direction:column; align-items:center; gap:4px; background:#fff; border:1px solid #e0e3e9;
                border-radius:9px; padding:8px 4px; }}
  .light-item span {{ font-size:9px; color:#69707c; white-space:nowrap; }}
  .light-monos {{ display:flex; gap:8px; flex-wrap:wrap; margin-top:14px; padding-top:14px; border-top:1px solid #dfe2e8; }}
  .light-mono {{ width:40px; height:40px; display:flex; align-items:center; justify-content:center; background:#fff;
               border:1px solid #e0e3e9; border-radius:8px; }}
  .ba-grid {{ display:grid; grid-template-columns:repeat(4,1fr); gap:14px; }}
  .ba-item {{ background:var(--panel); border:1px solid var(--line); border-radius:12px; padding:12px; }}
  .ba-name {{ font-size:12px; color:#c9d0da; margin-bottom:8px; }}
  .ba-pair {{ display:flex; align-items:center; gap:8px; }}
  .ba-cell {{ display:flex; flex-direction:column; align-items:center; gap:4px; }}
  .ba-box {{ width:86px; height:86px; display:flex; align-items:center; justify-content:center; background:var(--panel2);
            border:1px solid #2b2e35; border-radius:9px; }}
  .ba-box .old {{ width:64px; height:64px; image-rendering:auto; }}
  .ba-cell span {{ font-size:9px; color:var(--dim); }}
  .ba-arrow {{ color:#5d6470; font-size:16px; }}
  .footer {{ margin-top:46px; border-top:1px solid var(--line); padding-top:20px; display:grid;
            grid-template-columns:1.4fr 1fr; gap:36px; }}
  .footer h3 {{ margin:0 0 8px; font-size:14px; }}
  .footer p, .footer li {{ color:var(--muted); font-size:12px; line-height:1.7; }}
  .footer ol {{ margin:0; padding-left:18px; }}
  .footer b {{ color:#dfe5ee; }}
  .foot-note {{ color:var(--dim); font-size:10px; margin-top:18px; }}
</style>
</head>
<body>
<div class="page">
  <div class="head">
    <div>
      <div class="eyebrow">Nullus Editor · Icon System</div>
      <h1>编辑器图标重设计 <span>Unity 风格</span></h1>
      <p class="sub">统一为“轮廓优先、单一强调色、少细节”的编辑器中立语言。全部图标使用 24 × 24 画板与纯色平涂，
      保留现有 <code>editor.icon.*</code> ID、档位与 Atlas；本页仅为设计稿，确认前不修改任何 SVG 源或生成资源。</p>
      <div class="revision"><b>v4 修正：</b><code>&lt;/&gt;</code> 视觉居中；C# <code>#</code> 重新居中并留出安全间距</div>
    </div>
    <div class="badge">设计稿 v4 · <b>待确认</b></div>
  </div>

  <div class="specs">
    <div class="card">
      <h3>网格与比例</h3>
      {grid_spec_html()}
    </div>
    <div class="card">
      <h3>绘制规则</h3>
      <ul class="rules">
        <li>视觉笔画 <b>2 px</b>（24 网格），最小间隙 1.5 px</li>
        <li>圆角 1.5 px；箭头与端点保持锐利</li>
        <li>每个资源图标 1 个主色 + 1 个中性色，最多 3 个平涂色阶</li>
        <li>不使用文字标签；仅用单字母/符号（A、S、#、&lt;/&gt;）作类型标记</li>
        <li>16 px 先读剪影；32 px 起展示类型细节</li>
      </ul>
    </div>
    <div class="card">
      <h3>调色板</h3>
      <div class="palette">{palette_html()}</div>
      <p style="margin-top:12px">资源类型用色相区分；工具、播放与字形使用可着色中性色（<code>#C0C0C0</code> 源，运行时 tint）。</p>
    </div>
  </div>

  <div class="section">
    <div class="section-head"><h2>资源类型</h2><span class="count">21 icons · 源 24 · 档位 32 / 64 / 128 px</span>
      <span class="note">脚本家族统一浅色页面；C# / Lua 主标识为语言 logo，左上角保留 <code>&lt;/&gt;</code></span></div>
    <div class="asset-grid">{asset_tiles}</div>
  </div>

    <div class="section">
    <div class="section-head"><h2>家族一致性</h2><span class="count">64 / 32 px</span>
      <span class="note">脚本共享 <code>&lt;/&gt;</code> logo；着色器使用页 / 节点 / Subgraph 三种语义</span></div>
    <div class="family-grid">{family_tiles}</div>
  </div>

  <div class="section">
    <div class="section-head"><h2>变换工具</h2><span class="count">7 icons · 16 / 32 / 48 px</span>
      <span class="note">单色源，运行时按主题 tint</span></div>
    <div class="toolbar-grid">{toolbar_tiles}</div>
  </div>

  <div class="section">
    <div class="section-head"><h2>播放控制 / 字形 / 占位</h2><span class="count">5 + 5 + 1</span>
      <span class="note">左：48 px · 右：16 px</span></div>
    <div class="small-grid">{playback_tiles}{glyph_tiles}{missing_tile}</div>
  </div>

  <div class="section">
    <div class="section-head"><h2>场景灯光 Billboard</h2><span class="count">5 icons · 128 px</span>
      <span class="note">独立 SceneTexture，不进入 Atlas</span></div>
    <div class="scene-grid">{scene_tiles}</div>
  </div>

  <div class="section">
    <div class="section-head"><h2>尺寸阶梯</h2><span class="count">16 → 128 px</span>
      <span class="note">重点检查 16 px 的工具与 32 px 的资源图标</span></div>
    <table class="ladder">
      {ladder_row("asset.folder", "文件夹")}
      {ladder_row("asset.material", "材质")}
      {ladder_row("asset.prefab", "预制体")}
      {ladder_row("asset.scene", "场景")}
      {ladder_row("asset.shader", "着色器")}
      {ladder_row("asset.script", "脚本")}
      {ladder_row("toolbar.move", "移动")}
      {ladder_row("tool.play", "播放")}
    </table>
  </div>

  <div class="section">
    <div class="section-head"><h2>浅色主题检查</h2><span class="count">Light preview</span>
      <span class="note">色彩不依赖深色背景；中性色切换到深灰</span></div>
    <div class="light-panel">
      <h3>资源图标 · 36 px</h3>
      {light_strip()}
    </div>
  </div>

  <div class="section">
    <div class="section-head"><h2>方向对比</h2><span class="count">4 representative</span>
      <span class="note">仅用于说明变化，不保留旧稿</span></div>
    <div class="ba-grid">{before_after}</div>
  </div>

  <div class="footer">
    <div>
      <h3>确认后的实施范围</h3>
      <ol>
        <li>替换 <b>38 个静态 SVG 源</b>（21 资源 + 7 工具 + 5 播放 + 5 场景），保持现有 ID / 档位 / alias。</li>
        <li>5 个字形改为 SVG 单色图标（<b>ID 不变</b>，从 Font Awesome 字形切换为纹理路径）。</li>
        <li>缺失占位保留运行时绘制，改为 Unity 风格的洋红底 + 白色问号。</li>
        <li>运行 <code>iconbaker.py</code> 重建 PNG / Atlas / 生成表，更新校验与图标测试。</li>
        <li>面板代码不变；Asset Browser、Top Bar、Icon Browser 自动生效。</li>
      </ol>
    </div>
    <div>
      <h3>设计取舍</h3>
      <p><b>为什么更 Unity：</b>减少内部细节，优先让 16–32 px 的剪影承担识别；资源类型使用稳定的色相编码，
      工具和播放控制保持单色，避免与资源色冲突。</p>
      <p><b>需要你确认：</b>资源图标的色相分配、字形是否转为 SVG、以及是否接受场景灯光的半写实细节。
      确认后再进入实现。</p>
      <div class="foot-note">Generated design sheet · 44 icons · 24 × 24 grid · no manifest changes</div>
    </div>
  </div>
</div>
</body>
</html>
"""


def main():
    write_svg_files()
    html = build_html()
    (OUT / "design-sheet.html").write_text(html, encoding="utf-8")
    print(f"wrote {len(BODIES)} icons to {SVG_OUT}")
    print(f"wrote {OUT / 'design-sheet.html'}")


if __name__ == "__main__":
    main()
