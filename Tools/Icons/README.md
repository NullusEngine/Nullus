# Tools/Icons

Editor UI icon pipeline. See the public [Editor Workspace](../../Docs/en/editor/workspace.md) guide for the editor context; this page documents the source and generation workflow.

## Source of truth

| Path | Purpose |
| --- | --- |
| `Source/editor/*.svg` | **Only hand-editable image source** for static icons |
| `icons.manifest.json` | **Only icon metadata source** (id, kind, sizes, tags, aliases, license) |
| `iconbaker.py` | Deterministic builder: SVG -> multi-size PNGs + generated C++ |
| `validate_icons.py` | File-level validation used by CI |
| `../../App/Assets/Editor/Icons/*.png` | Generated tiers (committed, never hand-edited) |
| `../../Project/Editor/Icons/Generated/` | Generated C++ (committed) |

## Commands

```powershell
python Tools/Icons/iconbaker.py           # regenerate PNGs + generated C++
python Tools/Icons/iconbaker.py --check   # rebuild into temp and compare (CI)
python Tools/Icons/validate_icons.py      # manifest/source/PNG/ID validation
python Tools/Icons/validate_icons.py --baseline-ref origin/main  # published-ID protection
```

## Rasterizer

The baker uses a built-in, dependency-free rasterizer for the flat SVG subset:
`rect` (+`rx`), `circle`, `ellipse` (+`rotate`), `polygon`, and `path` with
`M/L/H/V/C/Z` (multiple subpaths supported). Strokes, text, and gradients are
rejected with a clear diagnostic; author with fills.

The tool version is the script itself, so output is byte-identical across
machines. A future optional `resvg` backend can be added without changing the
manifest or generated file formats.

## Atlas

`icons.manifest.json` optionally packs UI tiers into pages:

```json
"atlas": { "enabled": true, "pageSize": 512, "padding": 1 }
```

Pages are written as `editor-icons-atlas-<n>.png` and frames to
`Project/Editor/Icons/Generated/IconAtlas.generated.{h,cpp}`. Scene-targeted
icons are excluded. Set `"enabled": false` to fall back to standalone tier PNGs
without code changes.

## Notes

- Static icons use 24 × 24 SVGs (any viewBox is supported). Glyph IDs
  `editor.icon.glyph.*` are now monochrome SVG icons at 16/32/48; built-in
  Font Awesome codepoints are empty.
- `generated.missing` is a color diagnostic icon at 16/32/48. Unknown/failed
  icon lookups still fall back to `editor.icon.asset.default`.
- Atlas packing is build-time only; the current manifest produces 4 pages.

## Adding an icon

1. Add `Source/editor/<name>.svg` (24 × 24 preferred; any viewBox is supported, flat fills).
2. Add a manifest entry with `id`, `kind`, `source`, `sizes`, `license`.
3. Run `python Tools/Icons/iconbaker.py`.
4. Run the tests; commit the SVG, manifest, PNGs, and generated C++ together.

Generated C++ lives in `Project/Editor/Icons/Generated/` because the editor
CMake source glob excludes `/Gen/`.
