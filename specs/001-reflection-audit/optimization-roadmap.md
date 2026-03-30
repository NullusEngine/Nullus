# Reflection Optimization Roadmap

## Purpose

This roadmap turns the optimization opportunities identified in `reflection-audit.md` into an executable follow-up plan.

It is intentionally staged. The goal is not to do a broad reflection rewrite, but to improve the current MetaParser-based workflow in the order that creates the highest value with the lowest risk.

The actionable task breakdown for this roadmap is tracked in:

- `specs/001-reflection-audit/optimization-tasks.md`

## Optimization Principles

1. **Consumer-driven scope first**
   - Only expand reflection when editor inspection, serialization, `meta::Type`-driven lookup/creation, or already-reflected value types need the metadata.

2. **Generated-file boundaries remain strict**
   - All behavior changes must come from declaration inputs, generator logic, tests, or maintained docs.
   - Never hand-edit `Runtime/*/Gen/`.

3. **Patterns before one-off fixes**
   - Prefer improving shared patterns, test helpers, and validation rules before adding isolated type-specific special cases.

4. **Incremental validation**
   - Each wave must produce its own evidence.
   - One completed wave should not depend on speculative work from later waves.

## Recommended Path

### Wave 1 - Rule And Documentation Convergence

**Why first**
This is the highest-leverage, lowest-risk improvement. The audit already showed that implementation truth, naming, and documentation drifted apart.

**Goals**

- Turn “consumer-driven reflection” from an implicit habit into an explicit repository rule.
- Standardize the maintained terminology for parser behavior, registration entrypoints, and supported declaration patterns.
- Make the maintained docs the only authoritative onboarding path.

**Deliverables**

- A short reflection inclusion rule set:
  - when a type should be reflected
  - what should be reflected
  - what should stay out
- A supported-pattern matrix:
  - inline type
  - enum
  - auto property
  - explicit property
  - external reflection
  - private external reflection
- Direct links from top-level docs to the maintained guides, with no legacy reflection entry files left in the path.

**Primary files**

- `Docs/Reflection/ReflectionWorkflow.en.md`
- `Docs/Reflection/ReflectionWorkflow.zh-CN.md`
- `README.md`
- `README.en.md`
- `Runtime/Base/Reflection/MetaParserMigration.md`
- `Runtime/Base/Reflection/ReflectionPhase1.md`

**Validation**

- All top-level reflection doc links point directly to `Docs/Reflection/`
- No remaining docs describe the runtime entrypoint as `RegisterReflectionTypes_*`
- No remaining docs describe reflected type bodies as “CppAst-first” if that is not true for current usage

### Wave 2 - Consumer Coverage Inventory And Gap Closure

**Why second**
Once the rules are explicit, the next best return comes from systematically finding where reflection consumers still rely on manual paths.

**Goals**

- Build a consumer-by-consumer coverage inventory
- Classify each gap as:
  - ready to fix now
  - blocked by unsupported field type
  - intentionally out of scope
- Continue removing high-value manual fallbacks where reflection data is already sufficient

**Recommended focus order**

1. Inspector-editable component state
2. Serialization-facing data structures
3. `meta::Type`-driven creation and lookup paths
4. Lower-value or higher-risk resource-pointer surfaces last

**Recently completed from the shortlist**

1. Inspector enum rendering now uses enum metadata instead of field-name special cases
2. `GameObject::GetComponent(meta::Type)` now uses real type-driven lookup
3. MetaParser now reports parser routes in build logs and gives a more actionable hint for unqualified `Array<...>` reflected field types

**Current remaining shortlist**

1. Continue shrinking Inspector fallback coverage by expanding generic reflected field support
2. Define a safe editing strategy for resource-pointer-heavy reflected surfaces
3. Complete the remaining validation and ownership cleanup around `ReflectionTest` versus `NullusUnitTests`

**Deliverables**

- A maintained consumer matrix in the spec bundle
- A gap table with:
  - type
  - consumer
  - needed members
  - current blocker
  - recommended action
- One or more targeted consumer fixes similar in profile to `MaterialRenderer`

**Primary files**

- `specs/001-reflection-audit/reflection-audit.md`
- `Project/Editor/Panels/Inspector.cpp`
- `Runtime/Engine/Serialize/GameobjectSerialize.cpp`
- selected reflected headers under `Runtime/Engine/` or `Runtime/Math/`

