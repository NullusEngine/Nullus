# Research: Reflection-Driven Component Search Panel

## Decision 1: Discover candidate components by enumerating reflected types at runtime

- **Decision**: Build the picker's source list from `meta::Type::GetTypes()` and retain only types that derive from `NLS::Engine::Components::Component`.
- **Rationale**: The reflection runtime already supports global type enumeration and inheritance checks, while `GameObject::AddComponent(meta::Type)` already consumes `meta::Type` for dynamic creation. Using those two existing capabilities removes the current Inspector hardcoding and ensures newly reflected components can appear without Inspector wiring changes.
- **Alternatives considered**:
  - Maintain a manual editor-side registry of addable component types. Rejected because it recreates the same scaling problem the feature is meant to solve.
  - Discover components from generated registration files. Rejected because generated files are not meant to be hand-maintained and would couple the feature to generated output details.

## Decision 2: Filter availability through a shared addability rule instead of UI-only heuristics

- **Decision**: Evaluate each reflected component type against a single addability rule that at minimum excludes invalid base/non-instantiable types, the transform singleton, and component types already blocked by existing actor state.
- **Rationale**: The current Inspector disables some entries by directly probing `GetComponent<T>()`, but that approach does not scale to arbitrary reflected types. A shared rule keeps the picker honest and aligns visible availability with actual add behavior.
- **Alternatives considered**:
  - Allow all reflected component types and handle failures only after click. Rejected because it produces an untrustworthy picker and violates the spec's requirement to avoid invalid add actions.
  - Hardcode a blocklist only for known built-in types. Rejected because it would drift as components grow and would still duplicate engine knowledge.

## Decision 3: Model the UX as a dedicated searchable picker surface, owned by the Inspector flow

- **Decision**: Replace the combo-and-button flow with a dedicated searchable picker surface that opens from the Inspector's `Add Component` action and supports browse, search, choose, and immediate Inspector refresh.
- **Rationale**: The existing combo box cannot scale to reflection-driven discovery. The UI layer already provides `InputText`, selectable text rows, and standalone `PanelWindow` support, which is enough to build a Unity-style picker interaction without inventing a new framework.
- **Alternatives considered**:
  - Keep the combo box and only auto-fill its entries from reflection. Rejected because it does not satisfy the requested Unity-like searchable panel experience.
  - Implement the picker as a generic right-click popup plugin first. Rejected for this feature because the request is specifically anchored to the Inspector button flow and does not require a reusable popup framework yet.

## Decision 4: Keep display naming and sorting deterministic in editor space

- **Decision**: Normalize reflected type names into readable component labels in editor code and sort the picker entries deterministically before display.
- **Rationale**: Reflected names are likely fully qualified or otherwise not directly suitable for designers. A stable label/sort policy makes search and browsing predictable and keeps verification straightforward.
- **Alternatives considered**:
  - Display the raw reflected type name directly. Rejected because it would surface engine namespace noise to the user.
  - Store localized/editor-facing names in reflection metadata as part of this feature. Rejected for now because the spec does not require metadata authoring changes and that would widen scope.
