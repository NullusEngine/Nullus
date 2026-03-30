# Quickstart: Reflection Audit And Coverage

## 1. Build through the normal MetaParser path

Use the normal CMake flow so reflection generation runs exactly the way the repository expects:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target NullusUnitTests ReflectionTest -- /m:1
```

## 2. Run the reflection-focused validation targets

Run the unit suite and the standalone smoke tool used by this feature:

```powershell
ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests
.\build\bin\Debug\ReflectionTest.exe
```

## 3. Check the generated outputs only as consequences of source changes

When reflected declarations change, inspect the corresponding generated files under `Runtime/*/Gen/` and confirm they match the intended declaration change. Do not hand-edit generated files.

Also inspect the MetaParser log while building:

- confirm that each reflected header reports its parser route
- use those route messages to check whether a header is currently using:
  - `text-top-level-enum`
  - `text-type-body`
  - `external-declaration`
  - `cppast`
- for `text-type-body` classes and structs, confirm the build also prints a member-discovery summary
- use that summary to sanity-check whether a type is currently contributing:
  - inline reflected fields
  - explicit property or method directives
  - auto properties inferred from getter/setter pairs
  - rejected or overload-rejected candidates that need cleanup

## 4. Follow the repository registration rules

Use these rules when deciding whether to reflect a type:

1. Reflect owned runtime classes, structs, and enums when editor inspection, serialization, or `meta::Type`-driven behavior already needs runtime metadata.
2. Prefer inline `CLASS` / `STRUCT` / `ENUM` declarations for owned runtime types.
3. Prefer `FUNCTION()`-marked `Get`/`Set` pairs when auto-property inference gives the desired property name.
4. Use explicit `PROPERTY(...)` when the exposed property name differs from the accessor naming pattern.
5. Use `MetaExternal` plus `REFLECT_EXTERNAL` when a type should stay free of inline reflection macros.
6. Leave types out of reflection when no current consumer needs runtime metadata for them.

## 5. Extend tests with the supported pattern buckets

When adding or changing reflection coverage, update the automated tests in the matching bucket:

- runtime registration coverage
- generated output coverage
- external or private external binding coverage
- consumer-driven coverage for editor or serialization use

## 6. Keep documentation bilingual

If the maintained reflection workflow changes, update both:

- `Docs/Reflection/ReflectionWorkflow.en.md`
- `Docs/Reflection/ReflectionWorkflow.zh-CN.md`

Then verify the README still points to both files.
