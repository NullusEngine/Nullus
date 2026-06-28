# Unity-Aligned Asset Reuse Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Model imports reuse existing project texture assets through Unity-style asset identity/remaps instead of generating duplicate model-owned texture sub-assets.

**Architecture:** Add focused resolver and report units under `Project/Editor/Assets`, then integrate them into `ExternalAssetImporter.cpp`. Keep phase-one material artifacts path-compatible, store identity/remap data in model metadata and manifest dependencies, and show the last committed resolution report in `AssetProperties`.

**Tech Stack:** C++20, Nullus editor asset pipeline, `.meta` sidecar settings, artifact manifests/database, existing material conversion XML, GoogleTest/`NullusUnitTests`.

---

## File Map

- `Project/Editor/Assets/ModelTextureReferenceResolver.h/.cpp`: Owns `mtxsrc:v1` source key derivation, material texture key preservation, remap validation, path/name lookup, dependency descriptors, and fallback decisions.
- `Project/Editor/Assets/ModelTextureResolutionReport.h/.cpp`: Owns versioned report serialization/parsing, stale-context validation, committed report path helpers, and Asset Properties display rows.
- `Project/Editor/Assets/AssetImporterSettings.h/.cpp`: Adds model texture resolution setting defaults and remap key/value helpers.
- `Project/Editor/Assets/ExternalAssetImporter.cpp`: Orchestrates resolver in model import, filters texture payloads, records dependency freshness, preserves material path compatibility, and atomically writes the report on successful commit.
- `Runtime/Rendering/Assets/MaterialConversion.h/.cpp`: Should need minimal or no schema change; verify conversion receives resolver paths through existing context.
- `Project/Editor/Panels/AssetProperties.h/.cpp`: Adds settings controls, resolution rows, and remap edit/clear controls.
- `Tests/Unit/AssetImportPipelineTests.cpp`: Main test file for settings, stable keys, resolver, reports, importer integration, remap behavior, dependency freshness, legacy reimport, and settings effects.
- `specs/052-unity-aligned-asset-reuse/quickstart.md`: Final manual validation notes.

---

### Task 1: Create Focused Resolver and Report Skeletons

**Files:**
- Create: `Project/Editor/Assets/ModelTextureReferenceResolver.h`
- Create: `Project/Editor/Assets/ModelTextureReferenceResolver.cpp`
- Create: `Project/Editor/Assets/ModelTextureResolutionReport.h`
- Create: `Project/Editor/Assets/ModelTextureResolutionReport.cpp`
- Inspect: `Project/Editor/CMakeLists.txt`

- [ ] **Step 1: Add empty but compiling resolver types**

Add the header with these initial public types:

```cpp
#pragma once

#include "Assets/AssetId.h"

#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
enum class ModelTextureResolutionKind
{
    ExplicitRemap,
    SourcePath,
    NameSearch,
    ModelEmbeddedFallback,
    Missing,
    Invalid
};

enum class TextureSourceKind
{
    ExternalFile,
    EmbeddedData,
    BufferView,
    Missing
};

struct ModelTextureDiagnostic
{
    std::string severity;
    std::string code;
    std::string message;
};

struct ModelTextureSourceReference
{
    std::string sourceKey;
    std::string materialTextureKey;
    std::string stableKey;
    std::string displayName;
    std::string uri;
    std::string normalizedUri;
    std::string bufferViewKey;
    std::string embeddedIndex;
    std::string stableDiscriminator;
    TextureSourceKind kind = TextureSourceKind::Missing;
    std::string stableKeyStatus = "Stable";
    bool hasModelLocalPayload = false;
};

struct ResolvedModelTextureReference
{
    ModelTextureSourceReference source;
    ModelTextureResolutionKind kind = ModelTextureResolutionKind::Missing;
    std::string materialTextureKey;
    NLS::Core::Assets::AssetId targetAssetId;
    std::string targetSubAssetKey;
    std::filesystem::path resourcePath;
    std::string modelSubAssetKey;
    std::vector<ModelTextureDiagnostic> diagnostics;
};

std::string MakeModelTextureStableKey(const ModelTextureSourceReference& source);
const char* ToString(ModelTextureResolutionKind kind);
const char* ToString(TextureSourceKind kind);
}
```

