# Unified 2D architecture and source audit

Status: Phase 0 design baseline; implementation and verification are tracked separately in Progress.md.

## Scope and compatibility

Requirements.md preserves the requested scope. FeatureMatrix.md tracks individual requirements through Phase 0–8. Later phases remain required. Existing Scene, GameObject, Transform, object identifiers, scripting hosts, input, RHI, materials, audio and UI remain shared with 3D.

## Source audit

| Area | Existing integration | Reuse | Required extension / compatibility boundary |
|---|---|---|---|
| Scene and lifetime | Runtime/Engine/SceneSystem/Scene.{h,cpp}; GameObject; Components/Component and TransformComponent | Reflected components, enable/disable, deferred destruction, world transforms | Add sprite fast-access/proxy collection; keep serialized scene and transform representation unchanged |
| Serialization and Prefab | Engine/Serialize/PPtrResourceTypes.h; PersistentManager; ObjectGraphInstantiator; Editor PrefabUtilityFacade | GUID + local file identifier, object graph cloning, prefab overrides | Register SpriteAsset as a resource target; stable child identity must not depend on slice order |
| RHI and cameras | Rendering/Entities/Camera; Engine/Rendering/SceneProxySourceRegistry, SceneRenderStore, RenderScene; Rendering/FrameGraph | Orthographic projection, immutable view inputs, graphics commands, retirement | Sprite payload, shared geometry/instance buffers, sorted transparent submission; no component pointers on render thread |
| Assets and import | Core/Assets/AssetMeta, ArtifactManifest, ArtifactWriter; Editor/Assets/ExternalAssetImporter; Cook/AssetCooker | Versioned metadata, content-addressed artifacts, target manifests, texture decoding | Emit Sprite artifacts alongside the unchanged primary Texture; persist slice UUIDs in importer metadata |
| Shaders and materials | Rendering/Assets/ShaderArtifact; Material/MaterialInstance; MaterialCompiler | Exact platform/variant reflection, shader compilation and material ownership | Sprite Unlit/Lit interface, per-instance tint/UV, normal mapping; preserve Working/Saved/Applied semantics |
| Animation | Animation/Animation.h, Graph.h, AnimationInstance.cpp; AnimatorComponent; AnimationWorkbench | Clips, graph state machine, events, skeletons, preview | Typed property/reference tracks and skeleton-free programs; sprite references use discrete sampling |
| Input and update order | Scripting/RuntimePlaySession.cpp; Input/InputDomain; ScriptEngineApi | Fixed input sampling, FixedUpdate/Update/LateUpdate, focus/UI blocking | Add fixed-step cap and 2D simulation after gameplay FixedUpdate; retain existing project defaults |
| Script languages | Scripting/PlayScriptBindings, LuaPlayBindings, CoreClrScriptBackend; Managed/Nullus.Managed | Portable value ABI and reflected component APIs | Sprite, Physics2D, Tilemap, camera/light APIs and typed events; no native physics IDs exposed |
| Editor and Undo | Panels/SceneViewToolbar.cpp; CameraController; Inspector; Undo/EditorUndoService | Existing 2D mode, planar navigation, object/file scopes, grouped edits | Sprite dragging, alpha picking, XY grid, tools; respect active ImGui window ownership |
| Physics, VFX, UI, audio, build | Physics/PhysicsWorld (PhysX); Engine/Rendering/ParticleSceneRenderer; RuntimePlaySession; Build/BuildPipeline | Independent 3D physics, particle submission, UI/audio/session lifetime, .npak packaging | Scene-owned Box2D world; adapt existing particle/material/UI/audio systems, extend dependency collection |
| Platforms | Windows DX12; Linux Vulkan; macOS Metal; README and platform workflows | Existing target policies and shader toolchains | Compile, GPU execution and distributable validation are separate gates; DX11/OpenGL are not release targets |
| Tests and CI | Tests/Unit, Tests/Rendering, Tests/Performance; .github/workflows/build_* | GoogleTest, reflection tests and platform builds | Sprite reference/import/batch tests, GPU pixels, actual editor interactions, three packaged demos |

## Data and thread flow

Source + .meta -> shared compiler -> Texture + Sprite artifacts -> RuntimeAssetDatabase -> SpriteAsset reference -> reflected component -> immutable scene proxy -> cull/sort/batch -> existing Render Graph/RHI.

The game thread resolves assets and publishes immutable payloads. The render thread owns GPU resources. Submitted draws retain owners until retirement. Failed reimport leaves the previous valid revision available and reports a diagnostic.

## Defaults

XY plane, rotation about Z; Z participates in presentation, not Physics2D. Ordinary sprite PPU=100. Pixel-art preset: PPU=16, 320x180 reference resolution, point sampling. Sorting layers have stable identities independent of their user-defined order. Physics uses one world unit per metre.

## Dependency choice

Physics2D targets Box2D 3.2.0 (MIT), C API. Motion locks implement rotation constraints. Mouse Joint is an engine-level spring-target adapter over the 3.2 Motor Joint. Reference: https://box2d.org/documentation/md_release__notes__v320.html . No dependency is considered integrated until it is built and tested.
