# Editor Icon Redesign · Unity Style (v4)

Design-only reference. The v4 SVGs were copied into `Tools/Icons/Source/editor`
and baked on 2026-10-21; this folder remains the visual review/history copy.
Nothing here is read at runtime.

| File | Purpose |
| --- | --- |
| `unity-style-icons-v4.png` | Final review sheet, 44 icons |
| `design-sheet.html` | Scrollable version of the same sheet |
| `svg/` | 44 design SVGs (24 × 24, fill-only) |
| `generate_sheet.py` | Regenerates the sheet and SVG set |

## v4 change

- Centered the `</>` logo by moving the slash to the visual center and
  balancing both chevrons.
- Centered the C# `#` in the page, moved it below the small script marker,
  and added safe spacing so it is fully visible at 32 px.

## v3 change

- Script family uses a light page. Generic script keeps the large `</>` logo;
  C# and Lua use the language logo as the main mark and keep a small `</>`
  script marker at the top-left.

## v2 changes

- `toolbar.local`: wireframe cube (object/local space), not an axis triad.
- `toolbar.rotate`: two curved arrows + center dot (Unity RotateTool).
- `tool.refresh`: single circular arrow (Unity Refresh).
- `generated.missing`: Unity-style magenta fallback with a white question mark.
- Script family: shared `</>` page logo; C# adds a green `#`, Lua adds a moon.
  (Superseded by the v3 hierarchy above.)
- `asset.scene`: neutral hollow cube (Unity SceneAsset), not a clapperboard.
- `asset.shader`: page with an `S` (Unity shader page).
- `asset.shader-graph`: node path with a start circle and arrow (Unity GraphAsset).
- `asset.shader-function`: node with two outputs and an `f` (Unity SubgraphAsset).

All 44 SVGs rasterize with the existing `iconbaker.py` at 32 and 128 px; no
text, stroke, gradient, or group elements are used.