- [ ] **Step 2: Add minimal resolver implementation**

```cpp
#include "Assets/ModelTextureReferenceResolver.h"

namespace NLS::Editor::Assets
{
namespace
{
std::string PercentEncodeModelTextureToken(const std::string& value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char byte : value)
    {
        if (byte == '%' || byte == '=' || byte == '#' || byte == ';' || byte == '|' ||
            byte == '\r' || byte == '\n' || byte >= 0x80u)
        {
            encoded.push_back('%');
            encoded.push_back(hex[(byte >> 4u) & 0x0Fu]);
            encoded.push_back(hex[byte & 0x0Fu]);
        }
        else
        {
            encoded.push_back(static_cast<char>(byte));
        }
    }
    return encoded;
}
}

std::string MakeModelTextureStableKey(const ModelTextureSourceReference& source)
{
    std::string key = "mtxsrc:v1:kind=";
    key += ToString(source.kind);
    if (!source.sourceKey.empty())
        key += ";source=" + PercentEncodeModelTextureToken(source.sourceKey);
    if (!source.normalizedUri.empty())
        key += ";uri=" + PercentEncodeModelTextureToken(source.normalizedUri);
    if (!source.bufferViewKey.empty())
        key += ";bufferView=" + PercentEncodeModelTextureToken(source.bufferViewKey);
    if (!source.embeddedIndex.empty())
        key += ";embedded=" + PercentEncodeModelTextureToken(source.embeddedIndex);
    if (!source.stableDiscriminator.empty())
        key += ";discriminator=" + PercentEncodeModelTextureToken(source.stableDiscriminator);
    const bool hasStableIdentity =
        !source.sourceKey.empty() ||
        !source.normalizedUri.empty() ||
        !source.bufferViewKey.empty() ||
        !source.embeddedIndex.empty() ||
        !source.stableDiscriminator.empty();
    if (!hasStableIdentity && !source.displayName.empty())
        key += ";name=" + PercentEncodeModelTextureToken(source.displayName);
    return key;
}

const char* ToString(const ModelTextureResolutionKind kind)
{
    switch (kind)
    {
    case ModelTextureResolutionKind::ExplicitRemap: return "ExplicitRemap";
    case ModelTextureResolutionKind::SourcePath: return "SourcePath";
    case ModelTextureResolutionKind::NameSearch: return "NameSearch";
    case ModelTextureResolutionKind::ModelEmbeddedFallback: return "ModelEmbeddedFallback";
    case ModelTextureResolutionKind::Invalid: return "Invalid";
    case ModelTextureResolutionKind::Missing:
    default: return "Missing";
    }
}

const char* ToString(const TextureSourceKind kind)
{
    switch (kind)
    {
    case TextureSourceKind::ExternalFile: return "ExternalFile";
    case TextureSourceKind::EmbeddedData: return "EmbeddedData";
    case TextureSourceKind::BufferView: return "BufferView";
    case TextureSourceKind::Missing:
    default: return "Missing";
    }
}
}
```

`PercentEncodeModelTextureToken` can start as a private helper in `ModelTextureReferenceResolver.cpp`; Task 2 will reuse the same encoding rules for settings/report values. When collecting all source references for one model, resolve collisions by appending the most stable available differentiator first: format image/texture index, container ordinal, `materialTextureKey`, material channel path, then importer-provided source-stable ordinal. Add `;dup=<n>` only when no stable differentiator exists; mark those entries `stableKeyStatus = "OrderDerived"` and emit `model-texture-source-key-order-derived` so Asset Properties does not silently trust old remaps.

- [ ] **Step 3: Add report header and implementation skeleton**

Create a report type that can be expanded in later tasks:

```cpp
#pragma once

#include "Assets/ModelTextureReferenceResolver.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
struct ModelTextureResolutionReport
{
    uint32_t reportVersion = 1u;
    std::string modelAssetId;
    std::string targetPlatform;
    uint32_t importerVersion = 0u;
    std::string settingsFingerprint;
    std::vector<ResolvedModelTextureReference> entries;
};

std::string SerializeModelTextureResolutionReport(const ModelTextureResolutionReport& report);
std::optional<ModelTextureResolutionReport> ParseModelTextureResolutionReport(const std::string& text);
std::filesystem::path ModelTextureResolutionReportPath(const std::filesystem::path& committedArtifactRoot);
}
```

