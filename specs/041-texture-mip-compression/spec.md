# Feature Specification: Texture Mipmap And Compression Pipeline

**Feature Branch**: `041-texture-mip-compression`
**Created**: 2026-05-30
**Status**: Draft
**Input**: User description: "Design and plan a Nullus texture mipmap and compression pipeline by referencing UE 4.27 and Unity 2018.4. First implementation should focus on Windows/DX12, RGBA8 fallback, BC1/BC3/BC5/BC7 support, and extension points for Vulkan, ASTC, and ETC2."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Build Importable Textures With Mips (Priority: P1)

An artist or developer imports a texture and receives a native texture artifact that contains a deterministic mip chain, respects texture type and color-space intent, and can be loaded by the runtime without runtime mip generation.

**Why this priority**: Mipmap correctness is the base requirement for visual quality, stable sampling, and later compression. It also unifies the current split between `.ntex` artifacts and direct raw image loading.

**Independent Test**: Import representative color, normal, mask, UI, and tiny textures, then inspect the generated artifact metadata and mip payloads. This delivers immediate value even before compressed formats are enabled.

**Acceptance Scenarios**:

1. **Given** a power-of-two color texture with mipmaps enabled, **When** it is imported, **Then** the generated artifact contains levels from the base image down to `1x1`.
2. **Given** a normal map with mipmaps enabled, **When** it is imported, **Then** each generated mip remains valid for normal-map sampling rather than being treated as generic color data.
3. **Given** a UI or cursor texture with mipmaps disabled, **When** it is imported, **Then** the generated artifact contains only the base level and records that choice.
4. **Given** a texture whose source or settings have not changed, **When** it is reimported, **Then** the build identity remains stable and produces byte-identical metadata for the same target.

---

### User Story 2 - Select Platform Formats Automatically (Priority: P2)

An editor user can choose simple texture intent and compression quality settings, while the importer resolves the final platform format automatically for the active target and records any fallback.

**Why this priority**: Users should not need to manually pick low-level GPU formats for common cases. Unity's importer model shows that high-level choices such as texture type, compression mode, and platform override are easier to maintain than exposing every format first.

**Independent Test**: Configure default and Windows overrides for color, normal, mask, HDR, and UI textures, then verify the resolved format, output size, and warnings without requiring renderer execution.

**Acceptance Scenarios**:

1. **Given** an RGB color texture using default compression, **When** the Windows target is selected, **Then** the importer resolves to a BC-capable color format when supported and RGBA8 when not supported.
2. **Given** a normal map using default compression, **When** the Windows target is selected, **Then** the importer resolves to a two-channel normal-friendly compressed format when supported.
3. **Given** a texture with a per-platform override, **When** the override is active, **Then** the override takes precedence if supported and emits a clear diagnostic if it must fall back.
4. **Given** a texture whose top-level size violates the selected backend's compressed-texture alignment capability, **When** compression is requested, **Then** the importer selects a valid fallback or policy-driven padded encoding and reports why.

---

### User Story 3 - Load Compressed Artifacts Through RHI (Priority: P3)

The runtime can load texture artifacts whose mip payloads are already compressed for the selected backend and upload them without reinterpretation or per-pixel assumptions.

**Why this priority**: Compression only becomes useful once the runtime can describe block-compressed formats, calculate upload pitches, create backend-native textures, and create views safely.

**Independent Test**: Load generated RGBA8 and BC artifacts through the runtime texture loader on the validated backend, then inspect texture descriptors and upload plans with unit tests and focused runtime verification.

**Acceptance Scenarios**:

1. **Given** a compressed `.ntex` artifact with multiple mips, **When** the runtime creates a texture, **Then** the RHI descriptor records the correct compressed format and mip count.
2. **Given** a compressed format with block dimensions, **When** the upload plan is built, **Then** row pitch and slice pitch are computed from block counts instead of bytes per pixel.
3. **Given** a backend without support for the artifact format, **When** the texture is requested, **Then** the runtime reports an explicit unsupported-format diagnostic instead of silently creating an invalid texture.

---

### User Story 4 - Preserve Extension Paths For Later Platforms (Priority: P4)

Engine maintainers can add Vulkan, ASTC, ETC2, cubemap, and streaming behavior later without replacing the user-facing importer settings. The first schema must already carry subresource addressing so cubemaps and arrays do not require a structural rewrite.

**Why this priority**: The first implementation is intentionally scoped to Windows/DX12, but the design must not close the door on Nullus' multi-backend renderer direction.

**Independent Test**: Review artifact metadata, subresource records, format descriptors, and format-selection rules to confirm new platform families can be added as data and backend capability entries.

**Acceptance Scenarios**:

1. **Given** a future Vulkan backend, **When** it declares support for a texture format family, **Then** the importer can resolve that target without changing user-facing texture settings.
2. **Given** a future ASTC or ETC2 encoder, **When** it is registered, **Then** it can use the same build settings and artifact metadata path as the first BC implementation.

### Edge Cases

