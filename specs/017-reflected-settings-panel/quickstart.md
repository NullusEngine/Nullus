# Quickstart: Reflected Editor Settings Panel

## Build

```powershell
cmake --build Build --target NullusUnitTests Editor --config Debug
```

## Automated Validation

```powershell
Build\bin\Debug\NullusUnitTests.exe
```

Expected evidence:

- Settings registry tests pass for unique registration, duplicate rejection, ordering, and search.
- Settings persistence tests pass for missing file defaults, round-trip values, unknown fields, and invalid values.
- Reflected drawer tests pass for label formatting, supported type detection, and unsupported fallback behavior.
- Existing Inspector tests continue to pass.

## Manual Editor Check

1. Launch the Debug editor build.
2. Open `Edit > Settings`.
3. Confirm a modal Settings window opens with top search, left categories, and right reflected properties.
4. Confirm Scene view mouse input does not respond while the modal is open.
5. Edit a visible setting, close Settings, reopen it, and confirm the value remains.
6. Type a partial category or field label into search and confirm matching settings stay discoverable.
