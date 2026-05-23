# Quickstart: MetaParser Resource Reflection

Run the source-contract test before implementation and confirm it fails because resource headers still handwrite type names:

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=MetaParserGenerationModuleTests.RenderingResourceReflectionOwnedClassesUseGeneratedBodies
```

After migration, rebuild through the normal MetaParser path:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false
```

Run focused tests:

```powershell
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=MetaParserGenerationModuleTests.*:ReflectionRuntimeTestFixture.*:PPtrTests.*
```

Run reflection smoke validation:

```powershell
cmake --build Build --target ReflectionTest --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false
.\Build\bin\Debug\ReflectionTest.exe
```
