# Transparent Thumbnail Letterbox Design

## Goal

Prevent image thumbnails from showing black bars when the source aspect ratio does not match the square Asset Browser thumbnail area. The unused area must remain transparent so the Asset Browser background shows through.

## Current Behavior

Thumbnail generation already preserves the source aspect ratio and encodes RGBA PNG data. During Asset Browser drawing, texture thumbnails are fitted inside the square icon bounds, then the full bounds are filled with opaque black before the image is drawn. This display-layer fill creates the visible black bars.

## Design

Keep `ComputeAssetBrowserThumbnailRect` unchanged so thumbnails continue to fit inside their bounds without stretching or cropping. Stop requesting a letterbox background for texture thumbnails and remove the corresponding opaque black draw path from `DrawProjectGridItemThumbnail`.

The unused portion of the icon bounds will not receive a thumbnail-specific fill. Normal Asset Browser content underneath it will therefore remain visible. Thumbnail PNG dimensions, alpha data, cache keys, and cache files remain unchanged.

## Scope

- Change only Asset Browser thumbnail presentation behavior.
- Do not pad thumbnail PNG files to square dimensions.
- Do not stretch or crop source images.
- Do not change model, material, prefab, or GPU preview rendering.
- Do not hand-edit generated files.

## Testing

Update the focused Asset Browser presentation test first so it requires no thumbnail type to request an opaque letterbox background. Run the test and confirm it fails against the current texture-specific behavior, then apply the display-layer change and rerun the focused test suite.

## Compatibility

The change is backend-independent because it removes an ImGui background primitive and leaves image texture rendering untouched. Existing cached thumbnails remain valid and do not need regeneration.
