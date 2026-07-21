# Static Mesh LOD Import UI Design

## Goal

Expose the existing static-mesh LOD import settings in the Asset Properties panel so users can configure and reimport model assets without editing `.meta` files manually.

## Scope

The model import settings panel will expose these existing serialized settings:

- `LOD_GROUP`
- `IMPORT_MESH_LODS`
- `MIN_LOD`
- `AUTO_COMPUTE_LOD_SCREEN_SIZE`

This change does not add per-LOD source-model editing, custom reduction percentages, or a mesh-preview LOD debugger.

## UI

The controls appear in `Asset Properties > Settings` for model assets, after the existing model import flags and before texture-resolution settings:

- `LOD Group`: combo box populated from `StaticMeshLODSettingsRegistry`.
- `Import Mesh LODs`: checkbox.
- `Min LOD`: non-negative integer input.
- `Auto Compute LOD Screen Size`: checkbox.

The combo box stores the preset name, not its numeric index. Unknown stored preset names are displayed and persisted as `None` so stale metadata cannot select an undefined build preset.

## Data Flow

1. Opening Asset Properties loads or creates the four keys in the model `.meta` file.
2. UI changes update the in-memory `IniFile` values through the existing widget events.
3. `Apply` rewrites the metadata through the existing `AssetProperties::Apply` path.
4. `Reimport` rewrites metadata, calls `AssetImporterFacade::SaveAndReimport`, rebuilds the LOD artifact, refreshes the Asset Browser, and refreshes Asset Properties.

No separate UI-side LOD state is introduced. The serialized metadata remains the source of truth.

## Validation

- `LOD Group` choices are derived from the runtime preset registry to prevent UI/build preset drift.
- Unknown `LOD_GROUP` values fall back to `None`.
- `MIN_LOD` is clamped to zero or greater before it is stored.
- Existing defaults remain unchanged: `None`, authored LOD import disabled, `Min LOD` zero, automatic screen-size calculation enabled.

## Test Strategy

Add focused tests for a small pure settings-view helper used by the panel:

- exposes every registered LOD group in stable order;
- maps a valid serialized group to the combo-box selection;
- falls back from an unknown group to `None`;
- preserves the three remaining LOD values;
- stores a changed selection and clamps a negative `Min LOD` to zero.

Then build `NullusUnitTests` and `Game`, and run the new UI settings tests together with the existing LOD import, thumbnail, Cook, and runtime-selection regression filter.

## Acceptance Criteria

- A model asset can configure all four LOD settings from Asset Properties.
- Apply persists the selected values to the model `.meta` file.
- Reimport consumes those values and rebuilds the correct LOD bundle.
- No manual `.meta` editing is required.
- Invalid legacy metadata cannot select an undefined LOD preset or store a negative minimum LOD.
