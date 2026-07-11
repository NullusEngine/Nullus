# Contract: Facade Published State

## Read API

`GetPublishedState()` returns one retained immutable state under `m_manifestMutex`. The lock is released before the caller receives the pointer.

## Publication

- Candidate construction, path normalization, indexing, validation, and semantic comparison occur outside the lock.
- Candidate retains its exact base state.
- Locked commit performs only pointer comparison and pointer assignment.
- Base mismatch cannot publish; rebuild scheduling/hook/callback occurs after unlock.
- Manifest, known-current, and snapshot views publish as one state identity.

## Error Contract

Malformed/duplicate snapshot input publishes a new empty `Error` index with stable code, path, and reason. Consumers must not interpret it as valid-empty or retain an older valid view.

## Migration

Remove all old snapshot value, view, iteration, testing-storage, and snapshot-vector entry-builder APIs. No compatibility wrapper may erase `Error`.
