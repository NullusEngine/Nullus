# Nullus Animation sample

Copy this directory into a project's `Assets/AnimationSample` folder, preserving
the `.meta` files. Import `TwoBoneCharacter.gltf` first, then `Walk.animclip`, `Locomotion.blendspace`,
`UpperBody.bonemask`, and `Hero.animgraph`.

Open **Window > Animation Workbench** and open `TwoBoneCharacter.gltf` as the preview
character. Open `Hero.animgraph`, compile it, and use the preview controls to play.
The graph exposes `Speed`, `IsGrounded`, and `UpperBodyWeight`. Bind `Hero.animgraph`
to a generated character Prefab's Animator to drive the same graph from gameplay.
The `Jump` trigger also enters the jump state; landing blends back after its exit
time. Use **Preview Parameters** to test these inputs without changing defaults.

`AnimationSample.scene` includes the bound Animator, a skinned body, camera and
light. Set it as the start scene to test a cooked Player. Its graph starts at
walking speed so deformation is visible immediately.
`FbxSample.scene` plays the FBX `Move` Take through the same components and uses
the blue material from `TwoBoneCharacter.gltf` to make GPU readback comparable.

`Idle`, `Walk`, `Run`, `Jump`, and `UpperBody` are deliberately simple synthetic
clips. All geometry and animation data in this sample is generated for Nullus;
no external character assets are required.

The FBX importer is additionally tested against the repository's existing
Assimp `animation_with_skeleton.fbx` fixture. To preview an FBX, open its original
file in the workbench. Reader, Take filtering, and sample-rate settings come from
the model `.meta` file. Animation-only FBX files can bind a compatible skeleton;
retargeting different skeleton proportions is outside P0/P1.

Saved source, working preview, and applied runtime programs are separate. Save
writes authoring data. Compile/Preview validates the current working document.
Apply publishes a successfully compiled graph to instances bound to that saved
asset GUID on their next animation update.
