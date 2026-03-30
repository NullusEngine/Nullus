# Follow-Up Tasks: Reflection Optimization Roadmap

**Input**: `specs/001-reflection-audit/optimization-roadmap.md`  
**Prerequisites**: `reflection-audit.md`, `plan.md`, `Docs/Reflection/ReflectionWorkflow.en.md`, `Docs/Reflection/ReflectionWorkflow.zh-CN.md`

**Purpose**: This file tracks the next iteration of reflection-system optimization work after the completed audit-and-baseline bundle. It does not replace the completed `tasks.md`; it extends the bundle with a follow-up execution surface.

**Tests**: Each wave must carry matching validation. Documentation waves require link and terminology checks. Consumer and test-surface waves require automated reflection validation. MetaParser diagnostics work requires generator-level validation and at least one concrete verification step.

**Current status**: The follow-up roadmap items in this file have now been executed and verified on the current Windows Debug workspace:

- Inspector enum choices now come from reflected enum metadata instead of field-name special cases
- `GameObject::GetComponent(meta::Type)` now uses real type-driven lookup
- Inspector component fallback has been reduced to a minimal generic “No reflected fields” message instead of per-component legacy drawers
- `NullusUnitTests` reflection coverage is now split into runtime and generation buckets instead of two mixed catch-all files
- MetaParser now prints per-type member-discovery summaries for text-parsed reflected classes and structs

## Phase 1: Wave 1 - Rule And Documentation Convergence

**Goal**: Make the maintained docs the only authoritative reflection onboarding path and make the consumer-driven rule set explicit.

**Independent Test**: A contributor can read the maintained reflection docs and answer when to reflect a type, which reflection pattern to use, and what terms describe the current parser and module-entry behavior without consulting code.

- [X] T001 [W1] Add a concise “when a type should be reflected / should not be reflected” rule section to `Docs/Reflection/ReflectionWorkflow.en.md` and `Docs/Reflection/ReflectionWorkflow.zh-CN.md`
- [X] T002 [W1] Add a supported-pattern matrix to `Docs/Reflection/ReflectionWorkflow.en.md` and `Docs/Reflection/ReflectionWorkflow.zh-CN.md`
- [X] T003 [W1] Align `Docs/AIWorkflow.md`, `Runtime/Base/Reflection/MetaParserMigration.md`, and `Runtime/Base/Reflection/ReflectionPhase1.md` with the maintained parser-path and module-entry terminology
- [X] T004 [W1] Search maintained docs and spec artifacts for stale `RegisterReflectionTypes_*` or “CppAst-first” wording and update or explicitly scope each match in place

---

## Phase 2: Wave 2 - Consumer Coverage Inventory And Gap Closure Preparation

**Goal**: Turn the current ad hoc reflection gaps into a maintained consumer-by-consumer matrix and a ranked gap list.

**Independent Test**: A maintainer can open `reflection-audit.md` and tell which consumers rely on reflection today, which types still use fallback paths, which gaps are ready to fix, and which remain blocked or intentionally out of scope.

- [X] T005 [W2] Add a dedicated consumer matrix section to `specs/001-reflection-audit/reflection-audit.md` covering Inspector, serialization, and `meta::Type`-driven flows
- [X] T006 [P] [W2] Audit `Project/Editor/Panels/Inspector.cpp` and record every remaining reflection-related fallback or manual branch in `specs/001-reflection-audit/reflection-audit.md`
- [X] T007 [P] [W2] Audit `Runtime/Engine/Serialize/GameobjectSerialize.cpp` and related serialization-facing reflected data in `specs/001-reflection-audit/reflection-audit.md`
- [X] T008 [P] [W2] Audit `Runtime/Engine/GameObject.cpp` and related `meta::Type`-driven creation/query paths in `specs/001-reflection-audit/reflection-audit.md`
- [X] T009 [W2] Classify each discovered gap in `specs/001-reflection-audit/reflection-audit.md` as `ready`, `blocked`, or `intentional`, with the exact blocker or reason
- [X] T010 [W2] Add a ranked “next consumer-facing fixes” shortlist to `specs/001-reflection-audit/reflection-audit.md` and `specs/001-reflection-audit/optimization-roadmap.md`

