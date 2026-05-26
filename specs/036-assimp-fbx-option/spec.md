# Feature Specification: Optional Assimp FBX Import

**Feature Branch**: `036-assimp-fbx-option`
**Created**: 2026-05-25
**Status**: Draft
**Input**: User description: "Add Assimp FBX reading support as an optional path. Keep Autodesk FBX SDK as the default, add a per-FBX asset setting that can choose Assimp, and support controlled fallback to Assimp when the SDK is unavailable or parsing fails."

## User Scenarios & Testing

### User Story 1 - Preserve Existing FBX Imports By Default (Priority: P1)

Existing projects and built-in FBX assets continue using the current Autodesk FBX SDK import path unless a user explicitly selects another FBX reader.

**Why this priority**: FBX import feeds generated model prefabs, mesh artifacts, material conversion, and built-in primitive resources. The new option must not silently change existing asset output.

**Independent Test**: Can be tested by importing an FBX asset with no new setting and verifying the selected reader, diagnostics, artifact importer metadata, and generated assets match the default Autodesk path.

**Acceptance Scenarios**:

1. **Given** an FBX asset with no reader setting, **When** the editor imports it, **Then** the Autodesk FBX SDK reader is selected first.
2. **Given** an existing FBX meta file created before this feature, **When** the asset is reimported, **Then** the import behavior remains unchanged unless the user changes the reader setting.
3. **Given** built-in primitive FBX resources, **When** mesh artifacts are generated, **Then** they continue using the Autodesk FBX SDK reader by default.

---

### User Story 2 - Choose Assimp For A Specific FBX Asset (Priority: P1)

An asset author can select Assimp as the FBX reader for a particular `.fbx` source when they need Assimp compatibility or want to avoid the Autodesk SDK dependency for that asset.

**Why this priority**: The requested feature is an optional Assimp FBX read path, and per-asset control avoids applying a risky parser change globally.

**Independent Test**: Can be tested by setting one FBX asset to the Assimp reader, importing it, and verifying the resulting model scene, mesh artifacts, material channels, texture dependencies, and diagnostics come from the Assimp path.

**Acceptance Scenarios**:

1. **Given** an FBX asset whose reader is set to Assimp, **When** it is imported, **Then** the system reads the FBX through Assimp and writes the normal generated artifacts.
2. **Given** one FBX asset set to Assimp and another left at default, **When** both are imported, **Then** only the explicitly configured asset uses Assimp.
3. **Given** Assimp FBX support is not available in the current build, **When** an asset requests Assimp, **Then** import fails with an actionable diagnostic instead of silently using another reader.

---

### User Story 3 - Controlled Fallback When Autodesk FBX Import Is Unavailable Or Fails (Priority: P2)

A project can allow an FBX asset to try Autodesk first and then fall back to Assimp if the Autodesk SDK is unavailable or cannot parse the file.

**Why this priority**: Fresh clones and non-standard FBX files should have a controlled recovery path, but fallback must be visible so users can make an informed compatibility decision.

**Independent Test**: Can be tested by enabling fallback on an FBX asset, forcing the Autodesk path to be unavailable or fail, and verifying Assimp imports the asset while the diagnostics record the fallback event.

**Acceptance Scenarios**:

1. **Given** fallback is enabled for an FBX asset and Autodesk FBX import is unavailable, **When** the asset is imported, **Then** Assimp is attempted and a warning records the fallback reason.
2. **Given** fallback is enabled and Autodesk FBX import fails to parse the source, **When** Assimp succeeds, **Then** the import succeeds with a warning that identifies both the failed primary reader and the successful fallback reader.
3. **Given** fallback is disabled and Autodesk FBX import fails, **When** the asset is imported, **Then** the import fails without attempting Assimp.

### Edge Cases

