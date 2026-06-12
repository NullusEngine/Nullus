# Data Model: Editor HitProxy Picking

## HitProxyPickingSignature

Represents whether a readable picking frame is compatible with a new hover or click request.

Fields:

- `renderWidth`, `renderHeight`: Render target extent for coordinate compatibility.
- `cameraViewHash`: Stable hash of camera position, rotation, projection, and view parameters.
- `pickableSceneRevision`: Revision that changes when pickable objects are created, destroyed, activated, hidden, transformed, streamed, or have mesh/material draw identity changed.
- `pickableDrawSourceHash`: Hash of visible pickable draw-source identities, mesh pointers/revisions, state masks, and world matrices.
- `viewId`: Identity of the rendered Scene View or render surface that produced the frame.

Validation:

- Any extent mismatch invalidates the signature.
- Any view/camera mismatch invalidates click picking.
- Hover may reuse only exact signature matches in v1.

## HitProxyPickingFrame

Represents a submitted or readable picking frame.

Fields:

- `serial`: Monotonic picking frame serial.
- `signature`: `HitProxyPickingSignature`.
- `readbackTexture`: Texture used for pixel decode.
- `pickRegistry`: ID-to-object registry captured at submission.
- `submittedAtFrame`: Optional renderer frame counter for diagnostics.

Validation:

- A frame is decodable only when the async readback lifecycle marks it readable.
- Registry entries must be checked against object liveness or compatible scene revision before returning a hit.

## PickingRequest

Represents hover or click demand for picking.

Fields:

- `kind`: Hover or click.
- `renderCoordinate`: Clamped render-target coordinate.
- `minimumReadableSerial`: Serial required for click freshness.
- `requiredSignature`: Signature expected by the request.
- `allowHoverBudgetSkip`: True for hover, false for click.

Validation:

- Click requests cannot resolve against frames older than `minimumReadableSerial`.
- Requests must be cancelled on resize, view identity mismatch, object-lifetime invalidation, or camera-control cancellation.

## PickingDiagnostics

Represents per-frame picking state for FrameInfo and traces.

Fields:

- `rebuiltFrames`: Count of picking frames rebuilt this frame or aggregate interval.
- `reusedFrames`: Count of cache reuses.
- `hoverBudgetSkips`: Count of hover skips due to visible draw budget.
- `pendingReadback`: Whether click or hover is waiting for readback.
- `submittedSerial`, `readableSerial`, `clickMinimumSerial`: Separate serial fields.
- `visiblePickableDrawCount`: Last known visible pickable source count.

Validation:

- Values must be displayed as separate labeled fields, not slash-separated packed strings.
