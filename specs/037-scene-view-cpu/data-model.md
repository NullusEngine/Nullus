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

## Selection Outline Draw Item

Represents one selected renderable used by the editor outline path.

- `mesh`: Render mesh or editor icon mesh to draw into the mask.
- `worldMatrix`: World transform used for the mask draw.
- `worldScale`: World scale used only by the legacy shell fallback.
- `selectionGroupId`: Stable non-zero identifier for the selected root, wrapping only when the mask format requires it.
- `classification`: Parent/root selection or selected child outline category.
- `visibilityMode`: Draw visible channel, occluded channel, or both.
- `sourceComponentId`: Optional source component identity used to map collected selected-tree items to a stable selection group without re-traversing the scene.
- `renderEligibility`: Current-frame eligibility result derived from active state, enabled renderer/component state, editor visibility, Scene View culling/layer checks, and available mesh/material data.
- `maskMaterialMode`: Material selection mode for mask capture: material selection pass, alpha-clipped default mask, or opaque default mask.
- `alphaClipParameters`: Optional main texture, texture transform, and cutoff values copied from the selected material when the default mask shader must preserve cutout shape.

Validation rules:

- Items are derived from the selected-tree collector, not from a second recursive traversal.
- Items with missing mesh or inactive owner are skipped.
- Camera icon outline behavior remains represented as an outline item.
- Parent/root and child classification is assigned before pass emission so shader constants and mask channels do not require late scene queries.
- Items that would not be part of the current Scene View renderable set are filtered before mask capture.
- Alpha-clipped materials must not silently fall back to opaque whole-mesh masks when their cutout inputs are available.

## Selection Outline Mask Channel Layout

Represents the shader-visible meaning of the mask texture channels.

- `groupId`: Encoded non-zero selection-root identifier used by the ID-edge pass.
- `visible`: Whether the selected object passed scene-depth testing.
- `selected`: Coverage used by the fused composite shader to detect outline distance.
- `classification`: Encoded parent/root versus selected-child value used by composite color selection.
- `occluded`: Coverage written by the occluded contribution without destroying `groupId` or `visible`.

Validation rules:

- Channel meanings are declared through one cross-language source of truth, such as a simple `.def` table consumed by both C++ constants and `SelectionOutlineMask.hlsl`, or by a generated/checked pair with tests proving the names and swizzles match.
- Group ID wrapping is deterministic and keeps a visible outline even when unique edge separation is approximate.
- Visible and occluded coverage are preserved separately enough for the composite pass to fade occluded selection on small selections; large selections must at least preserve selected coverage so hidden selected pixels do not disappear, while visible-vs-occluded refinement may be approximate to bound CPU command preparation.
- Tests must fail if C++ channel constants and HLSL channel macros drift.

## Selection Outline Mask Resources

Represents the reusable offscreen textures used by the screen-space outline algorithm.

- `width`: Current mask width.
- `height`: Current mask height.
- `maskFramebuffer`: Color mask target that stores selected visibility, occlusion, and group/classification channels.
- `maskFramebuffer`: Intermediate target written by selected-mask capture and sampled by composite.
- `sceneDepthView`: Read-only scene depth view used by mask capture.
- `sceneDepthIdentity`: Current-frame depth texture/view descriptor, format, extent, sample count, subresource range, and frame/output identity used to reject stale depth reuse.
- `outputColorView`: Scene View output target written by the final composite pass.
- `outputColorIdentity`: Current-frame output color target identity used to reject stale composite attachments.
- `colorFormat`: Mask color format selected for channel precision and backend support.
- `sampleCount`: Output color sample count and scene depth sample count used to prove mask intermediates are compatible. The current screen-space path is valid only when output and depth are both single-sample; MSAA or output/depth mismatches produce an `UnsupportedSampleCount` skip-frame decision.
- `valid`: True only when dimensions are non-zero and all required views exist.

Validation rules:

- Stable-size frames reuse resources.
- Resize, zero-size frames, allocation failure, missing views, format changes, sample-count changes, depth identity changes, output identity changes, or mismatched view descriptors invalidate the resources.
- Invalid resources trigger a structured fallback decision. Startup/compatibility gaps such as missing scene depth or missing shader use the legacy shell path when that path is attachment-compatible; transient runtime failures and unsupported sample counts skip the current outline frame instead of re-enabling the expensive or MSAA-unsafe shell path.
- The mask path must not attach stale scene depth when mask resources or depth views do not match the current frame.
- Fused composite must not read and write the same texture subresource.

## Selection Outline Mask Contribution

Represents the two mesh-draw contributions used to build the selected-object mask.

