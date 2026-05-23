# Research: MetaParser Resource Reflection

## Decision: Existing `GENERATED_BODY()` Is The Correct Owned-Class Path

The current generated header template already emits stable type-name and type-key functions, and emits `GetObjectTypeName()` for types that inherit from `NLS::Object` or `NLS::NamedObject`. Rendering resources are Nullus-owned classes, so using the same path as `GameObject`, components, and reflected structs is the lowest-risk SSoT correction.

## Decision: Resource Fields Stay Unreflected

The goal is generated object identity, not broadening serialization or Inspector-editable resource internals. Resource members should stay unreflected unless future specs introduce explicit `PROPERTY()` annotations.

## Decision: External Reflection Keeps Only External/Value Entries

`Runtime/Rendering/ExternalReflection.h` should continue to host types that are not normal owned reflected classes. It should stop manually registering Mesh, Material, Shader, Texture, Texture2D, and TextureCube because generated registration will own those class entries and their pointer variants.

## Alternatives Rejected

- **New lightweight object macro**: unnecessary until `GENERATED_BODY()` proves inadequate.
- **RTTI/typeid lookup**: compiler-specific names are not stable for serialization.
- **Manual external fallback**: duplicates the generated registration responsibility and undermines the rule that owned reflected classes use MetaParser.