- [ ] **Step 4: Return an empty parse-safe implementation**

```cpp
#include "Assets/ModelTextureResolutionReport.h"

namespace NLS::Editor::Assets
{
std::string SerializeModelTextureResolutionReport(const ModelTextureResolutionReport& report)
{
    std::string text = "NULLUS_MODEL_TEXTURE_RESOLUTION_REPORT=1\n";
    text += "reportVersion=" + std::to_string(report.reportVersion) + "\n";
    text += "modelAssetId=" + report.modelAssetId + "\n";
    text += "targetPlatform=" + report.targetPlatform + "\n";
    text += "importerVersion=" + std::to_string(report.importerVersion) + "\n";
    text += "settingsFingerprint=" + report.settingsFingerprint + "\n";
    text += "entryCount=" + std::to_string(report.entries.size()) + "\n";
    return text;
}

std::optional<ModelTextureResolutionReport> ParseModelTextureResolutionReport(const std::string& text)
{
    if (text.find("NULLUS_MODEL_TEXTURE_RESOLUTION_REPORT=1") == std::string::npos)
        return std::nullopt;
    return ModelTextureResolutionReport {};
}

std::filesystem::path ModelTextureResolutionReportPath(const std::filesystem::path& committedArtifactRoot)
{
    return committedArtifactRoot / "texture-resolution-report.txt";
}
}
```

- [ ] **Step 5: Confirm new `.cpp` files are picked up by CMake**

`Project/Editor/CMakeLists.txt` uses `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` for editor sources. Do not edit it unless local configure/build output shows the new files are not discovered.

- [ ] **Step 6: Build/test compile**

Run:

```powershell
ctest --test-dir build --output-on-failure -R NullusUnitTests
```

Expected: existing status is unchanged; failures at this stage should be compile/link issues from the new files and must be fixed before proceeding.

---

### Task 2: Add Settings and Report Tests First

**Files:**
- Modify: `Tests/Unit/AssetImportPipelineTests.cpp`
- Modify: `Project/Editor/Assets/AssetImporterSettings.h`
- Modify: `Project/Editor/Assets/AssetImporterSettings.cpp`
- Modify: `Project/Editor/Assets/ModelTextureResolutionReport.cpp`

- [ ] **Step 1: Write failing settings tests**

Add tests named:

```cpp
TEST(AssetImportPipelineTests, ModelTextureResolutionSettingsUseUnityAlignedDefaults)
TEST(AssetImportPipelineTests, ModelTextureResolutionSettingsRejectMalformedEncodedValues)
TEST(AssetImportPipelineTests, ModelTextureRemapSettingsRoundTripStableSourceKeys)
TEST(AssetImportPipelineTests, ModelTextureRemapSettingsEscapesStableKeysAndRejectsMalformedValues)
```

Assertions:

```cpp
NLS::Core::Assets::AssetMeta meta;
const auto settings = NLS::Editor::Assets::LoadModelTextureResolutionSettings(meta);
EXPECT_TRUE(settings.useExternalTextures);
EXPECT_TRUE(settings.searchByName);
EXPECT_TRUE(settings.autoImportMissingTextureFiles);
EXPECT_EQ(settings.embeddedTextureMode, NLS::Editor::Assets::ModelEmbeddedTextureMode::ModelSubAsset);
```

For remap round-trip, save a fake source key and fake texture GUID in `meta.settings`, load it, clear it, and assert the setting key is removed.
Use stable keys containing `%`, `=`, `#`, `;`, `|`, newline, and non-ASCII UTF-8 bytes to prove metadata keys and values survive round-trip.

- [ ] **Step 2: Run tests and verify failure**

Run:

```powershell
ctest --test-dir build --output-on-failure -R NullusUnitTests
```

Expected: compile failure or test failure because `LoadModelTextureResolutionSettings` and remap helpers do not exist.

