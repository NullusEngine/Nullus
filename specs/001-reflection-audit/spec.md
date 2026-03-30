# Feature Specification: Reflection Audit And Coverage

**Feature Branch**: `001-reflection-audit`  
**Created**: 2026-03-30  
**Status**: Draft  
**Input**: User description: "查看当前反射系统+代码生成方案：1.当前这套系统还有什么可以优化的空间 2.看看当前项目中有没有正确使用这套方案 3.还有什么类，枚举，属性，函数应该注册到反射，归纳下什么样的类应该注册到反射 4.优化反射测试用例的写法，补全反射系统的测试用例。5.总结一下这套方案的使用方法并且更新到文档，并且在ReadME引用。仓库文档统一要中英文两个版本"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Audit Reflection Coverage And Registration Rules (Priority: P1)

As a Nullus maintainer, I need one authoritative audit of the current reflection runtime and MetaParser generation flow so I can see what is working, what is misused, what remains missing, and which kinds of types should be registered in the future.

**Why this priority**: Without a grounded audit, every later reflection change is guesswork. The project needs a clear source of truth before expanding coverage or rewriting tests.

**Independent Test**: Review the audit artifacts and confirm they identify the active reflection paths, current optimization opportunities, confirmed correct usages, confirmed gaps, and the registration rules for classes, enums, properties, and functions that the project depends on today.

**Acceptance Scenarios**:

1. **Given** the current reflection runtime, generator, editor, and serialization code, **When** the audit is complete, **Then** contributors can trace how a reflected declaration becomes generated registration code and runtime metadata.
2. **Given** the current project codebase, **When** the audit reviews reflected consumers and reflected declarations together, **Then** it records both confirmed correct usages and concrete missing or inconsistent registrations.
3. **Given** a new contributor deciding whether to reflect a type, **When** they read the rules from the audit, **Then** they can tell which types should be registered, which members should be exposed, and which types should stay out of reflection.

---

### User Story 2 - Strengthen Automated Reflection Regression Coverage (Priority: P2)

As a contributor changing reflection code or reflected gameplay types, I need reflection tests that are easier to extend and that cover the real registration patterns used in Nullus so regressions are caught before they break editor tooling, serialization, or generated bindings.

**Why this priority**: The current tests verify only a narrow slice of the system and rely heavily on string fragments. Better structured coverage reduces churn and makes reflection changes safer.

**Independent Test**: Run the reflection-related automated tests and confirm they verify runtime registrations, generated bindings, external reflection declarations, property inference, explicit property directives, and any newly registered members introduced by this feature.

**Acceptance Scenarios**:

1. **Given** a reflected runtime type or external reflection declaration, **When** registration or generation changes regress, **Then** the automated tests fail with a targeted assertion instead of only a broad smoke failure.
2. **Given** a contributor adds a new reflected type or property pattern, **When** they extend the reflection test helpers, **Then** they can add focused coverage without duplicating large blocks of setup and assertions.
3. **Given** generated bindings and runtime registration must stay aligned, **When** tests run after generation, **Then** they confirm both the generated artifacts and the runtime database reflect the expected type and member coverage.

---

### User Story 3 - Replace Reflection Gaps That Block Current Consumers (Priority: P3)

As a maintainer of editor and serialization workflows, I need the currently important runtime types to expose the properties and functions that existing reflection consumers already expect so handwritten fallback paths can shrink and reflection usage becomes more consistent.

**Why this priority**: The project already depends on reflection for editor inspection, serialization, and dynamic type-driven behavior. Missing registrations in those surfaces create duplicated manual code and inconsistent behavior.

**Independent Test**: Validate the identified high-value reflected types in automated tests and confirm that the current consumers can rely on the reflected members that were previously missing or incompletely covered.

**Acceptance Scenarios**:

1. **Given** a component whose editable state is already consumed by the Inspector or serialization helpers, **When** the type is reviewed for reflection coverage, **Then** the missing reflected properties and methods required by those consumers are registered.
2. **Given** an existing handwritten fallback path that only exists because reflection data is incomplete, **When** reflection coverage is improved for that type, **Then** the fallback can be reduced or the remaining fallback scope becomes explicit and justified.
3. **Given** a type that is internal-only, unstable, or not consumed through reflection, **When** it is reviewed during the audit, **Then** it is left unregistered and documented as intentionally outside the current reflection boundary.

---

### User Story 4 - Publish A Bilingual Reflection Usage Guide (Priority: P4)

As a contributor onboarding to Nullus, I need a Chinese and English explanation of the reflection workflow, supported declaration patterns, registration rules, and validation expectations, with README links, so I can use the system correctly without reverse-engineering the generator.

**Why this priority**: Reflection is now a maintained workflow in the repository, but the current guidance is fragmented and partially outdated. A bilingual guide lowers onboarding cost and reduces accidental misuse.

**Independent Test**: Open the new documentation from the README, confirm that both language versions exist, and verify they describe the same workflow, supported patterns, constraints, and validation guidance.

**Acceptance Scenarios**:

1. **Given** a contributor reading the README, **When** they look for reflection guidance, **Then** they find direct links to the updated Chinese and English documentation.
2. **Given** a contributor writing a reflected class, enum, property, function, or external declaration, **When** they follow the guide, **Then** they can use the supported macros and validation steps without relying on outdated notes.
3. **Given** the documentation is reviewed after implementation, **When** the Chinese and English versions are compared, **Then** they describe the same supported workflow and limitations with equivalent scope.

### Edge Cases

