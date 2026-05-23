# UI Contract: Component Picker

## Purpose

Define the observable interaction contract for the Inspector-driven searchable component picker so implementation and validation share the same expected behavior.

## Trigger Contract

- **Trigger**: User clicks the Inspector `Add Component` button while an actor is selected.
- **Preconditions**:
  - Inspector has a valid selected actor.
  - Reflection runtime has registered engine component types for the current session.
- **Expected result**:
  - A searchable component picker becomes visible.
  - The picker is populated from the current reflected component set after addability filtering is applied.

## Search Contract

- **Input**: Free-text component name query.
- **Behavior**:
  - Matching is case-insensitive.
  - Empty query restores the full available set.
  - Non-matching query shows an explicit empty-state row/message.
- **Non-goals for this contract**:
  - Fuzzy ranking, categories, favorites, or localization-aware tokenization.

## Entry Availability Contract

- **Shown as selectable**:
  - Component types that are valid, directly instantiable through the existing actor/component path, and currently allowed for the selected actor.
- **Excluded or unavailable**:
  - Base/foundation component types not intended for direct addition.
  - Transform or other actor-singleton component types already satisfied by actor construction rules.
  - Component types blocked by current actor state or runtime addability rules.
- **Expectation**:
  - The picker must not advertise an action that the add path will predictably reject.

## Add Action Contract

- **Input**: User selects one available component entry.
- **Behavior**:
  - The add request is executed against the currently selected actor using the engine's existing dynamic component add path.
  - On success, the picker closes or returns control to the Inspector flow and the new component is immediately visible.
  - On failure, the actor remains unchanged and the user receives a clear failure outcome.

## Refresh Contract

- **After successful add**:
  - Inspector component display refreshes in the same editing flow.
- **After actor change or invalidation while open**:
  - Picker data must stop using stale actor state.
  - Add requests against invalid actor context must not proceed.
