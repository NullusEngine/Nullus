# Verification gates

For an unmodified TwoD template's Windows product, run `Tools/2D/ValidateTemplatePlayer.ps1 -ProductDirectory <product> -EvidenceDirectory <fresh-directory> -HideSpriteAuthoringSources`. The runner uses the dedicated template GPU readback, checks blue Sprite coverage and selected script-host initialization, rejects native modules loaded outside the product/Windows roots, verifies the source Hero.png was not shipped, and temporarily hides the two Sprite shader authoring files. It restores those files and only terminates its own validation Player if cleanup is needed. It writes presented.png, logs and evidence.json. This does not verify Editor mouse interaction or other platforms.

Headless tests cover sprite schema/validation/roundtrip, stable identities, source rect/pivot/UV/bounds, sorted adjacent batches, dependencies and reimport. Integration tests cover reflected properties, scene/prefab clone, Undo, Play lifetime, Cook and packaged content.

Real GPU tests cover alpha, flip/rotation/scale, sorting/groups, mask/picking, atlas gutters, pixel scaling and 2D/3D depth/transparent interaction. Run supported RHI thread modes. Unavailable GPU execution is recorded as unexecuted, not replaced with a passing mock.

Actual editor acceptance: create 2D project, import image/sheet, slice, drag sprite, edit/multiselect/Undo, create prefab, save/reopen, Play/Pause/Step. Later add palette/physics/animation/light tools. Exercise floating tool windows and verify actual mouse interaction.

Benchmarks: 10,000 sprites; texture/material/atlas/sorting changes; large map scroll and local edits; many colliders; many lights; animation/deformation; resource load/reload and memory. Use fixed seed/configuration and warmup/sample intervals. Record CPU/GPU distributions, draws/batches/visibility, chunks, physics contacts and texture memory.

Generate the initial Sprite-only workload from a 2D Core project with `python Tools/2D/GenerateSpriteBenchmark.py --project <project.nullus> --count 10000 --seed 12345`. Open Assets/Scenes/SpriteBenchmark.scene and use its orthographic camera. The companion benchmark JSON records the configuration, not a measured result. Compare GPU instancing and CPU batching through Scene View's existing Camera/Advanced controls, and capture actual per-view Sprite statistics. Use the same resolution, RHI/thread mode, warmup and sampling intervals for comparisons. The generator preserves Main.scene and refuses to overwrite an existing benchmark unless --force is specified.

Desktop gates: Windows/DX12, Linux/Vulkan, macOS/Metal each require compile, focused tests, GPU/interactive evidence and independent package startup. All three demos must run from a standalone directory without source images or editor data. Keep original 3D tests. Do not remove assertions or ignore failures to claim completion.
