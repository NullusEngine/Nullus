# Physics2D simulation contract

Box2D 3.2.0 (MIT), C API, hidden behind scene-owned Physics2DWorld. The existing PhysX world remains independent. Do not attach competing 2D and 3D dynamic transform authorities to one object.

Fixed input -> script/component FixedUpdate -> apply queued commands -> synchronize static/kinematic data -> step worlds -> read dynamic poses -> collect stable-ID events -> dispatch callbacks -> deferred destruction. No Entity/component deletion occurs from solver callbacks or worker threads.

Old projects retain 0.02s fixed delta. New 2D templates use 1/60s, maximum 8 fixed ticks/frame and 4 Box2D substeps. Drop excess whole ticks after the cap, retain fractional remainder, and expose dropped-time diagnostics. Pause/Step use the same update path.

Dynamic simulation owns XY/Z-rotation. Kinematic movement uses target/velocity APIs. Teleport explicitly specifies velocity preservation and resets interpolation history. Render interpolation never modifies simulation Transform. Validate non-planar/sheared transforms and unrepresentable circle scaling; support negative polygon winding and offsets.

Collider/shape/joint references use engine ownership and generations. Combine multiple shape contacts into component-level Begin/Stay/End. Snapshot event values before dispatch. Rebuilds, scene deletion, enable/disable and Play transitions invalidate stale handles safely.

The complete shape/joint/query list is retained in FeatureMatrix.md. Motion locks replace old fixedRotation APIs; the engine Mouse Joint uses a Motor Joint spring target. Debug drawing is snapshot-based and editor tools must work without a running world.