- `selectedCoverageContribution`: Uses an occluded or always-depth mode, writes group/selected/classification coverage, and is retained for all valid selection sizes so hidden selected pixels remain represented.
- `visibleContribution`: Uses the current scene depth test, writes group/visible/selected/classification channels, disables depth and stencil writes, and is emitted only while the selected item count stays under the bounded visible-refinement threshold.
- `materialSelectionPass`: Optional material-specific selection pass equivalent to Unity's `SceneSelectionPass`.
- `alphaClipFallback`: Future default mask shader path for copying available alpha texture/cutoff parameters when no material-specific selection pass exists; current unsupported inputs are reported through `UnsupportedMaterialMask`.

Validation rules:

- Read-only scene depth requires mask PSOs to disable depth writes and stencil writes.
- Occluded contribution must not overwrite object ID, visible, or classification channels needed by ID-edge and composite passes.
- Alpha-clipped material inputs are either honored or recorded as an explicit unsupported material fallback.

## Selection Outline Screen-Space Pass

Represents one helper pass in the mask/composite pipeline.

- `kind`: Mask or composite.
- `inputTextures`: Textures sampled by the pass.
- `outputAttachment`: Render target written by the pass.
- `requiresSceneDepth`: True for mask passes that use the scene depth attachment.
- `drawCount`: Number of mesh or full-screen draw commands.
- `profilerScope`: Exportable scope name such as `SelectionOutlineMask::CaptureMask` or `SelectionOutlineMask::Composite`.
- `fallbackAllowed`: True only for the top-level selection outline request, not for partially built mask/composite pass chains.

Validation rules:

- Mask passes may contain selected mesh draw commands.
- Composite is a bounded full-screen pass and performs edge detection/softening directly from the mask.
- Texture access declarations and queue dependencies must match the read/write order.
- The primary threaded path emits no legacy inflated-shell pass when all screen-space passes are valid.
- The expected resource matrix is:
  - Mask capture reads scene depth as read-only depth/stencil and writes mask color as a color attachment.
  - Composite reads mask, performs edge detection/softening inline, and writes Scene View output color as a color attachment.
- Validation must inspect real pass inputs or RHI/RenderDoc evidence for write-to-read transitions, not just pass names.

## Selection Outline Prepared Output

Represents the threaded output of the screen-space outline path.

- `passInputs`: Ordered vector of helper pass inputs for mask and composite.
- `metadata`: Matching framegraph metadata entries whose graph pass names, command kinds, and execution order match `passInputs`.
- `fallbackDecision`: Empty when screen-space output is valid; populated when the outline request must either use a compatible legacy shell path or skip the current outline frame.
- `selectedItemCount`: Count of selected mask items used for trace comparability.

Validation rules:

- Screen-space outline preparation returns a vector-like output; it must not pack all work into one `RenderPassCommandInput`.
- Appended helper pass assembly, metadata counts, consumption, and duplicate-name handling must support all emitted selection-outline passes.
- Valid resources produce no fallback decision and no legacy inflated-shell pass.

## Selection Outline Fallback Decision

Represents why the renderer could not use the screen-space path and which fallback action is allowed.

- `reason`: Missing resources, unsupported backend feature, missing shader/material, missing scene depth, zero-size target, or explicit disabled flag.
- `selectedItemCount`: Number of selected outline items at the time of fallback.
- `reported`: Whether the reason was made visible to validation logs, profiler/debug markers, or test-access state.
- `injectedFailure`: Optional deterministic failure mode used by tests to simulate allocation failure, missing views, stale depth, unsupported material mask semantics, or unsupported backend support.

Validation rules:

- Fallback must preserve visible selection feedback.
- Fallback must be observable in tests or validation notes so a silent permanent fallback cannot masquerade as the optimized path.
- Tests must be able to assert the last structured fallback reason without depending on nondeterministic GPU allocation failure.

## Editor Threaded Publication Backpressure

Represents the editor-only frame-slot cushion used when Scene View selection helpers and large asset creation both need prepared object-data storage.

- `threadedFrameSlotCount`: Editor-resolved slot count, equal to frames-in-flight plus one cushion slot.
- `publishRetirementWaitMs`: Bounded wait used when reserving a reusable prepared object-data slot.
- `reservedSlotIndex`: Optional retired or available threaded slot reserved for the prepared publication path.
- `backpressureOutcome`: Either a reusable slot was reserved, the publication proceeds without waiting, or the frame explicitly skips prepared helper publication without submitting an empty main-scene snapshot.

Validation rules:

- The default zero-wait lifecycle behavior remains available for non-editor callers.
- Waiting must be bounded and must wake on `RetireFrame` or retained-resource release notifications.
- A reserved slot is consumed only by prepared publication or explicitly released before snapshot publication can reuse it.
- Runtime validation must verify that large selected-tree creation no longer logs `Skipping threaded deferred capture` or `recordedDraws=0` while `sceneOpaqueDrawables` is non-zero.
