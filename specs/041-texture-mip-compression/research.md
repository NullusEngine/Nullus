# Research: Texture Mipmap And Compression Pipeline

## Decision 1: Use Unity-style importer controls with UE-style build backend

**Decision**: Expose high-level importer settings similar to Unity: texture type, sRGB, alpha handling, mipmap enabled, max size, compression intent, explicit format override, and per-platform override. Internally resolve those settings into immutable build settings and a platform format/encoder like UE.

**Rationale**: Nullus already has `TextureImporterSettings` with Unity-like fields in `Project/Editor/Assets/AssetImporterSettings.h`, and tests already persist platform overrides. Unity 2018.4's Texture Importer documentation exposes user-facing texture type, sRGB, mipmap, compression, and per-platform override controls; UE 4.27's `FTextureBuildSettings`, derived-data texture build path, and `ITextureFormat` model separate build identity from platform compression. The hybrid fits Nullus better than copying either engine wholesale.

**Alternatives considered**:

- Expose raw GPU formats only. Rejected because users would need to know backend-specific constraints and fallback rules.
- Hard-code BC output for Windows. Rejected because it would block Vulkan/ASTC/ETC2 and make diagnostics brittle.
- Copy UE's full LOD group and streaming model. Rejected for first scope because Nullus needs a smaller deterministic artifact path before streaming.

## Decision 2: First support claim is Windows/DX12 RGBA8, BC1, BC3, BC5, BC7

**Decision**: Implement and validate RGBA8 fallback plus RGBA16F HDR fallback and BC1, BC3, BC5, and BC7 schema/descriptor/upload-plan plumbing for Windows/DX12 only. Reserve the end-to-end BC runtime support claim until real encoder output and Windows/DX12 runtime evidence exist. Reserve BC6H, Vulkan, ASTC, ETC2, cubemaps, and streaming as schema/capability extension points.

**Rationale**: Nullus has concrete DX12 format mapping and upload code today, and `RGBA16F` already exists in `TextureFormat` for non-BC HDR preservation. Unity and UE both treat texture formats as platform-specific outputs; both keep broader format families available but gate them by target capability. BC1/3/5/7 cover the common Windows desktop LDR cases: opaque color, alpha color, normal maps, mask maps, and high-quality color.

**Alternatives considered**:

- Add BC6H for HDR in the first pass. Rejected because HDR policy and BC6H validation are separate from LDR color/normal/mask requirements.
- Add ASTC/ETC2 immediately. Rejected because no current backend validation path exists for these formats in Nullus.
- Runtime compression. Rejected because it creates load-time stalls and weakens determinism.

## Decision 2A: Use DirectXTex as the first editor/tool-time BC encoder backend

**Decision**: Use DirectXTex for the first Windows/DX12 editor/tool-time BC encoder behind the `TextureEncoder` interface, but keep the DirectXTex implementation in an editor/tool-only Windows target that is not linked by the runtime `NLS_Render` target.

**Rationale**: The first validated platform is Windows/DX12, and DirectXTex directly supports BC1, BC3, BC5, BC7 encoding plus mip-oriented texture processing used by the DirectX toolchain. It also exposes compression flags that matter for correctness: color textures must use sRGB-aware in/out handling when appropriate, while masks and normal/two-channel data must use data-preserving options such as uniform channel weighting. It is a better first dependency than writing a BC7 encoder in-house, and the interface boundary keeps future encoders such as Compressonator, ISPC Texture Compressor, ASTC Encoder, or platform SDK tools replaceable.

**Alternatives considered**:

- Write Nullus-native BC encoders. Rejected because BC7 quality/performance is non-trivial and not the value of this feature.
- Use only `stb_dxt`. Rejected because it does not cover the full first-scope BC set, especially BC5/BC7, at the required quality level.
- Use Compressonator first. Rejected for the first Windows/DX12 integration because DirectXTex is narrower and closer to the target backend/toolchain.

## Decision 3: Add shared texture format descriptors with block semantics

**Decision**: Add a shared descriptor API for every `TextureFormat`, including format family, block width/height/depth, bytes per block, alpha/HDR flags, and whether the format can be sampled or used as attachment/storage.

**Rationale**: Current helpers such as `GetTextureFormatBytesPerPixel` and `BuildDX12TextureUploadPlan` assume linear pixels. BC formats require rows and slices to be computed from block counts. Unity's importer and texture-format model expose compression as format metadata rather than ad-hoc upload math; Nullus needs the same capability as one shared RHI source of truth.

