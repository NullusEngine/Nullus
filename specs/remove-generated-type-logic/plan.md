# Implementation Plan: Remove Generated Type Logic

**Branch**: `029-fbx-sdk-thirdparty` | **Date**: 2026-05-22 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `specs/remove-generated-type-logic/spec.md`

## Summary

Move generated field type classification policy from generated C++ into MetaParser generation. Generated files will keep registering fields and overriding field/class types, but they will no longer contain local `NLS::Array<...>` or `std::vector<...>` prefix parsing logic, and Runtime/Base will no longer reparse full container field type strings.

## Technical Context

**Language/Version**: C++20 runtime, C#/.NET 8 MetaParser templates
**Primary Dependencies**: Nullus reflection runtime, MetaParser T4 template pipeline
**Storage**: N/A
**Testing**: GoogleTest `NullusUnitTests`, MetaParser generation fixtures, `ReflectionTest` where practical
**Target Platform**: Current Windows development build; no cross-platform claim beyond source-level behavior
**Project Type**: Engine/runtime library plus code generator
**Performance Goals**: No measurable runtime regression during reflection registration; container classification happens once during generation
**Constraints**: Do not hand-edit `Runtime/*/Gen/`; preserve dependency tracking and diagnostics
**Scale/Scope**: Typed resolver APIs, one generator model/template update, generated output refresh, targeted tests

## Constitution Check

- **Spec scope**: Required and present because this changes reflection/MetaParser generation behavior.
- **Generated-file boundary**: Pass. Source changes target MetaParser template/runtime helper; `Runtime/*/Gen/` is updated only by generation.
- **Validation path**: Pass. Use MetaParser generation regression test plus reflection runtime tests.
- **Backend/platform claims**: N/A. No rendering backend behavior is changed.
- **Product runtime viability**: Pass. Reflection registration behavior is preserved.

## Project Structure

### Documentation

```text
specs/remove-generated-type-logic/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Runtime/Base/Reflection/
├── ReflectionDatabase.h
└── ReflectionDatabase.cpp

Tools/MetaParser/src/Templates/
└── HeaderGenerated.cpp.tt

Tests/Unit/
├── MetaParserGenerationModuleTests.cpp
├── ReflectionRuntimeCoreTests.cpp
└── ReflectedPropertyDrawerTests.cpp
```

**Structure Decision**: Keep lookup helpers in Runtime/Base reflection because generated files already include and receive `ReflectionDatabase`. Keep field-kind classification in MetaParser generation so generated C++ contains only the selected call.

## Complexity Tracking

No constitution violations.

## Research

- **Decision**: Add typed `ReflectionDatabase` resolver APIs for scalar, array, `PPtr`, and `PPtr` array fields.
  **Rationale**: The generated code already has `db`, `moduleKey`, and diagnostic context, while MetaParser already has normalized field type text and validation helpers. Selecting the resolver in MetaParser removes repeated prefix parsing from every generated file and avoids runtime string classification.
  **Alternatives considered**: A single `ResolveRegisteredFieldType(TypeKey, const char*, ...)` helper would remove generated-code branches but would still keep container grammar parsing in runtime registration, which the user explicitly rejected.

- **Decision**: Reuse MetaParser's existing array and `PPtr` type-shape helpers when building each field template model.
  **Rationale**: Validation and generation must agree on what counts as `NLS::Array<T>`, `std::vector<T>`, and `PPtr<T>`, so the type-shape decision stays in one MetaParser partial class rather than drifting between template and runtime code.
  **Alternatives considered**: Generating precomputed `TypeKey` constants for every element type would reduce runtime hashing work but would add more generated declarations and is not needed for this cleanup.

## Design Notes

- MetaParser emits:
  - `ResolveRegisteredType` for scalar type names
  - `ResolveRegisteredArrayFieldType` for `NLS::Array<T>` and `std::vector<T>`
  - `ResolveRegisteredPPtrFieldType` for `PPtr<T>`
  - `ResolveRegisteredPPtrArrayFieldType` for array/vector containers of `PPtr<T>`
- Runtime resolver APIs only look up the already-selected scalar or element type and preserve dependency tracking and diagnostics.
- The generated template keeps a small missing-type reporting lambda and uses the MetaParser-selected resolver expression for each field override.
- Tests first assert the generated fixture source no longer has the removed local prefix declarations and now contains typed resolver calls.

## Post-Design Constitution Check

Pass. The design maintains generated-code boundaries, uses normal MetaParser generation, and defines targeted validation.