- Non-power-of-two textures with mipmaps and compression requested must either be converted according to the selected NPOT policy or fall back with an explicit diagnostic.
- Textures whose top-level dimensions are unsupported by the selected backend's compressed-texture alignment rules must fall back or use an explicitly configured padded-encoding policy with diagnostics.
- Source alpha must be handled consistently across color, mask, sprite/UI, and normal-map usages.
- HDR sources must not be forced into LDR BC formats that discard required range.
- Corrupt or mismatched artifact metadata must fail loading with a diagnostic rather than attempting an unsafe upload.
- The importer must preserve runtime viability for existing RGBA8 artifacts during staged rollout.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The importer MUST support a deterministic native texture artifact containing texture dimensions, array-layer count, format, color-space intent, mip count, subresource identity, per-subresource dimensions, per-subresource pitches, and payload bytes.
- **FR-002**: The importer MUST generate mipmaps offline when mipmaps are enabled and MUST record single-mip output when mipmaps are disabled.
- **FR-003**: The mipmap generator MUST distinguish at least color, normal, mask, UI, and HDR texture intent.
- **FR-004**: The importer MUST support high-level compression choices: uncompressed, low quality, normal quality, high quality, and explicit format override.
- **FR-005**: The importer MUST support default texture settings and per-platform override settings for max size, resize policy, requested format, compression quality, and mip behavior.
- **FR-006**: The platform format resolver MUST select valid output formats based on texture intent, alpha needs, HDR status, compression choice, platform capability, and override validity.
- **FR-007**: The first implementation MUST validate Windows/DX12 support for RGBA8 fallback, RGBA16F HDR fallback, plus BC1, BC3, BC5, and BC7 artifacts.
- **FR-008**: The design MUST preserve extension points for Vulkan, ASTC, ETC2, cubemap mipmaps, and streaming mips without requiring a new user-facing settings model; artifact subresources MUST include layer/face/mip identity even though first delivery writes only 2D layer 0.
- **FR-009**: Runtime texture loading MUST create textures from artifact metadata rather than assuming RGBA8 bytes-per-pixel layout.
- **FR-010**: Runtime upload planning MUST calculate block-compressed pitches and data sizes correctly for every artifact mip.
- **FR-011**: Unsupported formats, invalid dimensions, invalid mip chains, and malformed artifact payloads MUST produce actionable diagnostics.
- **FR-012**: Existing uncompressed `.ntex` textures and direct texture loading behavior MUST remain usable during staged migration.
- **FR-013**: The implementation MUST include targeted automated coverage for artifact serialization, mip generation, format selection, RHI format metadata, and DX12 upload planning.
- **FR-014**: Final validation MUST include backend-specific evidence for every backend claimed as supported.
- **FR-015**: sRGB/linear sampling intent MUST be carried through artifact metadata into `RHITextureDesc` or `RHITextureViewDesc`, and DX12 SRV/resource format selection MUST be tested for RGBA8, BC1, BC3, and BC7.
- **FR-016**: Backend format capability MUST be modeled per format and usage, including DX12 `D3D12_FEATURE_FORMAT_SUPPORT` evidence for first-scope formats before the resolver selects BC output.
- **FR-017**: Build identity MUST include source GUID/path identity, source content hash, normalized importer settings and override ordering, importer/postprocessor/dependency versions or hashes, target platform, resolved format, mip policy, color-space intent, encoder id/version/options, DirectXTex version for BC outputs, and artifact schema version.
- **FR-018**: DirectXTex MUST remain a Windows editor/tool-time encoder implementation detail and MUST NOT become a required runtime `NLS_Render` dependency.

### Key Entities

- **Texture Importer Settings**: User-facing settings for texture type, color space, alpha handling, mipmap generation, compression preference, filtering, wrapping, max size, and platform overrides.
- **Texture Build Settings**: Resolved build identity used to generate deterministic artifacts, including source identity, target platform, output format, mip behavior, and encoder settings.
- **Texture Format Descriptor**: Shared description of a runtime format, including compression family, block dimensions, bytes per block, alpha/HDR capability, sRGB-view support, and backend support.
- **Texture Backend Format Capability**: Per-backend, per-format support record for sampled/upload/render-target/storage use, sRGB view support, and alignment constraints.
- **Texture Color-Space Intent**: Runtime descriptor/view field that controls whether color textures use linear or sRGB sampling variants.
- **Texture Artifact**: Native serialized texture payload containing format metadata and one or more mip payloads ready for runtime upload.
- **Texture Encoder**: Build-time component that transforms processed image mips into a final artifact format.
- **Texture Load Diagnostic**: Structured report describing import, artifact, format, or runtime upload problems.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: At least 95% of existing valid texture imports continue to load successfully after the artifact schema update.
- **SC-002**: A 1024x1024 color texture imported with mipmaps enabled produces 11 mip levels, ending at `1x1`.
- **SC-003**: Windows/DX12 RGBA8, RGBA16F, BC1, BC3, BC5, and BC7 artifact metadata pass format-selection and upload-plan tests.
- **SC-004**: Reimporting an unchanged source texture with unchanged settings produces the same build identity in 100% of tested cases.
- **SC-005**: Invalid compressed-dimension capability cases and unsupported backend-format cases produce diagnostics that name the source texture, requested format, and fallback or failure reason.
- **SC-006**: Runtime upload planning tests cover RGBA8, RGBA16F, one 4-bit BC format, one 8-bit BC format, a single-mip texture, and a multi-mip texture.
- **SC-007**: Final implementation evidence distinguishes validated backend support from reserved extension points.

## Assumptions

- First delivery targets Windows/DX12 because Nullus currently has concrete DX12 texture creation and upload code to extend.
- Vulkan, ASTC, ETC2, virtual texturing, texture streaming, and runtime render-target mip generation are out of scope for the first implementation, but the design must leave explicit extension points.
- BC compression can be supplied by a third-party encoder or platform tool selected during planning; the user-facing design must not depend on one encoder brand.
- Raw image loading should migrate toward the artifact path, but compatibility behavior can remain while the importer and runtime are staged.
- Generated files under `Runtime/*/Gen/` are not edited by hand.
- Rendering correctness claims require backend-specific validation; Windows/DX12 evidence does not prove Vulkan or future backend behavior.
