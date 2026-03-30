# Implementation Plan: Reflection Audit And Coverage

**Branch**: `001-reflection-audit` | **Date**: 2026-03-30 | **Spec**: `/specs/001-reflection-audit/spec.md`
**Input**: Feature specification from `/specs/001-reflection-audit/spec.md`

## Summary

Audit the current Nullus reflection runtime and MetaParser flow, align current registration coverage with active consumers, improve reflection regression tests around real repository patterns, and publish a bilingual reflection usage guide with README entry points.

The implementation stays inside the existing MetaParser-driven workflow. It will tighten consumer-driven reflection coverage, improve automated validation, and consolidate outdated documentation without introducing a parallel reflection architecture or hand-editing generated files.

## Technical Context

**Language/Version**: C++20 runtime/editor code, C#/.NET 8 MetaParser tooling, Markdown documentation, CMake  
**Primary Dependencies**: Nullus runtime modules, `Tools/MetaParser`, CppAst.NET/ClangSharp/libclang runtime, GoogleTest, existing reflection runtime under `Runtime/Base/Reflection`  
**Storage**: Repository source files, generated reflection outputs under `Runtime/*/Gen/`, committed spec bundle under `specs/001-reflection-audit/`  
**Testing**: `NullusUnitTests`, `Tools/ReflectionTest`, normal CMake-driven MetaParser generation, focused generated-artifact assertions, documentation and README link sanity checks  
**Target Platform**: Windows-first validation through the current build flow; cross-platform generation claims remain unverified unless explicitly tested  
**Project Type**: Native engine/editor repository with integrated code generation tooling  
**Performance Goals**: No meaningful regression to current reflection generation or reflection-based runtime behavior; keep editor and serialization consumers on the maintained reflection path  
**Constraints**: Do not hand-edit `Runtime/*/Gen/`; keep the existing build/test workflow; avoid widening reflection scope beyond active project consumers; keep `Editor` and `Game` runtime viability intact; documentation must ship in Chinese and English  
**Scale/Scope**: Reflection runtime, MetaParser-generated registration coverage, targeted Engine/Math/Base declarations, reflection-focused tests, and repository documentation/README updates

## Constitution Check

This plan satisfies the current constitution if it:

- Keeps all planning artifacts inside `specs/001-reflection-audit/` as the single source of truth for this change
- Preserves the generated-code boundary by changing reflection declarations or generator behavior only through maintained source inputs and normal generation
- Limits validation claims to the exact Windows build and test evidence gathered during implementation
- Avoids claiming platform parity or runtime behavior beyond the scenarios actually exercised
- Preserves existing `Editor` and `Game` viability by keeping the current runtime flow intact and only tightening reflection-backed surfaces

No constitution violations are required by this plan. The main risks are incomplete coverage decisions and test/documentation drift, not process exceptions.

## Project Structure

### Documentation (this feature)

```text
specs/001-reflection-audit/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/
├── Base/
│   └── Reflection/
├── Engine/
│   ├── Components/
│   ├── SceneSystem/
│   └── Serialize/
├── Math/
│   └── ExternalReflection.h
└── Rendering/
    ├── Geometry/
    └── Settings/

Project/
└── Editor/
    └── Panels/

Tests/
└── Unit/

Tools/
├── MetaParser/
└── ReflectionTest/

Docs/
├── Reflection/              # new bilingual home for maintained reflection docs
└── AIWorkflow.md

README.md
```

**Structure Decision**: Keep implementation inside the existing reflection/runtime/editor/test layout. Add a dedicated `Docs/Reflection/` bilingual documentation pair, update the README to point there, keep audit and planning artifacts in `specs/001-reflection-audit/`, and limit code changes to reflection declarations, reflection consumers, and reflection-focused tests.

## Current-State Findings Driving The Plan

### 1. The Maintained Reflection Path Is Already MetaParser-Centric

- Runtime modules register reflection through generated `MetaGenerated.*` entry points.
- Reflected class bodies are parsed through the text-based `GENERATED_BODY()` path, with external declarations handled through `MetaExternal` plus `REFLECT_EXTERNAL`.
- The runtime database and `Type` API are already consumed by editor inspection, serialization helpers, and type-driven component workflows.

