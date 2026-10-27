# Animation Validation

This page provides reproducible checks. Implementation, automated tests, GPU readback, packaged execution, and desktop interaction have separate acceptance requirements.

## Prerequisites

Configure an existing build directory with `NLS_BUILD_TESTS=ON` as described in [Environment and Build](../getting-started/build.md). GPU checks need a working device and the matching backend. Copy samples from `Templates/Animation` with their `.meta` files.

## Automated tests

```powershell
cmake --build build --config Debug --target NullusAnimationTests NullusAnimationImportTests NullusAnimationEditorTests NullusAnimationGpuTests
ctest --test-dir build -C Debug -R "^NullusAnimation" --output-on-failure
```

| Target | Main checks |
| --- | --- |
| `NullusAnimationTests` | TRS/quaternions, sampling, blending, states, events, binary round trips, failed program replacement |
| `NullusAnimationImportTests` | glTF/GLB, FBX, multiple Takes, sample rates, separate animation binding, stable reimport identity, dependencies |
| `NullusAnimationEditorTests` | Document Undo/Redo, scene updates, Prefabs, shared Animators, deferred binding, frozen dependencies |
| `NullusAnimationGpuTests` | Skinned positions and normals, object bindings, retained submission resources, pixel readback |

Record the actual backend, device, configuration, exit status, and skipped cases. Backend-specific checks do not replace full-scene acceptance.

## Packaged Player

1. Use `Templates/Animation/AnimationSample.scene` as the project's start scene and import the model, graph, Clip, BlendSpace, and BoneMask.
2. Follow [Player Packaging and Distribution](../platform-build/packaging.md). Run the packaged executable with `--package-self-test` to check containers, RuntimeManifest, and the start scene.
3. Run these commands in the package directory. Replace `Game.exe` if the product uses another executable name.

```powershell
.\Game.exe --animation-validation-output forward.png --animation-validation-time 0.25 --animation-validation-forward
.\Game.exe --animation-validation-output deferred.png --animation-validation-time 0.75
```

Inspect readback images and exit results, and verify that startup needs no project model or animation authoring sources. The scene must contain a visible animated character. Cover FBX Takes, nonuniform scale, and multiple meshes sharing an Animator separately.

## Editor interaction

Use the mouse on an accessible desktop to open Animation Workbench, change Takes, scrub time, edit states and links, Undo/Redo, save, close, reopen, and Apply to scene instances. A compile failure should keep the previous playable program.

Check Picking, Selection Outline, and DepthOnly separately; silhouette readback does not validate those passes. Autodesk FBX Reader needs a build with that SDK enabled; Assimp results do not cover it.

## Limits

Pose evaluation is serial, vertices use four influences and floating-point TRS, and dynamic bounds use CPU reference deformation. Character budgets, Metal execution, full Vulkan deferred scenes, and complex DCC-specific FBX combinations need independent acceptance.

Root Motion, Montage, IK, retargeting, Morph Targets, animation LOD, and Compute Skinning remain outside the current workflow.
