# Data Model: Editor Scene View CPU Frame-Time Optimization

## Stable GBuffer State

Represents the renderer's currently prepared deferred Scene View render targets.

- `width`: Current usable target width.
- `height`: Current usable target height.
- `colorTextures`: Wrapped color target handles for albedo, normal, and material buffers.
- `depthTexture`: Wrapped depth target handle when available.
- `valid`: True only when dimensions are non-zero and required color attachments are available.

Validation rules:

- Stable state may be reused only when dimensions match and all required attachments remain available.
- Zero dimensions invalidate the prepared state.
- Resize or failed allocation retry must rebuild the prepared state.

## Trace Export Event

Represents a completed event written to Chrome trace JSON.

- `threadId`: Numeric timeline track index.
- `timestamp`: Event start relative to export base time.
- `duration`: Event duration.
- `name`: Event display name.

Validation rules:

- Events with missing start or end ticks are not exported.
- Events with non-positive raw tick ordering are not exported as completed duration events.
- Exported duration must be non-negative.

## Performance Evidence

Represents the validation record for this optimization.

- `baseline`: Existing trace observations and scope timings.
- `automatedTests`: Targeted test filters and results.
- `runtimeTrace`: Optional post-change trace with backend and scenario details.

Validation rules:

- Backend and platform must be named for runtime evidence.
- Automated tests must include a failing-before and passing-after record for changed behavior.
