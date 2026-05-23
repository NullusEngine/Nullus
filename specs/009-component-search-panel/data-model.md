# Data Model: Reflection-Driven Component Search Panel

## Component Entry

- **Purpose**: Represents one reflected component type as a selectable option in the picker.
- **Fields**:
  - `componentType`: Reflected component type handle used for final add execution.
  - `displayName`: Human-readable label shown in the picker list.
  - `searchKey`: Normalized text used for case-insensitive filtering.
  - `isAddable`: Whether the selected actor can currently receive this component.
  - `availabilityReason`: Optional explanation when the entry is unavailable or excluded from interactive selection.
- **Relationships**:
  - Belongs to one `Selected Actor Context`.
  - Is filtered by one `Component Search Query`.
- **Validation rules**:
  - `componentType` must be valid and derive from the engine component base type.
  - `displayName` and `searchKey` must be derived consistently from the same source name.
  - `isAddable` must be determined from the same rule that governs actual add behavior.

## Component Search Query

- **Purpose**: Captures the user's current text filter while the picker is open.
- **Fields**:
  - `rawText`: Exact search text as entered by the user.
  - `normalizedText`: Case-insensitive comparison form used by filtering.
- **Relationships**:
  - Filters zero or more `Component Entry` records.
- **Validation rules**:
  - Empty query returns the full eligible set.
  - Filtering must not mutate the underlying discovered component set.

## Selected Actor Context

- **Purpose**: Holds the current Inspector target and the actor state needed to determine component availability.
- **Fields**:
  - `actorId`: Stable actor identifier for delayed or deferred actions.
  - `actorPointer`: Current actor reference when valid.
  - `existingComponentTypes`: Current component types already attached to the actor.
  - `isValid`: Whether the actor is still available for addition when an action is requested.
- **Relationships**:
  - Governs all `Component Entry` addability decisions for one picker session.
- **Validation rules**:
  - If the actor becomes invalid, add actions must stop safely.
  - Existing component type information must refresh when the Inspector target or actor component set changes.

## Component Picker Session

- **Purpose**: Describes the transient UI state for one open instance of the searchable picker.
- **Fields**:
  - `query`: Active `Component Search Query`.
  - `entries`: Ordered list of discovered `Component Entry` objects for the current actor context.
  - `selectedIndex`: Current highlighted row if keyboard or click selection is later added.
  - `isOpen`: Whether the picker is visible.
- **Relationships**:
  - Owns the current search/filter state for the selected actor.
- **State transitions**:
  - `Closed -> Open`: User clicks `Add Component`.
  - `Open -> Filtered`: User types or clears search text.
  - `Open/Filtered -> Committed`: User chooses a valid component and add succeeds.
  - `Open/Filtered -> Closed`: User dismisses the picker or actor context becomes invalid.
