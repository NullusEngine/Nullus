# Research: Optional Assimp FBX Import

## Decision: Add A Narrow Assimp FBX Build Option

Use a new Nullus-owned option such as `NLS_ENABLE_ASSIMP_FBX_IMPORTER` to enable `ASSIMP_BUILD_FBX_IMPORTER` when `NLS_ASSIMP_BUILD_ALL_FORMATS` is off.

**Rationale**: Current Nullus minimal Assimp configuration builds only the importers already used by the engine and disables exporters. A narrow option satisfies the feature request without turning on all upstream Assimp formats or exporters.

**Alternatives considered**:

- Reuse `NLS_ASSIMP_BUILD_ALL_FORMATS`: rejected because it enables far more than FBX and changes build cost/coverage.
- Always enable Assimp FBX: rejected because existing default policy intentionally routes FBX through Autodesk and keeps minimal Assimp build time low.

## Decision: Persist Reader Choice In Model Import Settings

Add an FBX reader selection to model importer settings with values for Autodesk, Assimp, and Autodesk with Assimp fallback.

**Rationale**: Reader choice is asset-specific compatibility behavior. Storing it beside model import settings lets the editor mark the importer dirty and reimport when changed.

**Alternatives considered**:

- Global project setting: rejected because one problematic FBX should not change every asset.
- Separate importer IDs per reader: rejected because the source asset type and generated artifact shape stay the same; the setting can participate in importer dependencies without multiplying importer descriptors.

## Decision: Preserve Autodesk Default

Assets with no setting continue to use Autodesk FBX SDK first, including built-in primitive mesh cache generation.

**Rationale**: Existing FBX SDK tests, helper models, and default assets are calibrated around Autodesk behavior. The safest feature shape is opt-in Assimp.

**Alternatives considered**:

- Prefer Assimp when both readers are available: rejected as a silent behavioral change.
- Prefer Assimp when Autodesk SDK is missing without metadata opt-in: rejected because fallback may produce different scene hierarchy or material output.

## Decision: Explicit Fallback Diagnostics

When fallback from Autodesk to Assimp occurs, emit a warning with the primary reader, fallback reader, and fallback reason. When Assimp is requested but unavailable, emit an error.

**Rationale**: Parser differences matter for generated prefabs and material conversion. Users need visible evidence when output came from the fallback reader.

**Alternatives considered**:

- Silent fallback: rejected because it hides compatibility differences.
- Treat fallback as failure even when Assimp succeeds: rejected because the selected mode is explicitly intended to recover.

## Decision: Use Existing Parser Boundaries

Reuse `FbxSdkParser` and `AssimpParser` through `LoadModelData` and `PopulateImportedSceneData`. Keep scene generation and artifact writing unchanged after parser output is selected.

**Rationale**: Both parsers already expose the same parsed mesh and imported-scene provider interfaces. Reusing these boundaries keeps the change local to reader selection.

**Alternatives considered**:

- Introduce a new model importer subsystem: rejected as too broad for a controlled optional reader.
- Fork artifact writing per reader: rejected because generated artifacts should remain a shared pipeline concern.
