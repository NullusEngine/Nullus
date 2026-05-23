# Tasks: Remove Old Reflection Syntax

**Input**: Design documents from `specs/remove-old-reflection-syntax/`
**Prerequisites**: plan.md, spec.md

## Phase 1: Generated Reflection AST Path

- [ ] T001 Update `Tools/MetaParser/src/Generation/GeneratorRegistry.cs` so header selection no longer treats external reflection macros as MetaParser input.
- [ ] T002 Update `Tools/MetaParser/src/MetaParserTool.Core.cs` and `Tools/MetaParser/src/MetaParserTool.CppAstParser.cs` to remove external text parsing from the generated reflection path.
- [ ] T003 Update `Tools/MetaParser/src/MetaParserTool.Core.cs` so marked overloaded methods emit disambiguated pointer expressions from AST signatures.
- [ ] T004 Update `Tools/MetaParser/src/MetaParserTool.Generation.cs` so object bridge generation is based on reflected inheritance from `NLS::meta::Object`.

## Phase 2: Manual External Reflection

- [ ] T005 Add runtime external reflection registration helpers in `Runtime/Base/Reflection/ExternalReflectionRegistration.h`.
- [ ] T006 Replace old external reflection call sites in `Runtime/Math/ExternalReflection.h`.
- [ ] T007 Replace old external reflection call sites in `Runtime/Engine/Serialize/SceneSerializationData.h`.
- [ ] T008 Replace old private external sample syntax in `Runtime/Base/Reflection/PrivateReflectionExternalSample.h`.

## Phase 3: Call-Site Cleanup

- [ ] T009 Remove old explicit `PROPERTY(name = ...)` from `Runtime/Engine/Components/MeshRenderer.h` without losing the reflected model path behavior.
- [ ] T010 Confirm `Runtime/Engine/SceneSystem/Scene.h` uses empty `FUNCTION()` and generated overload casting.
- [ ] T011 Search runtime/project/tools headers for old reflection call-site macros and remove remaining usage.

## Phase 4: Tests And Validation

- [ ] T012 Update reflection generation/runtime tests for manual external reflection behavior.
- [ ] T013 Build and run `ReflectionTest`.
- [ ] T014 Build and run `NullusUnitTests` or the most focused available subset.
