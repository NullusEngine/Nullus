# Tasks: Autodesk FBX SDK ThirdParty Integration

**Input**: `specs/029-fbx-sdk-thirdparty/spec.md`, `specs/029-fbx-sdk-thirdparty/plan.md`

## Phase 1: Setup

- [X] T001 Create `ThirdParty/FBX` package metadata and move official Autodesk packages into `ThirdParty/FBX/packages`.
- [X] T002 Add engine-bundled installed/unpacked SDK output under `ThirdParty/FBX/sdk/<platform>`.
- [X] T003 Add a failing contract test proving `.fbx` import paths must not instantiate `AssimpParser`.

## Phase 2: Build Dependency

- [X] T004 Implement cross-platform FBX SDK discovery in `ThirdParty/CMakeLists.txt`.
- [X] T005 Create the imported `Autodesk::FbxSdk` target with platform include, link, compile definition, and runtime copy settings.
- [X] T006 Link `NLS_Render` against `Autodesk::FbxSdk` in `Runtime/Rendering/CMakeLists.txt`.

## Phase 3: Parser Implementation

- [X] T007 Create `Runtime/Rendering/Resources/Parsers/FbxSdkParser.h`.
- [X] T008 Create `Runtime/Rendering/Resources/Parsers/FbxSdkParser.cpp` with SDK manager/importer lifetime, scene load, triangulation, material extraction, mesh extraction, and imported scene metadata.
- [X] T009 Add clear parser diagnostics/logging for SDK scene loading, triangulation, material extraction, mesh extraction, and total load timing.

## Phase 4: Routing

- [X] T010 Route `.fbx` in `Project/Editor/Assets/ExternalAssetImporter.cpp` to `FbxSdkParser`; keep `.obj` on `AssimpParser` and glTF on the existing importer.
- [X] T011 Route `.fbx` in `Runtime/Core/ResourceManagement/ModelManager.cpp` to `FbxSdkParser`; keep `.obj` on `AssimpParser`.
- [X] T012 Route `.fbx` in `Runtime/Rendering/Resources/Loaders/ModelLoader.cpp` to `FbxSdkParser`; keep non-FBX paths unchanged.
- [X] T013 Bump the FBX scene importer/artifact version if needed to invalidate stale Assimp-derived FBX artifacts.

## Phase 5: Validation

- [X] T014 Run the new contract test and verify it fails before routing changes, then passes after routing changes.
- [X] T015 Configure CMake with the SDK root and verify `Autodesk::FbxSdk` is found.
- [X] T016 Build `NLS_Render` or `NullusUnitTests` to verify FBX SDK headers/libraries compile and link.
- [X] T017 Run focused asset import tests covering FBX routing and existing OBJ/glTF behavior.
- [X] T018 Run plan-review quality gate and address P0/P1 findings before reporting completion.