- An asset requests Assimp but the build does not include Assimp FBX support.
- An asset enables fallback but both Autodesk and Assimp fail to parse the same FBX.
- An FBX imports through Assimp but exposes less scene detail than the Autodesk path.
- A default/legacy asset has no reader setting at all.
- A model import setting is edited and saved before the source asset exists or before reimport runs.
- Built-in FBX mesh cache generation runs outside the editor asset database path.
- A full Assimp-format compatibility build is enabled while the explicit Assimp FBX option is also enabled.

## Requirements

### Functional Requirements

- **FR-001**: The system MUST keep Autodesk FBX SDK as the default reader for `.fbx` assets with no explicit reader setting.
- **FR-002**: Users MUST be able to select an FBX reader for each FBX asset from at least "Autodesk FBX SDK", "Assimp", and "Autodesk FBX SDK with Assimp fallback".
- **FR-003**: The selected FBX reader setting MUST persist with the asset import settings and participate in reimport freshness so changing it queues or requires reimport.
- **FR-004**: When an asset selects Assimp, the import pipeline MUST attempt Assimp for both scene conversion and native mesh artifact generation.
- **FR-005**: When an asset selects Autodesk with Assimp fallback, the import pipeline MUST attempt Autodesk first and MUST attempt Assimp only after Autodesk is unavailable or fails.
- **FR-006**: When fallback to Assimp occurs, the import diagnostics MUST include a warning that identifies the primary reader, fallback reader, and reason for fallback.
- **FR-007**: When Assimp FBX support is unavailable but the asset requires it, the import diagnostics MUST include an error explaining that Assimp FBX support is not enabled in the current build.
- **FR-008**: The build configuration MUST expose a narrowly scoped option for enabling Assimp FBX import without requiring all Assimp importers/exporters.
- **FR-009**: Enabling the narrow Assimp FBX option MUST not enable Assimp exporters or unrelated import formats by default.
- **FR-010**: The existing "build all Assimp formats" compatibility mode MUST remain available and MUST continue to include FBX when full Assimp coverage is requested.
- **FR-011**: Built-in FBX mesh artifact generation MUST keep the Autodesk default and MUST have a controlled code path for Assimp only when explicitly requested by available metadata or a caller-provided choice.
- **FR-012**: The feature MUST include targeted regression coverage for default reader preservation, explicit Assimp selection, fallback diagnostics, unavailable Assimp diagnostics, and build-option behavior.

### Key Entities

- **FBX Reader Selection**: Asset import setting that records whether a given FBX source uses Autodesk, Assimp, or Autodesk with Assimp fallback.
- **FBX Reader Availability**: Build/runtime capability state that says whether each reader can be attempted in the current build.
- **FBX Reader Attempt Result**: Import outcome for a single reader, including success or failure diagnostics and any generated scene or mesh data.
- **Fallback Diagnostic**: Warning emitted when an asset succeeds through Assimp after Autodesk was unavailable or failed.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Existing FBX assets with no new setting keep the Autodesk reader in targeted regression tests.
- **SC-002**: A targeted regression test imports an FBX asset through explicit Assimp selection when Assimp FBX support is enabled.
- **SC-003**: A targeted regression test records a fallback warning when Autodesk fails or is unavailable and Assimp succeeds.
- **SC-004**: A targeted regression test records an actionable error when an asset requires Assimp FBX support but the build lacks it.
- **SC-005**: Build-option contract tests confirm the narrow Assimp FBX option enables only FBX import support while preserving existing export and full-format behavior.
- **SC-006**: Relevant unit tests for asset import settings, FBX import routing, and existing Autodesk FBX contracts pass after implementation.

## Assumptions

- The default reader remains Autodesk FBX SDK because current FBX-specific tests and built-in helper resources are calibrated around that path.
- Assimp FBX support is intended as a compatibility option, not a replacement for the existing Autodesk parser in this change.
- Automatic fallback is asset-controlled rather than global, so teams can decide when parser differences are acceptable.
- Full parity for skeletons, animation, morph targets, cameras, and lights is not required for the first optional Assimp path; missing data must be represented by diagnostics or existing scene conversion limits.
- This change affects model import and build configuration only; it does not change rendering backend behavior or generated file ownership.