### 2. Registration Completeness Is Consumer-Driven, Not Inheritance-Driven

- Types matter when editor inspection, serialization, or dynamic lookup already depends on runtime metadata.
- Some current reflected types are complete enough for those consumers.
- Some types still rely on handwritten fallbacks even though the reflection system already supports the needed patterns.

### 3. Current Tests Cover Important Paths But Leave Gaps

- Runtime registration tests verify several generated types and enums.
- Generation tests verify representative generated fragments.
- The standalone reflection smoke tool covers additional cases such as private external bindings.
- Coverage is still uneven for external serialization types, private bindings in unit tests, and the “consumer actually uses reflected members” story.

### 4. Current Documentation Is Split And Partly Outdated

- Reflection guidance exists in multiple places with overlapping scope.
- The README reflection section currently has encoding issues and is not a reliable entry point.
- The repository now needs one maintained bilingual workflow guide instead of scattered notes.

## Design Decisions

### Decision 1 - Keep The Existing Reflection Architecture

This feature does not replace the current reflection runtime or MetaParser architecture. It audits and strengthens the maintained path that already exists.

### Decision 2 - Use Consumer-Driven Registration Rules

Registration decisions will be based on active consumers:

- `Inspector` and other editor surfaces that need editable metadata
- serialization helpers that walk reflected fields
- runtime systems that create or query types through `meta::Type`

Types without those needs remain out of scope unless they are already part of a supported reflection pattern sample.

### Decision 3 - Prefer Existing Registration Patterns Before Adding New Ones

When a type fits the existing patterns, the feature will reuse them in this order:

1. `CLASS` / `STRUCT` / `ENUM` plus `GENERATED_BODY()` for owned runtime types
2. auto-property inference from `FUNCTION()`-marked `Get`/`Set` pairs when names line up cleanly
3. explicit `PROPERTY(...)` directives when the exposed property name or accessor pattern needs manual control
4. `MetaExternal` plus `REFLECT_EXTERNAL` for types that should remain free of inline reflection macros

### Decision 4 - Make Tests More Data-Driven And Pattern-Oriented

Reflection tests will be organized around supported patterns and consumer outcomes instead of only ad hoc string fragments. Generated-file assertions remain useful for template invariants, but runtime registration and consumer-relevant behavior become the primary regression guard.

### Decision 5 - Consolidate Documentation Into One Bilingual Workflow Guide

The maintained guidance will live under `Docs/Reflection/` in paired Chinese and English files. The README becomes the entry point. Older reflection entry files can be removed once direct links point to the maintained guide.

## Planned Workstreams

### Workstream A - Audit And Registration Rules

Scope:

- trace the current reflection declaration-to-registration flow
- review current project consumers against current reflected surfaces
- record correct usages, gaps, and registration heuristics

Primary files:

- `Runtime/Base/Reflection/`
- `Tools/MetaParser/src/`
- `Project/Editor/Panels/Inspector.cpp`
- `Runtime/Engine/Serialize/GameobjectSerialize.cpp`
- `specs/001-reflection-audit/`

### Workstream B - Consumer-Facing Coverage Fixes

Scope:

- register missing high-value members needed by active consumers
- remove or narrow handwritten reflection fallbacks where reflection data becomes sufficient
- regenerate outputs through the normal build flow

Primary files:

- `Runtime/Engine/Components/MaterialRenderer.h`
- `Project/Editor/Panels/Inspector.cpp`
- any additional reflected declaration files confirmed by the audit

### Workstream C - Reflection Test Restructure And Expansion

Scope:

- improve test readability and extensibility
- cover runtime registration, generated outputs, external reflection, explicit properties, auto properties, and private external bindings
- decide whether `Tools/ReflectionTest` remains as a smoke layer or should be narrowed

Primary files:

- `Tests/Unit/ReflectionRuntimeTests.cpp`
- `Tests/Unit/MetaParserGenerationTests.cpp`
- `Tests/Unit/CMakeLists.txt`
- `Tools/ReflectionTest/src/main.cpp`

