# Animation integration

Extend existing Clip, ClipAsset, AnimationProgram and AnimationFrameResult with typed property tracks and discrete asset-reference tracks. The evaluator supports property-only programs without a synthetic bone. Existing skeleton programs retain validation and sampling behaviour.

Sprite keys select a reference over a half-open time interval. They never interpolate GUIDs, file IDs or texture coordinates. During blends choose the highest-weight contribution; ties prefer the incoming transition. Loop/PingPong/Once, nonuniform frame durations, seeking and event boundaries need tests.

Bindings identify an owner-relative object, component and stable property. Resolve through existing reflection/setters, invalidating cached handles when ownership changes. Apply properties and publish skin poses before final animation-event dispatch. Save/Apply semantics follow the current workbench.

SpriteLibrary/Resolver select skin variants through stable references. Advanced 2D bones, weights, inverse bind pose and IK share the existing Animator/skeleton representation; CPU deformation writes shared render buffers. Bone/weight/pose editors and runtime/cooked examples are mandatory.