- What happens when a type is currently consumed by editor or serialization code but is only partially reflected, causing mixed reflected and handwritten fallback behavior?
- What happens when the generator can infer auto-properties from `Get` and `Set` pairs for some types but requires explicit `PROPERTY(...)` directives for others?
- What happens when a reflected type depends on helper types that are compared by `meta::Type` at runtime but do not themselves need full reflection registration?
- What happens when an external reflection declaration is technically valid but exposes members that project consumers should not treat as editable or serializable?
- What happens when the codebase contains legacy reflection guidance, duplicated tests, or documentation with encoding issues that no longer matches the maintained MetaParser flow?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The feature MUST produce one maintained audit that explains the active Nullus reflection runtime and MetaParser generation flow from declaration markers to runtime registration.
- **FR-002**: The audit MUST record confirmed optimization opportunities in the current reflection and code generation approach, including generator behavior, registration patterns, tests, and documentation.
- **FR-003**: The audit MUST compare current reflection declarations against real project consumers and identify which usages are already correct, which are incomplete, and which are intentionally out of scope.
- **FR-004**: The feature MUST define practical registration rules for Nullus classes, enums, properties, and functions based on current editor, serialization, and dynamic runtime usage rather than on blanket “reflect everything” guidance.
- **FR-005**: The feature MUST register any currently important runtime types or members that are already needed by active reflection consumers but are missing from the maintained reflection flow.
- **FR-006**: Newly added or adjusted reflected members MUST preserve the existing generated registration pipeline and MUST NOT rely on hand-edited files under `Runtime/*/Gen/`.
- **FR-007**: Reflection tests MUST be reorganized so contributors can add coverage through shared helpers or clearer structure instead of repeating large assertion blocks.
- **FR-008**: Automated reflection coverage MUST include runtime registration checks for reflected classes, reflected enums, auto-inferred properties, explicit property directives, and external reflection declarations used by the repository.
- **FR-009**: Automated reflection coverage MUST include generated-artifact checks for the maintained registration patterns used in the repository today.
- **FR-010**: Reflection coverage MUST include at least one regression path for special cases already supported by the system, including explicit property directives and private external bindings where applicable.
- **FR-011**: The feature MUST update the reflection usage documentation in both Chinese and English.
- **FR-012**: The updated documentation MUST explain when to use reflected class or struct declarations, when to use external reflection declarations, what kinds of members should be exposed, and how to validate a change.
- **FR-013**: The README MUST link to the updated reflection documentation in both languages.
- **FR-014**: The work MUST keep existing build and test responsibilities intact and use the normal project generation and test flow instead of introducing a parallel reflection workflow.
- **FR-015**: Any support claims made in docs or summaries MUST be limited to the reflection scenarios and test evidence actually validated in this change.

### Key Entities *(include if feature involves data)*

- **Reflection Coverage Audit**: The maintained record of how Nullus reflection currently works, where it is used correctly, where it is incomplete, and what should be improved next.
- **Reflected Runtime Type**: A class, struct, or enum whose metadata is generated and registered so runtime systems such as editor inspection, serialization, or dynamic type-driven behavior can query it.
- **External Reflection Declaration**: A reflection registration declaration for a type that should remain free of inline reflection macros in its own definition.
- **Reflected Member Surface**: The set of properties, fields, methods, and static methods intentionally exposed for runtime consumers.
- **Reflection Consumer**: A project subsystem that reads reflection metadata at runtime, especially editor inspection, serialization, or dynamic component/type creation.
- **Bilingual Reflection Guide**: The Chinese and English documentation pair that describes the supported workflow, usage rules, and validation path for this repository.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The repository contains a completed reflection audit that lists the current pipeline, confirmed correct usages, confirmed gaps, and next-step optimization opportunities in a form a maintainer can review in under 10 minutes.
- **SC-002**: Reflection-related automated tests cover the maintained repository patterns for reflected classes, reflected enums, external reflection declarations, and property registration without requiring large duplicated assertion blocks.
- **SC-003**: At least one currently important reflected runtime type that was previously incomplete now exposes the missing members required by active reflection consumers, and that coverage is asserted by automated tests.
- **SC-004**: Contributors can reach both the Chinese and English reflection workflow guides directly from the README in one navigation step.
- **SC-005**: The updated Chinese and English reflection guides describe equivalent workflow rules, supported declaration patterns, and validation expectations.
- **SC-006**: All final support claims for this feature are backed by targeted generation or unit-test evidence and do not imply unverified platform or runtime behavior.

## Validation Scope *(mandatory)*

- **Validated Targets**: Windows build flow, generated reflection outputs under the normal CMake-driven MetaParser path, targeted reflection unit coverage, and the standalone reflection smoke tool if it remains relevant after the refactor.
- **Validation Evidence**: `NullusUnitTests` reflection-related tests, generated artifact inspection through automated tests, `ReflectionTest` smoke validation if retained, and documentation or README link sanity checks.
- **Out Of Scope / Unverified**: Linux and macOS generation behavior unless explicitly tested during implementation, editor runtime interaction beyond focused reflection consumer evidence, and any claim that one test target proves broader platform parity.

## Assumptions

- The maintained reflection system in scope is the MetaParser-driven flow already integrated into the repository build, not a replacement reflection architecture.
- Types should be registered when current project consumers need runtime metadata for editing, serialization, dynamic creation, or stable scripting-style lookup; types without those needs can remain outside reflection.
- Files under `Runtime/*/Gen/` remain generated output and will only change as a consequence of normal generation, not by hand edits.
- The current repository already contains enough reflection consumers and sample types to derive practical registration rules without inventing hypothetical use cases.
- The documentation refresh may replace or supersede outdated reflection notes, and the README encoding issue can be corrected as part of the bilingual documentation cleanup.
