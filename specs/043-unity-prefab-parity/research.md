# Research: Unity Prefab Parity Phase 2

## Decision: Align To Unity Behavior Boundaries, Not Source-Level Cloning

**Rationale**: Unity 2018.4 provides the reference behavior: model prefabs are asset-backed, scene instances persist as prefab-instance records with modifications, drag previews are isolated from the active scene, and loaded resources stay cached while reachable. Nullus should mirror these responsibilities and user-visible invariants without copying Unity internals line for line.

**Alternatives considered**:

- Copy Unity class layout directly: rejected because Nullus object model, serialization, renderer, and resource managers differ.
- Continue incremental bug patches in `unity-ready-model-drop`: rejected because the remaining failures are lifecycle-level divergences that need one coordinated design.

## Decision: PrefabInstance Records Are Authoritative For New Scene Saves

**Rationale**: Unity scenes serialize prefab identity and modifications through `PrefabInstance` records plus `PropertyModification` and source correspondence. Nullus must stop treating root `scenePrefab` metadata or unpacked object copies as the authoritative representation for new saves.

**Unity reference anchors**:

- `D:/VSProject/Unity2018.4.0f1/Editor/Src/Prefabs/PrefabInstance.h`
- `D:/VSProject/Unity2018.4.0f1/Editor/Src/Prefabs/PropertyModification.h`
- `D:/VSProject/Unity2018.4.0f1/Runtime/Serialize/SerializedFileTests.cpp`

**Alternatives considered**:

- Keep root-only `scenePrefab` metadata: rejected because it cannot reliably represent source/instance correspondence or structural overrides.
- Save fully unpacked copies: rejected because it loses Unity-like prefab connection and causes drift from the source asset.

## Decision: One Unified Prefab Load Request Across Scene Load And Drag/Drop

**Rationale**: The current user-visible divergence comes from separate load paths. Unity-like behavior requires one identity, one cache key, one readiness gate, and one resource ownership path whether the prefab is restored from a scene or dragged from assets.

**Unity reference anchors**:

- `D:/VSProject/Unity2018.4.0f1/Editor/Mono/AssetDatabase/AssetDatabase.bindings.cs`
- `D:/VSProject/Unity2018.4.0f1/Runtime/Serialize/PersistentManager.cpp`

**Alternatives considered**:

- Optimize scene-load path and drag path separately: rejected because it risks permanent behavioral drift.
- Always block until fully loaded: rejected because Unity-like editor interaction should avoid long UI-thread stalls for large prefabs.

## Decision: Preview Uses A Private Preview Scene Or Equivalent Render-Only Scene

**Rationale**: Unity has first-class preview scenes and marks preview objects separately from normal scene objects. Nullus preview must be non-committed, non-persistent, hidden from normal hierarchy semantics, and safe to cancel without affecting scene instances.

**Unity reference anchors**:

- `D:/VSProject/Unity2018.4.0f1/Editor/Src/SceneManager/EditorSceneManager.cpp`
- `D:/VSProject/Unity2018.4.0f1/Editor/Mono/EditorSceneManager.bindings.cs`
- `D:/VSProject/Unity2018.4.0f1/Editor/Mono/Inspector/PreviewRenderUtility.cs`

**Alternatives considered**:

- Commit a hidden scene object during drag: rejected because it caused data persistence and cleanup bugs.
- Draw only a marker/crosshair until release: rejected because it does not meet the Unity-like mouse-follow model preview requirement.

## Decision: Resource Lifetime Is Owner-Based With Zero-Owner Trimming

**Rationale**: Unity keeps loaded assets and serialized files cached while reachable and unloads unused assets separately. Nullus should use `ResourceLifetimeRegistry` and `ResourceHandle<T>` to prevent both premature unload and unbounded residency.

**Unity reference anchors**:

- `D:/VSProject/Unity2018.4.0f1/Runtime/Serialize/PersistentManager.cpp`
- `D:/VSProject/Unity2018.4.0f1/Runtime/Scripting/Scripting.cpp`

**Alternatives considered**:

- Keep every imported prefab in memory after import: rejected due unbounded memory growth.
- Destroy resources immediately on object deletion: rejected because previews, inspectors, and other scene instances can share the same resource.

## Decision: No-White-Model Is A Readiness Invariant

**Rationale**: The user explicitly rejected white-model behavior. Mesh-only visibility is not a valid ready state for generated/model prefabs. Preview and committed visibility must treat mesh, material, and texture readiness as one gate.

**Alternatives considered**:

- Show mesh first, material later: rejected because it creates misleading feedback and differs from the requested Unity-like ready behavior.
- Use a deliberate "pending" placeholder: accepted only as an explicit non-ready state that cannot be mistaken for the final preview.
