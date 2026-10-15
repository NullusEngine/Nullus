# LOD and HLOD

LOD represents the same mesh at different detail levels; HLOD represents a group of objects with a proxy. Both trade detail for cost, differing in granularity.

## Static mesh LODs

Static mesh LODs are built during import and the runtime only selects a level:

| Stage | Work |
| --- | --- |
| Import | Generate per-level mesh data according to import settings and pack artifacts |
| Runtime | Select a level from view distance and screen size |

Key import fields:

| Field | Default | Notes |
| --- | --- | --- |
| `lodGroup` | `None` | LOD group preset |
| `minLOD` | `0` | Lowest level allowed |
| `autoComputeLODScreenSize` | `true` | Whether to derive per-level screen size thresholds |
| `sourceModels` | One per LOD | Source kind, screen size, and mesh data per level |

Source kind distinguishes `Imported`, `Authored`, and `Generated`. When levels come from a simplification flow, the triangle percentage and pixel error come from the LOD group preset.

LOD group presets unify switching thresholds across meshes: every mesh in the group switches at the same distance, so neighboring objects do not pop at inconsistent rates.

## HLOD

HLOD replaces a group of objects with a proxy at distance. Implementation requirements:

- Decisions stay view local and never write shared state.
- HLOD children selected in the editor must remain inspectable; being proxied must not remove them from manipulation.
- Transparent, order-dependent, skinned, and animated children do not participate in suppression until explicit proxy compatibility exists.

Those two rules define the usable range: static opaque subsets can enter early; the rest needs additional compatibility work.

## Relationship to visibility

The visibility pipeline narrows the candidate set first, and LOD/HLOD then decides which representation each candidate uses. The order cannot be inverted: doing LOD before culling spends work on objects that end up invisible.

## Relationship to residency

HLOD proxies and lower-level meshes are separate artifacts that participate in residency accounting and budgets. Confirm the budget model covers proxy meshes before introducing HLOD into a scene, otherwise distant views introduce a new load spike. See [Residency and Budgets](streaming.md).

## Validation focus

- LOD selection matches across serial, parallel, and full-scan modes
- No level thrashing while the camera moves quickly
- HLOD suppression never hides children the editor needs to manipulate
- Proxied objects are counted at the lower level in the residency system

## Next steps

- [Residency and Budgets](streaming.md) keeps streaming from introducing spikes.
- [Models, Meshes, and Textures](../assets/models-textures.md) covers the import-side LOD settings.
