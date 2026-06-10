# Implementation Plan: DX12 Sampler Descriptor Reuse

**Branch**: `dx12-sampler-descriptor-reuse` | **Date**: 2026-06-10 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/dx12-sampler-descriptor-reuse/spec.md`

## Summary

Fix DX12 sampler heap exhaustion by adding a DX12-internal sampler descriptor table cache. `NativeDX12BindingSet` will continue to allocate resource descriptors per binding set, but sampler descriptor tables will be keyed by table layout and sampler state so identical material sampler tables share one shader-visible descriptor range.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Direct3D 12 backend, existing Nullus RHI descriptor allocator  
**Storage**: N/A  
**Testing**: `NullusUnitTests` focused GoogleTest filters  
**Target Platform**: Windows DX12 for runtime behavior; non-Windows builds exclude DX12 unit files  
**Project Type**: Desktop editor/game engine runtime  
**Performance Goals**: Avoid sampler heap exhaustion for large imported scenes; keep binding set creation O(number of sampler descriptors) plus cache lookup  
**Constraints**: Preserve the 2048 DX12 shader-visible sampler heap capacity; avoid non-DX12 API changes; preserve resource descriptor allocation behavior  
**Scale/Scope**: Hundreds to thousands of material binding sets sharing a small set of sampler states

## Constitution Check

- **Spec-first major change**: Pass. This RHI/DX12 backend behavior change is tracked under this spec bundle.
- **Validation matches subsystem**: Pass. Validation will use DX12-focused unit tests and existing renderer binding lifecycle tests. Runtime Sponza verification is desirable if available after the focused tests pass.
- **Generated code/backend boundaries**: Pass. No generated files are edited; the fix is contained to DX12 backend internals plus tests.
- **Incremental delivery**: Pass. Tests first, then a small cache implementation, then focused validation.
- **Product runtime preservation**: Pass. The change is backend-internal and should preserve editor/game runtime behavior while reducing descriptor pressure.

## Project Structure

### Documentation

```text
specs/dx12-sampler-descriptor-reuse/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Runtime/Rendering/RHI/Backends/DX12/
├── DX12Descriptor.h
├── DX12Descriptor.cpp
└── DX12ExplicitDeviceFactory.cpp

Tests/Unit/
└── DX12SamplerDescriptorReuseTests.cpp
```

**Structure Decision**: Keep the cache inside the DX12 descriptor implementation because it owns shader-visible descriptor heap allocation and table GPU handles. Expose only the minimal constructor dependency needed by `NativeDX12ExplicitDevice`.

## Phase 0: Research

### Decision: Cache sampler descriptor tables rather than increasing heap capacity

**Rationale**: The observed failure reaches `persistentCapacity=2048`. DX12 shader-visible sampler heap capacity is intentionally bounded and material sampler states often repeat. Deduplicating identical sampler tables reduces live descriptor pressure without changing shader bindings or material data.

**Alternatives considered**:

- Increase sampler heap capacity: rejected because it fights the DX12 sampler heap limit and does not address duplicated descriptors.
- Convert material samplers to static samplers: rejected because imported material sampler metadata can vary and static samplers are pipeline-layout scoped.
- Delay prefab resource resolution: rejected as a partial mitigation that leaves large live scenes vulnerable.

### Decision: Cache by table layout plus ordered sampler descriptor values

**Rationale**: The table GPU handle must match the root signature table layout. Sampler descriptor values alone are insufficient because binding/register space/range shape also affect compatibility.

**Alternatives considered**:

- Cache only individual sampler descriptors: rejected because command binding consumes table GPU handles, and assembling per-material tables would still need table storage.
- Cache the entire binding set: rejected because resource descriptors remain per-material and per-texture.

## Phase 1: Design

### Data Model

- `DX12SamplerDescriptorTableKey`: ordered range metadata plus ordered `SamplerDesc` values.
- `DX12SamplerDescriptorTableCacheEntry`: descriptor offset, descriptor count, and key.
- `DX12SamplerDescriptorTableLease`: shared ownership token returned to binding sets; final destruction releases the descriptor range.

### Contracts

- `NativeDX12BindingSet` must receive a shared sampler table cache from the explicit device.
- The cache must return GPU handles and descriptor heap compatibility through the existing binding set access methods.
- Cache internals must be mutex-protected for concurrent binding set creation/destruction.

### Quickstart Validation

1. Build or use an existing Windows test build of `NullusUnitTests`.
2. Run `NullusUnitTests --gtest_filter=DX12SamplerDescriptorReuseTests.*`.
3. Run `NullusUnitTests --gtest_filter=DescriptorAllocatorTests.*:DX12SamplerUtilsTests.*`.
4. If a DX12 runtime editor verification is available, import or commit `Model/main_sponza/NewSponza_Main_Zup_003.fbx` and confirm no `DX12SamplerHeapAllocator: Out of descriptors` errors occur.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| None | N/A | N/A |