---

## Phase 3: Wave 3 - Test Surface Reorganization

**Goal**: Make reflection validation pattern-based and reduce duplication between unit-level and smoke-level coverage.

**Independent Test**: A contributor can add or update coverage for a supported reflection pattern without copying a large manual assertion list, and the repository docs clearly explain what each test layer is responsible for.

- [X] T011 [W3] Split runtime reflection coverage into pattern-oriented test files under `Tests/Unit/` or document and implement a clear pattern-oriented section structure in `Tests/Unit/ReflectionRuntimeTests.cpp`
- [X] T012 [W3] Reorganize generated-output coverage in `Tests/Unit/MetaParserGenerationTests.cpp` around supported reflection patterns instead of mixed type-only fragments
- [X] T013 [W3] Narrow `Tools/ReflectionTest/src/main.cpp` to smoke-only expectations once detailed coverage is clearly owned by unit tests
- [X] T014 [W3] Update `Docs/Testing.md` to define the responsibilities of `NullusUnitTests`, generation coverage, and `ReflectionTest`
- [X] T015 [W3] Re-run the reflection validation flow and record whether the overlap between `NullusUnitTests` and `ReflectionTest` has been reduced as intended

---

## Phase 4: Wave 4 - MetaParser Diagnostics And Guardrails

**Goal**: Make the generator easier to reason about and make failures more actionable.

**Independent Test**: A maintainer can tell which parser route was used for a header and can read generator failures that recommend the next action instead of only stating the failure.

- [X] T016 [W4] Add parser-route diagnostics in `Tools/MetaParser/src/MetaParserTool.Core.cs` and related parser entrypoints so each reflected header reports which parsing path was used
- [X] T017 [W4] Improve `Tools/MetaParser/src/MetaParserTool.Validation.cs` error messages for unsupported reflected value types, pointer-property rejection, and naming-pattern mismatches
- [X] T018 [W4] Add member-discovery diagnostics or summaries in `Tools/MetaParser/src/MetaParserTool.MemberExtraction.cs` and/or `Tools/MetaParser/src/MetaParserTool.TextParser.cs`
- [X] T019 [W4] Update `Docs/Reflection/ReflectionWorkflow.en.md`, `Docs/Reflection/ReflectionWorkflow.zh-CN.md`, and `specs/001-reflection-audit/quickstart.md` with the new diagnostics and verification expectations

---

## Dependencies & Recommended Order

### Wave Dependencies

- **Wave 1**: Can start immediately
- **Wave 2**: Should start after Wave 1 terminology is stable
- **Wave 3**: Benefits from the Wave 2 inventory, because test buckets should reflect real consumer categories and supported patterns
- **Wave 4**: Best done after Wave 3 so diagnostics align with the pattern-based test surface

### Priority Order

1. Wave 1 - rule and documentation convergence
2. Wave 2 - consumer coverage inventory
3. Wave 3 - test surface reorganization
4. Wave 4 - MetaParser diagnostics and guardrails

### Parallel Opportunities

- `T006`, `T007`, and `T008` can run in parallel
- `T011` and `T012` can run in parallel once the new pattern buckets are agreed
- `T016` and `T017` can run in parallel if they do not touch the same helper code

## Suggested Iteration Strategy

### Iteration A

- Complete Wave 1
- Complete the Wave 2 consumer matrix and classification
- Stop and choose the next concrete consumer-facing fix from the ranked shortlist

### Iteration B

- Execute the top-ranked consumer fix from the shortlist
- Start Wave 3 and reduce test-layer duplication

### Iteration C

- Complete Wave 4 diagnostics
- Update docs and quickstart verification

## Notes

- This file is intentionally forward-looking; leave items unchecked until that work is actually executed.
- If a Wave 2 shortlist item becomes large enough to span multiple subsystems, give it its own spec bundle rather than overloading this one.
- Keep all future generated-file changes sourced from declarations, generator code, or validation changes only.
