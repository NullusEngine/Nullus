# Contract: Connected Generated-Child Geometry

## Segmentation

- Only equal nonzero group IDs are continuous.
- Grid continuity ends at row boundaries.
- Each grid row contains maximal same-group segments spanning full allocated bounds and internal gaps; row start/end are true segment outer edges even when the group continues on another row, so they receive the grid segment's outer rounding.
- Each visible list group intersection is one segment with true start/end metadata.

## Drawing

- Draw one fill shape per segment below item-local hover/selection overlays.
- Round grid row-segment outer edges. In list mode, round only actual group start/end; viewport clipping does not manufacture an outer edge, so clipped continuation edges remain square.
- Intersect with the current content clip; never replace it with the window clip.
- Source cards/rows are outside generated-child fills.

## Raster Verification

The deterministic test rasterizer honors display transforms, `IdxOffset`, `ElemCount`, `VtxOffset`, clip rectangles, packed vertex alpha, top-left triangle coverage, and source-over composition. It rejects callbacks and invalid geometry. Pixel checks cover internal gaps, rounded edges, and absence outside the parent clip.