- [ ] **Step 3: Implement settings API**

Add to `AssetImporterSettings.h`:

```cpp
enum class ModelEmbeddedTextureMode
{
    ModelSubAsset
};

struct ModelTextureResolutionSettings
{
    uint32_t settingsVersion = 1u;
    bool useExternalTextures = true;
    bool searchByName = true;
    bool autoImportMissingTextureFiles = true;
    ModelEmbeddedTextureMode embeddedTextureMode = ModelEmbeddedTextureMode::ModelSubAsset;
};

struct ModelTextureExplicitRemapSetting
{
    std::string sourceStableKey;
    NLS::Core::Assets::AssetId targetAssetId;
    std::string targetSubAssetKey;
    std::string targetEditorPath;
};

ModelTextureResolutionSettings LoadModelTextureResolutionSettings(const NLS::Core::Assets::AssetMeta& meta);
void StoreModelTextureResolutionSettings(NLS::Core::Assets::AssetMeta& meta, const ModelTextureResolutionSettings& settings);
std::string MakeModelTextureRemapSettingKey(const std::string& stableSourceKey);
std::vector<ModelTextureExplicitRemapSetting> LoadModelTextureRemapSettings(const NLS::Core::Assets::AssetMeta& meta);
void StoreModelTextureRemapSetting(NLS::Core::Assets::AssetMeta& meta, const ModelTextureExplicitRemapSetting& remap);
void ClearModelTextureRemapSetting(NLS::Core::Assets::AssetMeta& meta, const std::string& stableSourceKey);
std::string ComputeModelTextureSettingsFingerprint(const NLS::Core::Assets::AssetMeta& meta);
```

Implement in `.cpp` using string settings keys from `contracts/model-texture-remap-settings.md`. The remap value format is `<texture-guid>#<sub-asset-key>` with percent encoding for both fields before joining. Unknown settings versions, malformed keys, and malformed percent encodings must not partially apply a remap.

- [ ] **Step 4: Write failing report round-trip tests**

Add:

```cpp
TEST(AssetImportPipelineTests, ModelTextureResolutionReportRoundTripsEntriesAndDiagnostics)
TEST(AssetImportPipelineTests, ModelTextureResolutionReportRejectsMalformedPayload)
TEST(AssetImportPipelineTests, ModelTextureResolutionReportRejectsStaleContext)
TEST(AssetImportPipelineTests, ModelTextureResolutionReportEscapesSpecialCharacters)
```

Use one entry with `resolutionKind=SourcePath`, one entry with `resolutionKind=ModelEmbeddedFallback`, one diagnostic code `model-texture-name-ambiguous`, and assert parse returns `sourceStableKey`, `sourceKey`, `materialTextureKey`, `textureSourceKind`, `targetAssetId`, `resourcePath`, and diagnostics. The stale-context test should reject mismatched `modelAssetId`, `targetPlatform`, `importerVersion`, `settingsFingerprint`, or `reportVersion`.

- [ ] **Step 5: Implement report serialization/parsing fully**

Use simple line-based key/value parsing consistent with existing artifact text payload style. Percent-encode values that can contain `%`, `=`, `#`, `;`, `|`, carriage return, newline, or non-ASCII bytes. Prefer a shared local helper in `ModelTextureResolutionReport.cpp`/`AssetImporterSettings.cpp` if adding a common private utility is not worth the dependency. Add a validation function such as:

```cpp
struct ModelTextureReportContext
{
    std::string modelAssetId;
    std::string targetPlatform;
    uint32_t importerVersion = 0u;
    std::string settingsFingerprint;
};

bool IsModelTextureResolutionReportCurrent(
    const ModelTextureResolutionReport& report,
    const ModelTextureReportContext& context);
```

- [ ] **Step 6: Verify**

Run:

```powershell
ctest --test-dir build --output-on-failure -R NullusUnitTests
```

Expected: new settings and report tests pass.

---

### Task 3: Implement Resolver Pure Logic

**Files:**
- Modify: `Tests/Unit/AssetImportPipelineTests.cpp`
- Modify: `Project/Editor/Assets/ModelTextureReferenceResolver.h`
- Modify: `Project/Editor/Assets/ModelTextureReferenceResolver.cpp`

