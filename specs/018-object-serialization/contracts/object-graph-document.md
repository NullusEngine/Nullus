# Contract: Object Graph Document

## Purpose

Defines the stable text document shape for scene and prefab Object Graph files.

## Common Header

Every document contains:

```json
{
  "format": "Nullus.ObjectGraph.Scene",
  "version": 1,
  "documentId": "7b6f2d2e-9f04-48d5-82c8-24d52e9b3a41",
  "root": "5d11f818-0bda-4e3d-94d7-2b2fb1bc652b",
  "dependencies": [],
  "objects": [],
  "overrides": []
}
```

Rules:

- `format` identifies the schema and must be one of the supported Object Graph formats.
- `version` is the schema version for migration.
- `documentId`, `root`, object IDs, and asset IDs use canonical lowercase GUID strings.
- `dependencies` is optional for documents with no external dependencies.
- `overrides` is optional for documents with no prefab or patch operations.

## Scene Document

`format` is `Nullus.ObjectGraph.Scene`.

Root object:

- Must be type `NLS::Engine::SceneSystem::Scene` or its stable type alias.
- Must own scene game objects through an owned-reference collection.

Example:

```json
{
  "format": "Nullus.ObjectGraph.Scene",
  "version": 1,
  "documentId": "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa",
  "root": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
  "objects": [
    {
      "id": "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb",
      "type": "NLS::Engine::SceneSystem::Scene",
      "debugName": "Main Scene",
      "state": "Alive",
      "properties": {
        "gameObjects": [
          { "$owned": "cccccccc-cccc-4ccc-cccc-cccccccccccc" }
        ]
      }
    },
    {
      "id": "cccccccc-cccc-4ccc-cccc-cccccccccccc",
      "type": "NLS::Engine::GameObject",
      "debugName": "Player",
      "debugPath": "/Player",
      "state": "Alive",
      "properties": {
        "name": "Player",
        "tag": "Player",
        "active": true,
        "parent": null,
        "components": [
          { "$owned": "dddddddd-dddd-4ddd-dddd-dddddddddddd" }
        ]
      }
    }
  ]
}
```

## Prefab Document

`format` is `Nullus.ObjectGraph.Prefab`.

Root object:

- Must be a `GameObject` or stable type alias for a prefab-compatible game object type.
- May reference a base prefab through `basePrefab`.
- May contain overrides for prefab variants.

Example:

```json
{
  "format": "Nullus.ObjectGraph.Prefab",
  "version": 1,
  "documentId": "11111111-1111-4111-8111-111111111111",
  "root": "22222222-2222-4222-8222-222222222222",
  "basePrefab": { "$asset": "33333333-3333-4333-8333-333333333333" },
  "objects": [],
  "overrides": [
    {
      "op": "replaceProperty",
      "target": "22222222-2222-4222-8222-222222222222",
      "property": "name",
      "value": "Variant Root"
    }
  ]
}
```

## Reference Values

Owned reference:

```json
{ "$owned": "object-guid" }
```

Object reference:

```json
{ "$ref": "object-guid" }
```

Asset reference:

```json
{
  "$asset": "asset-guid",
  "type": "Material",
  "pathHint": "Assets/Materials/Default.mat"
}
```

Rules:

- `$owned` establishes lifecycle and serialization ownership.
- `$ref` establishes a non-owning object relationship.
- `$asset` points to an external asset and must preserve the GUID even when missing.

## Patch Operations

Replace property:

```json
{
  "op": "replaceProperty",
  "target": "object-guid",
  "property": "localPosition",
  "value": { "x": 1.0, "y": 2.0, "z": 3.0 }
}
```

Insert owned object:

```json
{
  "op": "insertOwned",
  "owner": "owner-guid",
  "property": "components",
  "index": 2,
  "object": "new-object-guid"
}
```

Remove owned object:

```json
{
  "op": "removeOwned",
  "owner": "owner-guid",
  "property": "components",
  "object": "component-guid"
}
```

Move owned object:

```json
{
  "op": "moveOwned",
  "owner": "owner-guid",
  "property": "components",
  "object": "component-guid",
  "index": 0
}
```

## Deterministic Output Rules

- GUID strings are lowercase canonical UUID strings.
- Object records are sorted by a stable policy defined by the writer.
- Property order is stable and deterministic.
- Floating point values use a round-trip safe representation.
- Paths use `/` separators.
- Output is UTF-8.
- No persistent `worldID` field is emitted.
