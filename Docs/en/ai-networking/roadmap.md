# AI and Networking Status and Plans

This page covers gameplay AI (navigation and decision making) and multiplayer networking. The former has a small working native foundation; the latter has not started.

## AI: partial

### What exists

`Runtime/Engine/AI` provides two native state machines:

| Type | Notes |
| --- | --- |
| `StateMachine` | A set of states and transitions advanced by `Update(dt)` |
| `PushdownMachine` | A pushdown automaton with a state stack, suited to interruptible and resumable flows |
| `State` | A state node holding an update function |
| `StateTransition` | Transitions between states |

These are native C++ types used by the engine and native logic. They are not in the scripting API manifest, so C# and Lua cannot construct state machines directly today.

### What is missing

- Navigation mesh generation, path queries, and dynamic avoidance
- Behavior trees
- Perception (sight, hearing)
- Crowd behaviors
- AI debug visualization and a runtime viewer

## Networking: not implemented

There is no network transport, session management, replication, or synchronization code, and no components reserved for networking.

## Goals

| Area | Goal |
| --- | --- |
| Navigation | Navmesh generation, path queries, dynamic obstacles, and avoidance |
| Decision making | Behavior trees and perception, editable and debuggable in the editor |
| Crowds | Local avoidance and group formation |
| Transport | A lower-level transport abstraction with reliable and unreliable messages |
| Sessions | Session creation, joining, leaving, and reconnection |
| Replication | Object and property replication, authority, and rollback correction |
| Determinism and replay | Record and replay input for debugging and validation |

## Dependencies and order

1. Exposing `StateMachine` and `PushdownMachine` to scripts requires adding them to the API manifest with portable signatures; see the [Scripting API Reference](../scripting/api.md).
2. Navigation depends on the spatial index and geometric queries. Those queries overlap with physics, so they should share infrastructure; see [Physics Status and Plans](../physics/roadmap.md).
3. Behavior trees need editor support and new asset types, following [Editor Script Extensions](../architecture/editor-extensions.md) and [Reflection and Serialization](../architecture/reflection-serialization.md).
4. A networking replication layer must cooperate with scene object lifetime: creation, destruction, and activation state all need stable network identity. That requires defining how object identity is represented on the wire first.
5. Networking and threaded rendering interact through threading: which thread network callbacks run on, and how they produce scene commands. Following the existing model, network callbacks should produce commands rather than mutate render state directly; see [Jobs and Threading](../architecture/jobs-threading.md).

## Not a commitment

This page describes goals and dependencies, not a schedule. When implementation status changes, the [Feature Status Matrix](../roadmap/index.md) and the source are authoritative.

## Next steps

- [Scripting Overview](../scripting/overview.md) for how native types are exposed to scripts.
- [Feature Status Matrix](../roadmap/index.md) for the cross-domain overview.
