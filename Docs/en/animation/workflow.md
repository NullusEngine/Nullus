# Animation Authoring Workflow

Nullus Animation evaluates skeletal poses on the CPU and skins vertices on the GPU. glTF/GLB and FBX import share Skeleton, SkinBinding, AnimationClip, and SkeletalMesh data.

## Authoring steps

1. Place model and animation sources in the project's Assets directory. FBX follows the model importer's reader setting; selecting Autodesk requires an enabled and available Autodesk FBX SDK.
2. Open the character source in Animation Workbench. Select a Clip / Take, play or pause, and scrub the timeline.
3. Use **Create Clip from Preview Take** to create an `.animclip`, then edit trimming, curves, and Notify events.
4. Create `.blendspace`, `.bonemask`, and `.animgraph` assets, selecting dependencies by GUID and subresource key.
5. **Compile / Preview** uses the working document. **Save** writes authoring data. **Apply** publishes a successfully compiled program. Running Animators retain the applied program; reimporting saved data does not replace it directly.
6. Assign the animation asset to an Animator, then connect a Skinned Mesh Renderer to its skeletal mesh and Animator. Body and clothing meshes using the same skeleton can share an Animator.

Animation Preview owns a separate scene and camera while using the same Animator, pose, and skinning submission path. Preview Notify events appear in its event list; scene events dispatch during animation finalization.

## Assets and scripting

Persistent references use `source GUID#subresource key`, for example `GUID#animation:animation/0`. Imported skeletons, bindings, and source clips remain model subresources. Editable assets use `.animclip`, `.animgraph`, `.blendspace`, and `.bonemask`. Cooking writes native resource containers; an AnimationProgram holds a validated dependency snapshot, so the Player does not need authoring graphs or model sources.

Animator exposes `Play`, `Pause`, `Stop`, `Seek`, `SetLooping`, `SetPlaybackSpeed`, `SetFloat`, `SetBool`, `SetVector`, `SetTrigger`, and `GetTime` through reflection and the scripting API manifest for C# and Lua.

FBX import supports ASCII/Binary files, linear skinning, multiple Takes, Take filtering, and sample-rate overrides. Metadata includes `MODEL_FBX_READER`, `MODEL_IMPORT_SKELETON`, `MODEL_IMPORT_ANIMATIONS`, `MODEL_ANIMATION_SAMPLE_RATE`, `MODEL_ANIMATION_TAKES` (semicolon-separated), and `MODEL_ANIMATION_TARGET_SKELETON`. Binding a separate animation file validates bone names or paths and hierarchy; cross-skeleton retargeting is outside the current workflow.

## Examples and limits

`Templates/Animation` contains a synthetic two-bone glTF, a two-Take ASCII FBX, an animation graph, BlendSpace, BoneMask, and a Clip with footstep events. Preserve their `.meta` files when copying samples.

Use `AnimationSample.scene` as a Player start scene. The graph starts with Speed set to 1; Preview Parameters can change Speed, IsGrounded, and UpperBodyWeight, or send a Jump trigger.

The first implementation uses four bone influences, floating-point TRS, and serial evaluation. Dynamic bounds use CPU reference deformation. GPU geometry and palettes remain retained through submission. Character budgets, compression, Root Motion, Montage, IK, retargeting, and Morph Targets require further work.

See [Animation Validation](validation.md) for automated, GPU, package, and interactive checks. Test targets and compiled backend packages do not establish validation on every device or render path.