- [ ] **Step 1: Add failing pure resolver tests**

Add tests:

```cpp
TEST(AssetImportPipelineTests, ModelTextureStableKeyUsesVersionedKindSourceAndNormalizedUri)
TEST(AssetImportPipelineTests, ModelTextureStableKeyHandlesEmptySourceKeyDataUriBufferViewAndEmbeddedIndex)
TEST(AssetImportPipelineTests, ModelTextureStableKeyAddsDeterministicCollisionSuffixes)
TEST(AssetImportPipelineTests, ModelTextureStableKeyKeepsUriIdentityWhenDisplayNameChanges)
TEST(AssetImportPipelineTests, ModelTextureStableKeyEscapesSemicolonAndPipeSeparators)
TEST(AssetImportPipelineTests, ModelTextureStableKeyMarksOrderDerivedFallback)
TEST(AssetImportPipelineTests, ModelTextureStableKeyIsStableAcrossRepeatedImports)
TEST(AssetImportPipelineTests, ModelTextureResolverUsesExplicitRemapBeforePath)
TEST(AssetImportPipelineTests, ModelTextureResolverUsesSourcePathBeforeNameSearch)
TEST(AssetImportPipelineTests, ModelTextureResolverDoesNotBindAmbiguousNameMatches)
TEST(AssetImportPipelineTests, ModelTextureResolverUsesModelLocalFallbackWhenExternalDisabled)
TEST(AssetImportPipelineTests, ModelTextureResolverOrdersMultiRootNameCandidatesDeterministically)
TEST(AssetImportPipelineTests, ModelTextureResolverTreatsCaseCollisionAsAmbiguous)
TEST(AssetImportPipelineTests, ModelTextureResolverSkipsUnimportedNameCandidatesWhenAutoImportDisabled)
```

Build small fake candidate inputs in memory instead of importing a full model.

- [ ] **Step 2: Run and verify failure**

Expected failure: missing resolver request/candidate types or failing assertions.

- [ ] **Step 3: Add resolver request model**

Extend header with:

```cpp
struct ModelTextureAssetCandidate
{
    NLS::Core::Assets::AssetId assetId;
    std::string subAssetKey;
    std::filesystem::path editorPath;
    std::filesystem::path artifactPath;
    std::string displayName;
    std::string assetType;
    bool imported = true;
};

struct ModelTextureExplicitRemap
{
    std::string sourceStableKey;
    ModelTextureAssetCandidate target;
};

struct ModelTextureResolveRequest
{
    ModelTextureResolutionSettings settings;
    std::vector<ModelTextureExplicitRemap> remaps;
    std::vector<ModelTextureAssetCandidate> pathCandidates;
    std::vector<ModelTextureAssetCandidate> nameCandidates;
};

std::vector<ModelTextureSourceReference> AssignModelTextureStableKeys(
    std::vector<ModelTextureSourceReference> sources);

ResolvedModelTextureReference ResolveModelTextureReference(
    const ModelTextureSourceReference& source,
    const ModelTextureResolveRequest& request);
```

- [ ] **Step 4: Implement priority logic**

Implement exact priority:

1. If external disabled: fallback or missing with `model-texture-external-resolution-disabled`.
2. Valid explicit remap by stable key.
3. First deterministic source path candidate when available and `assetType == Texture`.
4. Unique name candidate only when `searchByName` and exactly one viable texture candidate exists.
5. Ambiguous name diagnostic when more than one viable candidate remains after filtering by project roots, texture asset type, filename/stem rule, auto-import setting, and deterministic root/path/GUID ordering.
6. Model-local fallback when payload exists.
7. Missing.

Do not use content hashes for candidate matching. Use content hashes only in dependency `hashOrVersion` fields after a concrete target has been selected.

- [ ] **Step 5: Verify**

Run `NullusUnitTests`. Expected: resolver tests pass.

---

### Task 4: Integrate Resolver Into Model Import for US1

**Files:**
- Modify: `Tests/Unit/AssetImportPipelineTests.cpp`
- Modify: `Project/Editor/Assets/ExternalAssetImporter.cpp`
- Inspect: `Runtime/Rendering/Assets/MaterialConversion.h/.cpp`

