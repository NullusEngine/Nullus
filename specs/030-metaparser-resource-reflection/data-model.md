# Data Model: MetaParser Resource Reflection

## Owned Reflected Resource Class

Represents a Nullus-owned rendering resource class that must be reflected through MetaParser.

Fields:

- `qualifiedName`: Stable fully-qualified reflection name.
- `headerPath`: Header containing the class declaration.
- `requiresObjectBridge`: True for classes deriving from `NLS::Object` or `NLS::NamedObject`.
- `manualBridgeForbidden`: True for all classes in this migration.

Instances:

- `NLS::Render::Resources::Mesh`
- `NLS::Render::Resources::Material`
- `NLS::Render::Resources::Shader`
- `NLS::Render::Resources::Texture`
- `NLS::Render::Resources::Texture2D`
- `NLS::Render::Resources::TextureCube`

## Generated Type Identity

Represents the MetaParser output expected for an owned reflected class.

Fields:

- `StaticMetaTypeName`
- `StaticMetaTypeKey`
- `GetObjectTypeName`
- class type registration
- pointer type registration
- const-pointer type registration

## External Reflection Boundary

Represents which reflection entries remain outside generated class reflection.

Fields:

- `externalValueTypes`: Types that remain externally reflected.
- `ownedClassTypes`: Types that must not be externally registered.

Validation:

- `Bounds` remains externally reflected.
- Migrated resource classes are absent from manual external type-name declarations and manual resource registration calls.
