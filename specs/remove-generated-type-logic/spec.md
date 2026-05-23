# Feature Specification: Remove Generated Type Logic

**Feature Branch**: `remove-generated-type-logic`  
**Created**: 2026-05-22  
**Status**: Draft  
**Input**: User description: "不要在生成代码里做逻辑判断"

## User Scenarios & Testing

### User Story 1 - Keep Generated Reflection Thin (Priority: P1)

As a Nullus maintainer, I want MetaParser generated registration files to avoid embedding container and pointer type parsing logic so generated output stays declarative and easier to inspect.

**Why this priority**: The active complaint is about logic inside generated files, and this is the visible behavior that must change.

**Independent Test**: Generate or inspect a reflected fixture with `std::vector<T>` and `NLS::Array<T>` fields and verify the generated source uses MetaParser-selected typed resolver calls without local `arrayPrefix` or `vectorPrefix` checks.

**Acceptance Scenarios**:

1. **Given** a reflected type with array/vector fields, **When** MetaParser emits its `.generated.cpp`, **Then** the source does not contain local prefix parsing branches for array/vector field types.
2. **Given** generated registration code needs a field type, **When** it registers a field, **Then** MetaParser has already selected the scalar, array, `PPtr`, or `PPtr` array resolver call.

---

### User Story 2 - Preserve Runtime Reflection Semantics (Priority: P2)

As an editor and serialization consumer, I need reflected container fields to continue reporting the correct array element type so Inspector and object graph serialization behavior does not regress.

**Why this priority**: Moving logic out of generated code must not change existing reflection behavior.

**Independent Test**: Existing reflection tests that assert `field.GetType().IsArray()` and `GetArrayType()` for `NLS::Array<T>` and `std::vector<T>` fields continue to pass.

**Acceptance Scenarios**:

1. **Given** a reflected `NLS::Array<T>` field, **When** runtime reflection is initialized, **Then** the field type is valid, marked as an array, and its array type is `T`.
2. **Given** a reflected `std::vector<T>` field, **When** runtime reflection is initialized, **Then** the field type is valid, marked as an array, and its array type is `T`.
3. **Given** a reflected `PPtr<T>` field or array of `PPtr<T>`, **When** runtime reflection is initialized, **Then** the `PPtr` type dependency and allocation behavior remains unchanged.

### Edge Cases

- Missing referenced field element types still produce the same warning diagnostics instead of silently succeeding.
- Generated files under `Runtime/*/Gen/` remain generated outputs and are updated only through MetaParser.
- Runtime helper calls must handle scalar, array, `PPtr`, and `PPtr` array field types exactly as before without parsing container syntax from a full C++ field type string.

## Requirements

### Functional Requirements

- **FR-001**: Generated `.generated.cpp` files MUST NOT embed local string-prefix decision logic for `NLS::Array<...>` or `std::vector<...>` field type resolution.
- **FR-002**: MetaParser MUST classify each reflected field type during generation and emit the appropriate typed resolver call.
- **FR-003**: Runtime reflection resolver APIs MUST preserve existing scalar, array, vector, and `PPtr` field type resolution behavior without reparsing full container type strings.
- **FR-004**: The resolver APIs MUST preserve module dependency tracking for referenced field types.
- **FR-005**: The change MUST include an automated regression test that fails when generated fixture sources include the removed local prefix logic.
- **FR-006**: The change MUST not hand-edit `Runtime/*/Gen/` files; generated outputs must come from the MetaParser flow.

### Key Entities

- **Generated Reflection Source**: A MetaParser output file that registers reflected types, fields, and methods.
- **Typed Field Type Resolver**: Runtime reflection helper selected by MetaParser for scalar, array, `PPtr`, or `PPtr` array field type lookup.
- **Reflection Field Type**: The runtime `meta::Type` stored on a reflected `Field`.

## Success Criteria

### Measurable Outcomes

- **SC-001**: The vector/array MetaParser generation regression test passes and confirms zero local `arrayPrefix` and `vectorPrefix` declarations in the generated fixture source.
- **SC-002**: Existing reflection/editor tests that assert array field classification continue to pass.
- **SC-003**: Generated source call sites for field type overrides use MetaParser-selected typed resolver calls rather than duplicating resolver lambdas or parsing full field type strings.

## Assumptions

- The appropriate shared location for lookup APIs is under `Runtime/Base/Reflection` because generated registration already depends on Runtime/Base reflection APIs.
- The generated source may still contain simple calls and scalar registration data; the forbidden logic is the field type parsing policy.
- Existing `PPtr` special handling remains required because serialization depends on reflected object references.
