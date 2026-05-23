# Research: LightGrid Performance Toggle

## Decision: Store the toggle as a project/editor rendering setting

**Rationale**: Project Settings already has reflection-driven UI and JSON persistence. A reflected settings object gives users an immediate Editor UI without bespoke panel code.

**Alternatives considered**:
- Environment variable only: useful for profiling but not discoverable in the editor.
- Driver-only constructor setting: necessary internally, but insufficient for user control.

## Decision: Keep LightGrid enabled by default

**Rationale**: Existing scenes rely on current rendering behavior. Default enabled avoids visual regressions for existing projects.

**Alternatives considered**:
- Default disabled: improves performance for some scenes but silently changes lighting.

## Decision: Use a no-LightGrid context when disabled

**Rationale**: Forward and deferred graph builders already consume a LightGrid context. Providing an explicit disabled/empty context keeps the graph assembly path unified.

**Alternatives considered**:
- Fork renderers around every LightGrid call: higher risk and creates divergent render graph behavior.

## Decision: Cache per-frame LightGrid context on BaseSceneRenderer

**Rationale**: Tracy showed repeated `BuildLightGridCompileContext` and `LightGridPrepass::Prepare` work in the same frame. BaseSceneRenderer owns the common helper and can memoize one context per frame descriptor/skybox state for both BeginFrame builder capture and later graph/package assembly.

**Alternatives considered**:
- Cache inside LightGridPrepass only: reduces some work but does not prevent duplicate higher-level context construction.
- Move all preparation to RHI thread: larger architecture change, not required for the requested fix.