### Workstream D - Bilingual Documentation And README Entry Points

Scope:

- write a maintained English and Chinese reflection workflow guide
- explain supported declaration patterns, registration rules, test expectations, and generator constraints
- repair the README reflection entry point and point it to both documents

Primary files:

- `Docs/Reflection/ReflectionWorkflow.en.md`
- `Docs/Reflection/ReflectionWorkflow.zh-CN.md`
- `README.md`
- existing reflection docs that need redirect notes or scope cleanup

## Phase Outline

### Phase 0 - Baseline Audit

- inspect the current reflection runtime, MetaParser inputs, generated outputs, and active consumers
- identify confirmed correct usages, confirmed gaps, and the minimal code changes needed for active consumers
- turn those findings into the audit deliverables in this spec bundle

### Phase 1 - Targeted Coverage Fixes

- update reflected declarations for the selected missing consumer-facing members
- adjust the consumer code to use reflection where the gap is closed
- regenerate reflection outputs through the normal build flow

### Phase 2 - Test Restructure And Expansion

- refactor reflection test helpers to reduce duplication and clarify intent
- expand runtime and generation coverage for maintained repository patterns
- keep a lightweight smoke layer where it still adds value

### Phase 3 - Bilingual Documentation Refresh

- write the maintained English and Chinese reflection guides
- update or redirect outdated reflection notes where needed
- fix README reflection navigation and encoding issues relevant to this feature

### Phase 4 - Validation And Self-Review

- run targeted reflection-related build and test commands
- confirm generated outputs changed only through the normal generation flow
- record exact evidence and explicit unverified areas in the final summary

## Validation Strategy

### Automated Validation

- configure and build the affected targets through CMake so MetaParser runs in the real build flow
- run `NullusUnitTests` with the reflection-related suite
- run `ReflectionTest` if retained as a smoke layer after the test refactor

### Generated Output Validation

- verify changed generated files are the consequence of updated reflection declarations or maintained generator behavior
- use automated tests to assert representative generated fragments for explicit properties, auto properties, enums, and external reflection declarations

### Consumer Validation

- confirm the selected editor or serialization consumer now relies on reflected metadata for the newly covered members
- keep claims limited to the exact consumer paths exercised by tests or code inspection

### Documentation Validation

- confirm both bilingual reflection guides exist and cover the same maintained workflow
- confirm the README links to both language versions cleanly

## Risks And Mitigations

- **Risk**: A newly reflected member exposes a type that current consumers cannot render or serialize safely.
  **Mitigation**: Restrict new registration to types already handled by active consumers or pair the registration with a focused consumer adjustment.

- **Risk**: Generated-file churn obscures the real source change.
  **Mitigation**: Keep edits in declaration inputs or generator sources only, then explain each generated diff as a direct result of those inputs.

- **Risk**: Unit tests still duplicate smoke assertions and remain hard to extend.
  **Mitigation**: Introduce shared expectation helpers and pattern-based coverage instead of copying large assertion lists.

- **Risk**: Documentation improves in one language but drifts in the other.
  **Mitigation**: Treat the English and Chinese guides as one paired deliverable and validate scope parity before completion.

- **Risk**: The audit overreaches and recommends reflecting internal types that do not serve current consumers.
  **Mitigation**: Keep registration rules explicitly consumer-driven and document intentional exclusions.

## Follow-Up Optimization Roadmap

The audit has already identified a next-step optimization program beyond the work completed in this bundle. That follow-up is tracked in:

- `specs/001-reflection-audit/optimization-roadmap.md`
- `specs/001-reflection-audit/optimization-tasks.md`

The recommended order is:

1. Rule and documentation convergence
2. Consumer coverage inventory and gap closure
3. Test surface reorganization
4. MetaParser diagnostics and guardrails

This keeps the next iteration focused on the highest-value improvements first, instead of jumping directly into generator-heavy refactoring.

## Complexity Tracking

No constitution exception is planned. The feature is cross-cutting, but it stays within the repository’s normal reflection workflow: one committed spec bundle, no manual edits to generated output, targeted validation, and incremental updates to code, tests, and documentation.
