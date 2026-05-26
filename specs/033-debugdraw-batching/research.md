# Research: DebugDrawPass Line Batching

## Decision: Batch consecutive line runs by color, depth mode, and line width

**Rationale**: These fields affect visible output or render state for debug lines. Color is consumed by the debug material, depth mode controls whether depth testing is enabled, and line width maps into the pipeline line-width state. Merging across any of them could draw a line with the wrong visual style. Limiting batches to consecutive compatible runs preserves relative draw order with non-line primitives and incompatible line states.

**Alternatives considered**: Include category and lifetime in the key. Rejected because `CollectVisiblePrimitives()` already applies category visibility and lifetime presentation rules before the pass renders primitives.

## Decision: Batch only line primitives in this delivery

**Rationale**: The selected-object slowdown is line-heavy: bounds spheres, grids, and helper wireframes emit many line segments. Line batching addresses the measured draw submission explosion while avoiding unrelated changes to point sizing and triangle fill/wireframe behavior.

**Alternatives considered**: Batch all debug primitive types immediately. Rejected because it expands scope into separate state rules without evidence that those primitives are the bottleneck.

## Decision: Preserve the existing per-primitive shader path

**Rationale**: The current debug shader supports placeholder meshes by selecting `u_Point0`, `u_Point1`, and `u_Point2` with `SV_VertexID`. Existing point and triangle rendering depend on that behavior. A shader flag lets batched lines read vertex positions while existing primitives keep the old path.

**Alternatives considered**: Switch all debug rendering to vertex-position meshes. Rejected because it would require rebuilding point and triangle mesh generation and changes more behavior than needed.

## Decision: Use transient batch meshes first

**Rationale**: One transient mesh per batch sharply reduces draw submissions using existing `Resources::Mesh` APIs. This is a narrow change that can be unit-tested without introducing a new dynamic-buffer ownership model.

**Alternatives considered**: Add a persistent dynamic debug-line buffer. Rejected for this iteration because it requires RHI/resource lifetime design across backends. It remains a follow-up if profiling shows transient upload cost is still significant.