- [ ] **Step 1: Add failing full import tests**

Add tests named:

```cpp
TEST(AssetImportPipelineTests, ExternalModelImportReusesProjectTextureAssetBySourcePath)
TEST(AssetImportPipelineTests, ExternalModelImportAutoImportsMissingProjectTextureAssetWhenEnabled)
TEST(AssetImportPipelineTests, ExternalModelImportKeepsModelLocalTextureForEmbeddedFallback)
TEST(AssetImportPipelineTests, ExternalModelImportWarnsWhenAutoImportFailsAndFallsBack)
TEST(AssetImportPipelineTests, ExternalModelImportMaterialPathsUseImportedSceneTextureKeys)
TEST(AssetImportPipelineTests, ExternalModelImportRecordsTextureDependencyFreshness)
TEST(AssetImportPipelineTests, ExternalModelImportRecordsPathMappingFingerprintForSameCountDifferentCandidates)
TEST(AssetImportPipelineTests, ExternalModelImportReportsUnsupportedTextureEncodingWithoutPollutingExternalSubAssets)
TEST(AssetImportPipelineTests, ExternalModelImportDoesNotReplaceReportWhenImportFails)
TEST(AssetImportPipelineTests, ExternalModelImportReplacesReportAfterSuccessfulReimport)
TEST(AssetImportPipelineTests, ExternalModelReimportPrunesLegacyExternalTextureSubAssetsOnly)
```

Use existing test helpers around `ExternalObjModelImportWritesTextureArtifactPayloads` and path-resolution tests. Assertions:

- Material payload uniform path points at the external texture artifact path.
- Material path lookup uses `materialTextureKey`, with fixture coverage for glTF base color/normal and at least one FBX diffuse/opacity or OBJ MTL slot.
- Model manifest does not include `ArtifactType::Texture` for externally resolved texture.
- Embedded/data fallback still includes model-owned texture artifact.
- Manifest dependencies include `SourceAssetGuid`, `ImportedArtifact`, and `PathToGuidMapping` records with the schemas in `data-model.md`.
- `PathToGuidMapping.hashOrVersion` changes when the candidate count is unchanged but GUID, normalized path, asset type, imported state, case-fold policy, or artifact identity changes.
- Unsupported embedded or external texture encoding produces a stable diagnostic visible in the report and does not create externally resolved duplicate texture sub-assets.
- Failed import leaves the previous committed `texture-resolution-report.txt` untouched.
- Successful reimport replaces `texture-resolution-report.txt` and removes stale entries.
- Reimport of a legacy manifest keeps mesh/material/prefab/skeleton/animation sub-asset keys stable while pruning only externally resolved duplicate texture sub-assets.

- [ ] **Step 2: Run tests and verify failure**

Expected: current importer creates model-owned texture artifacts for external texture references.

- [ ] **Step 3: Build source references from `scene.textures`**

In `ExternalAssetImporter.cpp`, collect `ModelTextureSourceReference` entries from imported scene texture records. Set:

- `sourceKey` from texture record.
- `materialTextureKey` from the exact texture key used by material channels; if the importer currently has a single texture key field, copy that field into both `sourceKey` and `materialTextureKey`.
- `uri` from texture record.
- `normalizedUri` from project path helpers or safe URI normalization.
- `TextureSourceKind` from external URI, data URI, buffer view, embedded record, or missing reference state.
- `bufferViewKey` and `embeddedIndex` where available.
- `stableDiscriminator` from format image/texture index, material texture channel path, FBX embedded texture ID, OBJ MTL map declaration key, or another source-stable ordinal when available.
- `displayName` from record name/display field available in `ImportedSceneNamedRecord`.
- `hasModelLocalPayload` based on data URI, buffer view, embedded payload, or readable source bytes.

After collection, call `AssignModelTextureStableKeys` so duplicate/colliding references get deterministic stable keys before remap lookup.

- [ ] **Step 4: Resolve external texture candidates**

Use existing editor roots, meta loading, artifact manifest loading, and artifact database records to produce candidates for:

