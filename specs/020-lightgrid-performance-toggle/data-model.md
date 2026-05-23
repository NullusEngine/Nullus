# Data Model: LightGrid Performance Toggle

## Editor Rendering Settings

**Purpose**: Reflected settings object shown in Project Settings.

**Fields**:
- `enableLightGrid`: boolean, default `true`.

**Validation**:
- Missing persisted value keeps default `true`.
- Saved value must round-trip through existing settings persistence.

## Driver Runtime Rendering Settings

**Purpose**: Runtime copy of rendering options used by renderers and backend-facing helpers.

**Fields**:
- `enableLightGrid`: boolean, default `true`.

**Relationships**:
- Editor and Game contexts populate this from project/editor settings before constructing `Driver`.
- Renderers query the driver through existing renderer access helpers.

## LightGrid Frame Context Cache

**Purpose**: Stores the LightGrid context prepared for the current scene frame so BeginFrame and package assembly do not repeat the expensive preparation.

**Fields**:
- Frame identity derived from the active frame descriptor/snapshot.
- `hasSkyboxTexture`: boolean input to LightGrid context building.
- Prepared LightGrid compile context.
- Validity flag.

**State Transitions**:
- Invalid at renderer construction and after LightGrid disable.
- Populated on first LightGrid context request in a frame.
- Reused for subsequent requests in the same frame with matching inputs.
- Rebuilt on next frame or changed inputs.
