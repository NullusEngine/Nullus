# Research: Reflection Audit And Coverage

## Decision 1: Keep the existing MetaParser architecture and improve it in place

- **Decision**: Treat the current MetaParser-driven reflection flow as the maintained architecture and improve declaration coverage, tests, and documentation around it instead of introducing a new reflection framework.
- **Rationale**: The repository already routes runtime reflection through generated module entry points and real consumers depend on that path today. Replacing the architecture would explode scope and violate the feature goal, which is to audit and strengthen the current system.
- **Alternatives considered**:
  - Replace MetaParser with a different generator: rejected because it would turn an audit-and-coverage task into an architecture migration.
  - Hand-register missing reflection metadata in runtime code: rejected because it bypasses the maintained generation flow and would conflict with generated-code boundaries.

## Decision 2: Use consumer-driven registration rules

- **Decision**: Decide what should be reflected based on active consumers: editor inspection, serialization, and runtime `meta::Type`-driven creation or lookup.
- **Rationale**: The project does not benefit from reflecting every type. Consumer-driven rules keep the reflection surface intentional and explainable while still covering the places where runtime metadata matters today.
- **Alternatives considered**:
  - Reflect every public engine type: rejected because it adds noise, increases maintenance cost, and exposes many types that have no reflection consumer.
  - Reflect only `meta::Object` subclasses: rejected because the current system already supports external reflection and reflected value types that are not object subclasses.

## Decision 3: Prefer existing registration patterns over new syntax

- **Decision**: Reuse the current patterns in this order: inline reflected declarations, auto-property inference from `FUNCTION()`-marked `Get`/`Set` pairs, explicit `PROPERTY(...)` for renamed or special properties, and external reflection declarations for types that should not carry inline macros.
- **Rationale**: The generator already supports these patterns and the repository already contains examples of each. Reusing them makes the audit and tests clearer and avoids expanding the supported surface unnecessarily.
- **Alternatives considered**:
  - Add a brand-new macro family for editor-only properties: rejected because it would complicate both the generator and the documentation without a demonstrated need.
  - Require explicit property directives for every property: rejected because auto-property inference is already a maintained and useful pattern for matching `Get`/`Set` pairs.

## Decision 4: Close the highest-value current gap around MaterialRenderer and Inspector usage

- **Decision**: Treat `MaterialRenderer` as the first concrete coverage fix because it already exposes reflection-friendly getter/setter pairs, yet `Inspector` still forces a handwritten fallback path for it.
- **Rationale**: This is a clear example where the project is not fully using the reflection system it already has. Fixing it produces both a concrete code improvement and a better explanation of when a type should register reflection data.
- **Alternatives considered**:
  - Leave `MaterialRenderer` on the handwritten inspector path and only document the gap: rejected because the user explicitly asked to check whether the project is using the scheme correctly and to improve tests around real coverage.
  - Broaden the feature to all editor components and resource-pointer types immediately: rejected because some components still expose resource-pointer surfaces that generic reflection UI does not handle today.

## Decision 5: Shift test coverage toward maintained patterns and consumer outcomes

- **Decision**: Keep generated-fragment assertions for template invariants, but expand runtime tests and shared helpers so the suite clearly covers reflected classes, enums, external reflection declarations, private external bindings, explicit properties, and auto properties.
- **Rationale**: Current tests provide useful coverage, but they do not tell the full story of how the repository uses reflection. Pattern-oriented tests make future changes safer and easier to extend.
- **Alternatives considered**:
  - Replace all generation tests with runtime-only tests: rejected because some template regressions are only visible in generated output.
  - Leave the test structure unchanged and only add more assertion lists: rejected because that would grow duplication and make the suite harder to maintain.

## Decision 6: Consolidate reflection documentation into a bilingual maintained guide

- **Decision**: Publish the maintained reflection workflow under `Docs/Reflection/` as paired English and Chinese guides, then update the README to point there directly.
- **Rationale**: Existing reflection guidance is fragmented and the README reflection section is not currently a reliable entry point. A bilingual maintained guide matches the user request and reduces onboarding friction.
- **Alternatives considered**:
  - Keep updating multiple old documents independently: rejected because scope and terminology drift would continue.
  - Put the entire workflow only in the README: rejected because the README would become too long and harder to maintain in two languages.
