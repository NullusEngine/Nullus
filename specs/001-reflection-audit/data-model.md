# Data Model: Reflection Audit And Coverage

## Reflected Type Candidate

- **Purpose**: Represents a runtime type reviewed during the audit for reflection eligibility and current coverage.
- **Fields**:
  - `qualified_name`: Fully qualified type name used in reflection registration
  - `kind`: `class`, `struct`, or `enum`
  - `registration_mode`: `inline`, `auto_property`, `explicit_property`, `external`, or `excluded`
  - `consumers`: Active consumers that need the metadata, such as `inspector`, `serialization`, or `type_lookup`
  - `status`: `correct`, `incomplete`, `incorrect_usage`, or `out_of_scope`
  - `evidence`: Test, code path, or audit note that justifies the decision
- **Relationships**:
  - Owns zero or more `Reflected Member Candidate`
  - Can be linked to one or more `Reflection Consumer`

## Reflected Member Candidate

- **Purpose**: Represents a field, property, method, or static method reviewed for inclusion in the reflected member surface.
- **Fields**:
  - `owner_type`
  - `name`
  - `member_kind`: `field`, `property`, `method`, or `static_method`
  - `exposure_reason`: Why the member should or should not be reflected
  - `source_pattern`: `PROPERTY`, auto-inferred `Get/Set`, `FUNCTION`, or external directive
  - `consumer_requirement`: The active consumer that requires the member, if any
  - `status`: `covered`, `missing`, `intentional_gap`, or `unsupported`
- **Relationships**:
  - Belongs to exactly one `Reflected Type Candidate`

## Reflection Consumer

- **Purpose**: Represents a subsystem that reads runtime metadata and therefore drives registration decisions.
- **Fields**:
  - `name`
  - `path`
  - `consumes_types`: Whether it queries type existence or inheritance
  - `consumes_fields`: Whether it iterates reflected fields or properties
  - `consumes_methods`: Whether it invokes or looks up reflected methods
  - `notes`: Important constraints, such as supported field types or fallback behavior
- **Examples in scope**:
  - `Project/Editor/Panels/Inspector.cpp`
  - `Runtime/Engine/Serialize/GameobjectSerialize.cpp`
  - `Runtime/Engine/GameObject.cpp`

## Reflection Coverage Case

- **Purpose**: Represents one automated regression target that proves a supported reflection pattern still works.
- **Fields**:
  - `name`
  - `layer`: `runtime`, `generation`, or `smoke`
  - `pattern`: `inline_type`, `enum`, `external`, `private_external`, `auto_property`, `explicit_property`, or `consumer_usage`
  - `assertions`: The expected type, member, or generated output facts
  - `source_files`: Code or generated files involved in the case
- **Relationships**:
  - Can validate one or more `Reflected Type Candidate`

## Documentation Pair

- **Purpose**: Represents the maintained Chinese and English reflection workflow guides.
- **Fields**:
  - `topic`
  - `english_path`
  - `chinese_path`
  - `readme_entry`
  - `scope`
  - `parity_status`
- **Relationships**:
  - Summarizes the supported workflow and rules derived from the audit

## Audit Finding

- **Purpose**: Represents a concrete conclusion from the reflection audit.
- **Fields**:
  - `category`: `optimization`, `correct_usage`, `gap`, `documentation`, or `test_coverage`
  - `severity`: `high`, `medium`, or `low`
  - `summary`
  - `evidence`
  - `recommended_action`
- **Relationships**:
  - May reference one or more `Reflected Type Candidate`, `Reflection Consumer`, or `Reflection Coverage Case`