**Validation**

- Every newly fixed consumer gap has matching runtime and generation assertions
- Every intentionally skipped type has a stated reason
- Manual fallback code exists only where reflection coverage is still truly insufficient

### Wave 3 - Test Surface Reorganization

**Why third**
The audit found that the current tests are useful but uneven. The next step is to turn them into a stable pattern-based regression surface.

**Goals**

- Separate test responsibility clearly across:
  - runtime unit checks
  - generated-artifact checks
  - smoke validation
- Organize tests by supported reflection pattern instead of by ad hoc type lists
- Make failures identify the broken pattern, not just the broken type

**Target test buckets**

- inline reflected type
- reflected enum
- auto property
- explicit property
- external reflection
- private external reflection
- consumer-facing coverage

**Deliverables**

- Shared test helpers for runtime reflection assertions
- Shared helpers or conventions for generated-fragment checks
- A smoke layer that stays intentionally thin instead of duplicating the full unit suite

**Primary files**

- `Tests/Unit/ReflectionTestUtils.h`
- `Tests/Unit/ReflectionRuntimeCoreTests.cpp`
- `Tests/Unit/ReflectionRuntimeEngineTests.cpp`
- `Tests/Unit/MetaParserGenerationModuleTests.cpp`
- `Tests/Unit/MetaParserGenerationEngineTests.cpp`
- `Tests/Unit/MetaParserGenerationDataTests.cpp`
- `Tools/ReflectionTest/src/main.cpp`

**Validation**

- Adding a new reflection pattern test does not require copying a large manual assertion block
- Runtime failures clearly identify missing type/member coverage
- Generation failures clearly identify which supported output pattern regressed

### Wave 4 - MetaParser Diagnostics And Guardrails

**Why fourth**
After docs, consumers, and tests are cleaner, the next best improvement is making the generator easier to reason about and harder to misuse.

**Goals**

- Surface the actual parser route in diagnostics
- Make field-type validation and auto-property rejection reasons more actionable
- Help maintainers understand why a member was discovered, ignored, or rejected

**Candidate improvements**

- Log whether each header used:
  - text parsing
  - external declaration parsing
  - `CppAst`
- Improve validation messaging for:
  - unsupported reflected value types
  - pointer-property rejection
  - auto-property naming mismatches
  - explicit-property requirements
- Add optional developer-focused summaries for discovered fields and methods

**Primary files**

- `Tools/MetaParser/src/MetaParserTool.Core.cs`
- `Tools/MetaParser/src/MetaParserTool.Validation.cs`
- `Tools/MetaParser/src/MetaParserTool.MemberExtraction.cs`
- `Tools/MetaParser/src/MetaParserTool.TextParser.cs`

**Validation**

- Maintainers can tell why a header used a given parser route
- Generator failures recommend the next action instead of only stating what failed
- New diagnostics do not change generated outputs unless behavior was intentionally updated

## Priority Matrix

### P0

- Wave 1 rule and doc convergence
- Wave 2 consumer coverage inventory
- Wave 3 test responsibility cleanup

### P1

- Additional consumer-facing reflection gap fixes after the inventory exists
- Shared pattern-based regression expansion

### P2

- Wave 4 MetaParser diagnostics and guardrails
- Broader README or historical doc cleanup outside the maintained reflection scope

## Suggested Execution Model

### Iteration A

- Finish doc convergence
- lock the reflection inclusion rules
- produce the consumer matrix

### Iteration B

- fix the next 1-2 highest-value consumer gaps
- extend tests with the matching pattern buckets

### Iteration C

- reduce overlap between `NullusUnitTests` and `ReflectionTest`
- improve MetaParser diagnostics

## Exit Criteria

This roadmap can be considered successfully executed when:

- contributors can determine whether a type should be reflected without reading generator internals
- direct links point only to the maintained reflection guides
- the next consumer-facing reflection gaps are tracked explicitly instead of discovered ad hoc
- tests are organized by reflection pattern and consumer outcome
- MetaParser failures tell maintainers what to change next

## Non-Goals

- Replacing the current reflection architecture
- Reverting to a legacy parser path
- Reflecting all public types “for completeness”
- Exposing resource-pointer or lifecycle-heavy state before current consumers can safely use it
