# Editor integration

Extend existing SceneView::Set2D, planar camera navigation, grid and gizmos. Preserve 3D camera restoration and active ImGui window input ownership. Add Sprite projection/alpha picking, drag placement on XY, creation menus and selection bounds.

All reflected component properties use existing Inspector serialization, multiselection, asset picking and prefab override paths. New asset windows reuse EditorUndoService scopes with stable object/file identities; do not retain runtime physics/GPU pointers in persistent commands. Drag/brush gestures commit as one transaction.

Project creation selects the 2D template using the current launcher. Sprite/Atlas/Tilemap/Animation/Bone/Light/SpriteShape tools must provide actual preview, edits, save/reimport, copy/paste and Undo. Runtime-only configuration through handwritten JSON is not acceptance.

Play/Pause/Step retain current sandbox isolation. Opening an editor asset never requires a playing scene or initialized physics world. Game View adds reference-resolution and Pixel Perfect presentation controls without advancing simulation per repaint.
