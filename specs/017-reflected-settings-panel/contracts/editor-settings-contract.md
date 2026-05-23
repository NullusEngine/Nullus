# Editor Settings Contract

## Menu Contract

- The editor exposes `Edit > Settings`.
- Activating the command opens the Settings modal.
- Opening the modal blocks Scene view mouse interaction until the modal closes.

## SettingObject Registration Contract

Callers register settings with:

- stable id
- display name
- category path
- persistence scope
- reflected object instance

Expected behavior:

- Registration succeeds when id is unique and the object is reflected.
- Registration fails deterministically when id is empty, duplicated, or object reflection is invalid.
- Registered entries are returned in deterministic category/display order.

## Reflected Drawing Contract

The shared reflected-property drawer accepts:

- a widget container
- a reflected object instance
- draw options such as label width and optional search filter
- an optional change callback

Expected behavior:

- Supported fields create editable widgets.
- Unsupported fields create a stable read-only fallback.
- A value edit invokes the change callback for the owning field.
- Inspector and Settings call the same drawer for reflected fields.

## Persistence Contract

Persistence accepts registered SettingObjects and a target path.

Expected behavior:

- Missing files load defaults.
- Valid saved values overwrite defaults.
- Unknown object ids and fields are ignored.
- Invalid value types are ignored without crashing.
- Saving writes only supported persisted values keyed by stable object id and field name.
