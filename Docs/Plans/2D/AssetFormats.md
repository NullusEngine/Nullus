# Asset formats and identity

Texture remains the source asset and primary artifact. Sprite is a separate logical child artifact. Authoring slice definitions live in the existing .meta settings, each with a persistent UUID. A slice's name or position in an array is never its identity.

Sprite data includes source texture reference, texture dimensions, pixel rect, normalized pivot, PPU, border, original size/trim offset, UV/mesh and physics outline. Phase 1 implements full-rect Single Sprite; unimplemented authoring modes remain tracked in FeatureMatrix.md.

The Phase 2 resource increment adds `SPRITE_MODE` and version 1 `SPRITE_SLICES` metadata, persistent per-slice UUIDs, checked Grid generation and shared Editor/Cook Multiple compilation. Individual Sprite artifacts retain their v1 format and Texture primary identity. See [SpriteSlicing.md](SpriteSlicing.md) for the implemented schema, tests and remaining authoring tools. Border/trim/mesh/outline data is still a later format extension.

Artifact types are appended to existing enums without renumbering old values. Native artifacts carry their own format version and validated payload. Scene references use existing GUID/local-file-ID mapping, never physical artifact paths, GPU handles or atlas UVs.

Atlas mapping is (source asset, sprite child identity) -> page/UV/rotation/trim. Repacking changes the mapping revision, not references in scenes, prefabs, clips or tiles. Page and mapping publication must be atomic. Defaults: rotation off, mipmaps off, padding/extrusion enabled.

Shared Cook and Editor paths must generate equivalent logical sprite metadata. Target texture resizing changes normalized sampling coordinates, not the sprite's world size. Runtime manifests retain dependencies on the target Texture/Atlas and materials; Shipping does not require source images or ArtifactDB.

Future versioned source assets: SpriteAtlas, TileSet/Tile/Tilemap, PhysicsMaterial2D, SpriteLibrary/SpriteSkin and SpriteShape. Migration readers and invalid-reference diagnostics accompany each format change.
