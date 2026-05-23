# Implementation Plan: Autodesk FBX SDK ThirdParty Integration

**Branch**: `029-fbx-sdk-thirdparty` | **Date**: 2026-05-18 | **Spec**: `specs/029-fbx-sdk-thirdparty/spec.md`
**Input**: Feature specification from `specs/029-fbx-sdk-thirdparty/spec.md`

## Summary

Integrate Autodesk FBX SDK as the required FBX parser dependency for Nullus desktop builds. Store the official platform packages and engine-bundled SDK layout under `ThirdParty/FBX`, add CMake discovery for Windows/Linux/macOS bundled SDK roots, create an imported `Autodesk::FbxSdk` target, and route `.fbx` imports through a new `FbxSdkParser` with no Assimp fallback.

## Technical Context

**Language/Version**: C++20, CMake, platform toolchains already used by Nullus
**Primary Dependencies**: Autodesk FBX SDK 2020.3.9, existing Assimp for non-FBX formats
**Storage**: Source assets under `Assets`, imported artifacts under `Library/Artifacts`, third-party archives under `ThirdParty/FBX/packages`, unpacked SDK contents under `ThirdParty/FBX/sdk/<platform>`
**Testing**: `NullusUnitTests` plus focused CMake/source contract checks
**Target Platform**: Windows, Linux, macOS desktop editor/game builds
**Project Type**: Desktop game engine/editor
**Performance Goals**: Remove Assimp FBX `ReadFile` from FBX import path; large FBX source parsing should use Autodesk SDK.
**Constraints**: No Assimp fallback for `.fbx`; SDK discovery must fail loudly if the engine-bundled SDK is incomplete; Autodesk EULA acceptance is required before installer output can be used.
**Scale/Scope**: CMake integration, runtime rendering parser, editor asset importer, runtime model load path, unit/contract tests.

## Constitution Check

- Spec-first major change: Pass. This bundle is `specs/029-fbx-sdk-thirdparty`.
- Validation matches subsystem: Pass. Use CMake configure/build checks and importer contract/unit tests; platform claims limited to discovered SDK layouts unless separately run on those hosts.
- Generated code boundaries: Pass. No files under `Runtime/*/Gen/` will be hand-edited.
- Incremental verified delivery: Pass. First add package metadata and failing tests, then CMake target, then parser routing, then parser implementation.
- Product runtime preservation: Pass. Editor/Game should remain runnable when SDK is installed; missing SDK fails during configuration rather than at runtime.

## Project Structure

```text
ThirdParty/FBX/
├── README.md
├── packages/
│   ├── fbx202039_fbxsdk_vs2022_win.exe
│   ├── fbx202039_fbxsdk_gcc_linux.tar.gz
│   └── fbx202039_fbxsdk_clang_mac.pkg.tgz
└── sdk/
    ├── windows/
    ├── linux/
    └── macos/

ThirdParty/CMakeLists.txt   # creates Autodesk::FbxSdk target
Runtime/Rendering/CMakeLists.txt
Runtime/Rendering/Resources/Parsers/FbxSdkParser.h
Runtime/Rendering/Resources/Parsers/FbxSdkParser.cpp
Project/Editor/Assets/ExternalAssetImporter.cpp
Runtime/Core/ResourceManagement/ModelManager.cpp
Runtime/Rendering/Resources/Loaders/ModelLoader.cpp
Tests/Unit/AssetImportPipelineTests.cpp
Tests/Unit/FbxSdkIntegrationContractTests.cpp
```

**Structure Decision**: Keep Assimp in place for OBJ and any remaining non-FBX uses. Add FBX SDK as a separate imported dependency and parser so `.fbx` call sites can be audited clearly.

## Research

### Decision: Use official Autodesk packages as repository ThirdParty source

Rationale: The user explicitly requested the official Autodesk SDK and no fallback. The downloaded official page exposes current FBX SDK 2020.3.9 packages for Windows VS2022, Linux GCC, and macOS Clang universal.

Alternatives considered:
- UE 4.27 bundled FBX SDK 2020.2: useful for local validation, but not a Nullus-owned cross-platform dependency source.
- Assimp FBX importer tuning: already attempted and still measured about 50 seconds for Sponza FBX `ReadFile`.
- Third-party mirrors: rejected due to provenance and licensing risk.

### Decision: Require engine-bundled SDK root at configure time

Rationale: Autodesk installers require EULA acceptance. CMake should consume the completed SDK layout under `ThirdParty/FBX/sdk/<platform>` and should not search external SDK installs.

Alternatives considered:
- Automatically running installers from CMake: rejected because it blocks on EULA and is hostile to CI.
- Searching system or UE SDK installs: rejected because the user requires the engine-bundled SDK only.

### Decision: Imported target plus runtime copy

Rationale: UE uses platform-specific include/lib/runtime settings. Nullus should mirror that pattern with `Autodesk::FbxSdk` so target consumers do not duplicate include/library logic.

Alternatives considered:
- Hardcoding paths in `Runtime/Rendering/CMakeLists.txt`: rejected because it spreads platform logic and makes tests harder.

## Design

### CMake

`ThirdParty/CMakeLists.txt` will select only `ThirdParty/FBX/sdk/windows`, `ThirdParty/FBX/sdk/linux`, or `ThirdParty/FBX/sdk/macos` for the active host platform. The chosen target must expose:

- include directories for `fbxsdk.h`
- shared SDK compile definition where needed (`FBXSDK_SHARED`)
- platform link library (`libfbxsdk.lib`, `libfbxsdk.so`, or `libfbxsdk.dylib`)
- runtime library path copied to `NLS_APP_OUTPUT_PATH`

### Parser Routing

All `.fbx` extension handling in editor import and runtime source model load will instantiate `FbxSdkParser`. Assimp remains valid only for `.obj` and other non-FBX formats. `.fbx` failure returns a diagnostic and does not retry with Assimp.

### Parser Scope

Initial `FbxSdkParser` will load scenes, triangulate geometry, extract mesh vertices/indices, material names, texture file paths, and node transforms. It will intentionally skip animation/skin/morph/camera/light data unless already part of cheap scene metadata.

## Validation Plan

- Add a unit/source contract test that fails while `.fbx` paths still reference `AssimpParser`.
- Add CMake configure validation once `Autodesk::FbxSdk` exists.
- Run targeted unit tests for asset import routing and parser failure behavior.
- Run a local build target that compiles `FbxSdkParser` against the SDK.
- Record platform limitation honestly if only Windows is validated in this session.