**Alternatives considered**:

- Keep bytes-per-pixel and special-case DX12. Rejected because it would duplicate format logic and break artifact validation.
- Put all descriptor data in DX12 only. Rejected because importer and artifact serialization need platform-independent pitch validation.

## Decision 3A: Add backend format capability records

**Decision**: Add explicit per-backend format capability data instead of letting the resolver infer support from enum presence. DX12 must populate sampled/upload/render-target/storage and sRGB-view viability from `D3D12_FEATURE_FORMAT_SUPPORT` or record an unavailable diagnostic.

**Rationale**: A BC format can have a `TextureFormat` and a DXGI mapping but still be unusable for a requested backend/usage. The importer must fail early and deterministically instead of discovering the problem during resource or SRV creation.

**Alternatives considered**:

- Treat all first-scope DXGI mappings as supported. Rejected because it hides unsupported device/driver paths and makes fallback non-deterministic.
- Keep capabilities only in editor code. Rejected because runtime loading also needs to reject unsupported artifacts clearly.

## Decision 4: Artifact identity includes source, settings, platform, format, and encoder version

**Decision**: `TextureBuildSettings` produces a deterministic build identity from source asset identity, source content hash, normalized importer settings, normalized platform override ordering, importer/postprocessor/dependency versions or hashes, resolved target platform, resolved format, mip policy, color-space intent, encoder name/version/options, artifact schema version, and relevant tool versions.

**Rationale**: UE's `TextureDerivedData.cpp` builds derived-data keys from source/build settings and texture format version. Nullus needs deterministic reimport and cache invalidation without making runtime loaders infer how an artifact was produced.

**Alternatives considered**:

- Use file timestamp only. Rejected because it is non-deterministic across machines and source-control operations.
- Hash artifact bytes after building only. Rejected because it cannot skip unchanged work before encoding.

## Decision 5: Normal maps use intent-aware mip generation before BC5 encoding

**Decision**: Mip generation uses texture intent. Normal maps decode vectors, average/renormalize, and then encode two-channel normal-friendly output where requested. Color maps use color-space-aware filtering; UI/cursor textures can disable mips.

**Rationale**: UE exposes normal-specific build settings such as renormalizing mips, and Unity's texture importer distinguishes normal-map usage and BC5 behavior. Treating normals as generic color produces visibly wrong normals in lower mips.

**Alternatives considered**:

- Generic box filter for all textures. Rejected because it is incorrect for normal maps and weak for alpha coverage.
- Runtime mip generation. Rejected because the feature requires deterministic offline artifacts.

## Decision 6: Compression failure falls back only at import time, not silently at runtime

**Decision**: Import can fall back from requested LDR compression to RGBA8 with diagnostics when the source, format, or target capabilities require it. HDR sources fall back to RGBA16F unless a user explicitly allows range loss. Runtime loading rejects artifacts whose format is unsupported by the selected backend.

**Rationale**: Unity import instructions record desired/compressed/uncompressed formats and warnings; UE resolves platform data before runtime. Runtime silent reinterpretation of compressed bytes would create invalid uploads.

**Alternatives considered**:

- Runtime fallback to RGBA8. Rejected because the compressed artifact lacks uncompressed pixels unless the artifact stores duplicate payloads, which is out of scope.
- Import failure for all unsupported compression. Rejected because RGBA8 fallback preserves product runtime viability during staged rollout.

## Decision 7: Color-space intent must reach runtime texture creation

**Decision**: Artifact `colorSpace` is not only metadata; `Texture2D::SetTextureResource` must propagate it into `RHITextureDesc` and/or `RHITextureViewDesc`. DX12 texture view construction must use sRGB DXGI variants for sRGB color artifacts where the selected texture format has an sRGB representation.

**Rationale**: Unity and UE both preserve sRGB intent into GPU texture sampling rather than treating it as descriptive metadata. Nullus already stores `TextureArtifactColorSpace`, while `RHITextureDesc` and `RHITextureViewDesc` currently carry only `TextureFormat`. The implementation plan must add an enforceable RHI carrier so RGBA8, BC1, BC3, and BC7 color textures cannot silently sample as linear.

**Alternatives considered**:

- Record color-space intent only in `.ntex`. Rejected because it does not satisfy runtime sampling correctness.
- Add separate `TextureFormat::RGBA8Srgb`, `TextureFormat::BC7Srgb` enum values immediately. Deferred because a color-space field on texture descriptors avoids multiplying format enum values while still allowing DX12 mapping to choose sRGB resource/view formats.
