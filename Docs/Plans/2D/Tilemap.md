# Tilemap

Grid/TileSet/Tile/Tilemap assets share the scene, material and asset systems. Store tiles in dedicated 32x32 chunks, never one Entity per tile. Negative coordinates use floor division. Base assets are immutable during runtime; component edits are copy-on-write and clone correctly with Prefabs.

An edit invalidates its chunk and rule-dependent neighbours. Publish render/physics caches with revision checks at their safe boundaries. Rule tiles support neighbour requirements and rotated/mirrored matches; animated/random tiles preserve deterministic identity. Layout conversion is shared by rectangular, isometric and hexagonal tools and queries.

Streaming interest includes camera coverage, physics bodies and explicit anchors. Dirty unloaded chunks retain their edits. Range-readable cooked chunk data uses the existing content source and package infrastructure.

Tile Palette includes pencil/erase/fill/rectangle/select/move/copy/paste/eyedropper/rotation/flip/layers. A brush stroke is one Undo transaction; large edits use chunk deltas. Collider visibility and Rule Tile editing are required acceptance paths.
