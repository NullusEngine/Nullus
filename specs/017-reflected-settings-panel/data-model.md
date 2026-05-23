# Data Model: Reflected Editor Settings Panel

## SettingObject

Represents one editor settings page.

- `id`: Stable unique identifier used for lookup and persistence.
- `displayName`: User-facing page name.
- `categoryPath`: Slash-separated navigation path shown on the left side.
- `scope`: Persistence target, initially project or user/editor scope.
- `object`: Reflected object instance whose fields are drawn and persisted.
- `type`: Reflected runtime type for the object.

Validation:
- `id` must be non-empty and unique.
- `displayName` must be non-empty.
- `categoryPath` must be non-empty.
- `object` and `type` must resolve to a valid reflected object.

## SettingsRegistry

Owns registered SettingObject entries.

- `entries`: Ordered collection of SettingObject descriptors.
- `selectedId`: Optional id used by the UI to preserve selection.

Validation:
- Reject duplicate ids.
- Preserve deterministic ordering by category path and display name.
- Allow multiple objects under the same category path.

## ReflectedProperty

Represents a visible reflected field while drawing or persisting.

- `name`: Stable field identity from reflection.
- `label`: Display label derived from the field name.
- `type`: Reflected field type.
- `value`: Current field value read from the reflected object.
- `editable`: Whether the shared drawer supports editing this field.

Validation:
- Invalid fields are skipped.
- Unsupported editable fields render a deterministic fallback and do not write values.

## SettingsWindowState

Transient UI state for the modal Settings window.

- `isOpen`: Whether the modal is visible.
- `searchText`: Current search query.
- `selectedId`: Selected SettingObject id.
- `dirtyIds`: SettingObject ids with unsaved changes.

State transitions:
- Closed -> Open: choose the last selected entry or first visible entry.
- Search changed: keep current selection if still visible; otherwise select the first matching entry.
- Field edited: mark owning SettingObject dirty.
- Close: save dirty SettingObjects before or during close according to implementation policy.

## PersistedSettingsData

File-backed representation of SettingObject field values.

- `version`: Persistence schema version.
- `objects`: Map from SettingObject id to saved fields.
- `fields`: Map from reflected field name to typed saved value.

Validation:
- Missing file loads defaults.
- Unknown object ids are ignored.
- Unknown field names are ignored.
- Type mismatches leave the current/default value unchanged.