- source path match,
- unique name search,
- optional automatic texture import when source path is a project file without current artifact.

Rules to implement:

- Source path candidates must stay inside configured project asset roots.
- Name search considers texture assets only, filename including extension first, stem only when the source lacks extension.
- Case collisions are ambiguous on case-insensitive comparisons.
- Multi-root ordering is root order, normalized project-relative path, then GUID.
- Unimported texture files are candidates only when `autoImportMissingTextureFiles` is enabled.
- Automatic import is texture-only and non-fatal; failed imports add `model-texture-auto-import-failed`.
- Keep global content hash out of scope.

- [ ] **Step 5: Feed material conversion resolved paths**

Replace the old model-local-only `BuildTextureArtifactPathMap` call with a map built from `ResolvedModelTextureReference.materialTextureKey -> resourcePath`. Never key this map by `sourceStableKey` or display name.

- [ ] **Step 6: Filter texture payloads**

Update `LoadTexturePayloads` or its caller to load/serialize only references whose `kind == ModelEmbeddedFallback`.

- [ ] **Step 7: Record dependencies**

For external texture resolutions, add deduplicated dependency records:

- `SourceAssetGuid`: `value=<texture-guid>`, `hashOrVersion=<texture-meta-version-or-content-hash>`.
- `ImportedArtifact`: `value=<texture-guid>#<sub-asset-key>@<targetPlatform>`, `hashOrVersion=<artifact-content-hash-or-manifest-version>`.
- `PathToGuidMapping`: `value=<resolution-scope>|<normalized-query>|<match-mode>`, `hashOrVersion=<mapping-fingerprint>`.

Record path/name mapping dependencies for successful, missing, and ambiguous lookup results so a model becomes stale when a previously missing path appears or a unique-name candidate set changes. Build `mapping-fingerprint` from canonical rows:

```text
rootIndex|normalizedProjectPath|assetGuid|assetType|importedState|caseFoldPolicy|caseFoldedName|artifactSubAssetKey|artifactHashOrVersion
```

Percent-encode every row field before joining. Include rejected same-name candidates that were excluded because of asset type or import state so changes in viability stale the model deterministically.

- [ ] **Step 8: Persist report**

Write the serialized `ModelTextureResolutionReport` as a side report under the model artifact root at `ModelTextureResolutionReportPath(committedRoot)`. Do not add it as a runtime sub-asset in the model manifest.

Commit rule:

1. Serialize the report under the pending artifact root.
2. Parse it back and validate `modelAssetId`, `targetPlatform`, importer version, settings fingerprint, and report version.
3. Move/replace it only as part of the successful model artifact commit.
4. If model import fails, leave the previous committed report untouched.
5. If reimport succeeds, replace the report completely.

- [ ] **Step 9: Verify**

Run `NullusUnitTests`. Expected: US1 tests pass and existing texture import tests remain green.

---

### Task 5: Add Explicit Remap Behavior for US2

**Files:**
- Modify: `Tests/Unit/AssetImportPipelineTests.cpp`
- Modify: `Project/Editor/Assets/AssetImporterSettings.cpp`
- Modify: `Project/Editor/Assets/ModelTextureReferenceResolver.cpp`
- Modify: `Project/Editor/Assets/ExternalAssetImporter.cpp`

- [ ] **Step 1: Add failing remap tests**

Add:

```cpp
TEST(AssetImportPipelineTests, ExternalModelImportExplicitTextureRemapOverridesSourcePath)
TEST(AssetImportPipelineTests, ExternalModelImportInvalidTextureRemapWarnsAndFallsBack)
TEST(AssetImportPipelineTests, ModelTextureRemapSettingsClearReturnsToAutomaticResolution)
TEST(AssetImportPipelineTests, ModelTextureRemapSettingsRejectMalformedEscapedValues)
```

- [ ] **Step 2: Implement remap setting helpers**

Implement:

```cpp
std::optional<ModelTextureExplicitRemapSetting> ParseModelTextureRemapSetting(...);
void StoreModelTextureRemapSetting(...);
void ClearModelTextureRemapSetting(...);
```

Use the contract key/value format.

Required behavior:

