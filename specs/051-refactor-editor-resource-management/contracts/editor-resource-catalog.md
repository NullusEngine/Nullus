# Editor Resource Catalog Contract

## Purpose

The editor resource catalog is the single public index for editor-owned resources. Callers use stable Nullus IDs and do not depend on the loose development tree, packaged layout, process current directory, or legacy imported icon filenames.

## Resource IDs

- IDs are stable strings such as `editor.icon.asset.material`, `editor.font.ui.default`, and `engine.brand.logo.mark`.
- IDs and active paths must not contain Unity-related names.
- A duplicate ID is rejected when records are registered.

## Resource Scopes

- `Editor`: editor-only UI resources under `App/Assets/Editor` in development.
- `RuntimeBuiltin`: shared built-in resources under `App/Assets/Engine` in development.
- `ProjectUser`: project assets are not cataloged as editor resources.
- `Generated`: generated previews stay outside static editor icons and remain owned by `ThumbnailCache`.

## Backends

Development mode resolves `developmentPath` relative to the install assets root:

```text
<install-root>/App/Assets/<developmentPath>
```

Packaged mode returns `packagedPath` unchanged as the package key. Release package readers can bind that key to pak or embedded resource bytes without changing call sites.

## Path Rules

- Development paths must be relative, contained paths.
- Packaged paths must be relative package keys.
- Resource roots are derived from the executable install root or an explicit test root, never from the current working directory.
- Lookup failure diagnostics should include the ID, backend mode, category, and attempted location.

## Thumbnail Boundary

Asset previews are not static editor icons. Texture, material, mesh/model, and prefab previews use `AssetThumbnailService` and cache images under project `Library/AssetThumbnails`. Static catalog icons are only fallback images when no preview exists.
