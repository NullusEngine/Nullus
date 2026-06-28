# Contract: Import Diagnostics

## Purpose

Defines diagnostic outcomes required by the feature.

## Required Diagnostic Codes

```text
model-texture-remap-invalid-target
model-texture-remap-non-texture-target
model-texture-remap-malformed
model-texture-source-key-collision
model-texture-source-key-order-derived
model-texture-source-path-missing
model-texture-name-ambiguous
model-texture-artifact-missing
model-texture-auto-import-failed
model-texture-encoding-unsupported
model-texture-embedded-mode-unsupported
model-texture-external-resolution-disabled
model-texture-report-stale
model-texture-report-malformed
```

## Rules

- Invalid remaps are warnings unless no fallback exists and the material texture remains missing.
- Ambiguous name matches are warnings and must not bind automatically.
- Stable source key collisions are warnings only when the deterministic `dup=<n>` suffix preserves distinct remap targets; otherwise they are errors for that texture reference.
- Order-derived stable source keys are warnings and must be visible in Asset Properties; they prevent silent trust in old explicit remaps for that source.
- Missing artifacts for otherwise valid texture assets are warnings and should trigger or suggest texture import when allowed.
- Automatic missing texture import failures are warnings; they must not fail the whole model import unless no material-safe fallback exists and the caller treats missing material resources as fatal.
- Unsupported texture encoding continues to use existing texture artifact diagnostics.
- Stale or malformed reports are editor display diagnostics only; they must not mark model import failed.
- Diagnostics must be available to automated tests and visible through Asset Properties when tied to a model texture reference.
- Diagnostic entries include the source stable key and material texture key whenever available so tests and users can identify the affected material slot.
