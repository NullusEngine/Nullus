# Data Model: DebugDrawPass Line Batching

## Line Batch Key

Represents the render-state compatibility boundary for debug line-run grouping.

Fields:

- `color`: RGB debug line color.
- `depthMode`: Whether the line is depth-tested or always drawn on top.
- `lineWidth`: Requested debug line width before conversion to pipeline state.

Validation rules:

- Two line primitives can share a batch only when all key fields match exactly and they are part of the same consecutive visible line run.
- Category and lifetime are not part of the key because visibility collection resolves them before batching.

## Line Segment

Represents one debug line to preserve inside a batch.

Fields:

- `start`: World-space start position.
- `end`: World-space end position.

Validation rules:

- Segment order is preserved within each batch for deterministic rendering and test behavior.
- Each segment contributes two ordered vertices to the line-list batch mesh.

## Line Batch

Represents one grouped line draw submission for a consecutive run of compatible lines.

Fields:

- `key`: Shared line batch key.
- `segments`: Ordered line segments assigned to the batch.

State transitions:

1. Created when the first visible line with a new key is encountered.
2. Appended to as additional consecutive compatible visible lines are collected.
3. Rendered once as a line-list mesh.
4. Discarded at the end of the pass execution.