- Store remap values as `<texture-guid>#<sub-asset-key>` with percent-encoded fields.
- Load only remaps whose setting suffix decodes to a valid `mtxsrc:v1` stable key.
- Ignore malformed percent sequences and emit `model-texture-remap-malformed` through import diagnostics when encountered during model import.
- Clearing removes exactly `MODEL_TEXTURE_REMAP.<encoded-source-stable-key>` and leaves unrelated model importer settings untouched.

- [ ] **Step 3: Validate remap targets**

Resolver/integration must reject:

- missing GUID,
- non-texture asset type,
- missing artifact path.
- unavailable texture artifact for the current target platform.

Add diagnostic codes from `contracts/import-diagnostics.md`.

- [ ] **Step 4: Verify**

Run `NullusUnitTests`. Expected: remap tests pass.

---

### Task 6: Add Asset Properties UI for US3

**Files:**
- Modify: `Tests/Unit/AssetImportPipelineTests.cpp`
- Modify: `Project/Editor/Panels/AssetProperties.h`
- Modify: `Project/Editor/Panels/AssetProperties.cpp`
- Modify: `Project/Editor/Assets/ModelTextureResolutionReport.h/.cpp`

- [ ] **Step 1: Add pure helper tests for report row extraction**

Add tests that parse a report and convert entries into display rows. Keep immediate-mode widget behavior manual.

Use these test names:

```cpp
TEST(AssetImportPipelineTests, AssetPropertiesModelTextureRowsHideMissingStaleAndMalformedReports)
TEST(AssetImportPipelineTests, AssetPropertiesModelTextureRowsExposeInvalidTargetsAndWarnings)
TEST(AssetImportPipelineTests, AssetPropertiesModelTextureSettingEditsWriteMetadataOnlyUntilReimport)
TEST(AssetImportPipelineTests, AssetPropertiesModelTextureRemapApplyAndClearWritesExpectedSettings)
```

The helper should return an empty row list and a display diagnostic for missing/stale/malformed reports, not throw and not block preview.

- [ ] **Step 2: Add model texture setting controls**

In `CreateModelSettings`, add booleans for:

- `MODEL_TEXTURE_USE_EXTERNAL_TEXTURES`
- `MODEL_TEXTURE_SEARCH_BY_NAME`
- `MODEL_TEXTURE_AUTO_IMPORT_MISSING`

Add an embedded mode combo with only `ModelSubAsset` in phase one.

- [ ] **Step 3: Add External Textures/Remaps group**

Create a collapsible group that shows:

- source display name,
- URI,
- stable remap key,
- material texture key,
- resolution kind,
- target path/GUID,
- diagnostics.

Only show report rows when `IsModelTextureResolutionReportCurrent` accepts the selected model context. Stale or malformed reports should be hidden with a compact "not current" style status row.

- [ ] **Step 4: Add remap edit and clear**

Use existing asset picker patterns. Only persist texture asset remaps. On clear, remove the remap setting. Reimport remains the step that commits output changes.

After apply/clear, mark the model as requiring reimport. Do not rewrite material artifacts or resolution reports directly from Asset Properties.

- [ ] **Step 5: Verify**

Run `NullusUnitTests`, then perform manual quickstart.

---

### Task 7: Final Validation and Review

**Files:**
- Modify: `specs/052-unity-aligned-asset-reuse/quickstart.md`

- [ ] **Step 1: Run full focused validation**

```powershell
ctest --test-dir build --output-on-failure -R NullusUnitTests
```

- [ ] **Step 2: Confirm generated files untouched**

```powershell
git diff --name-only | rg "Runtime/.*/Gen/"
```

Expected: no output.

- [ ] **Step 3: Inspect feature diff**

```powershell
git diff --stat
git diff -- Project/Editor/Assets Project/Editor/Panels Runtime/Rendering/Assets Tests/Unit specs/052-unity-aligned-asset-reuse
```

- [ ] **Step 4: Run required plan-review gate**

Use `plan-review` per AGENTS.md. Fix P0/P1 findings before finalizing.

- [ ] **Step 5: Final report**

Summarize changed files, tests run with exact result, manual verification result, and any remaining risks.
