# Contract: Object Reference Picker Cache

## Threading

Provider, refresh, mutation helpers, and getters are editor-main-thread-only and assert that ownership. Worker jobs return owned result values only.

## State Binding

Cache stores retained `PickerCacheLifetime` identity, source facade state, `Loading/Success/Failure/Closed` status, entries, and diagnostic. A facade identity/status change synchronously binds an empty non-actionable cache to the new state before scheduling. `Closed` is terminal for that lifetime and its getters expose no entries.

## Publication

- Current success installs entries and clears diagnostic.
- Current failure clears entries and installs diagnostic.
- Snapshot `Error` installs immediately without a worker.
- Stale lifetime or facade-state success, failure, exception, or cancellation is discarded.
- Getter exposes entries only for current-state success.
- Installing a new provider creates a fresh cache instance/lifetime; it never reopens a closed instance.

## Scheduling

One active future and one latest state per cache lifetime. Every future/result retains both lifetime and facade state. Rejection/submission failure creates no fake future, installs contextual failure, and retries only on explicit request or semantic change. Teardown closes its lifetime before draining/retiring futures; a later owner can create a distinct lifetime without accepting old completion.
