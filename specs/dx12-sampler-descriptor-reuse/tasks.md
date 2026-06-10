# Tasks: DX12 Sampler Descriptor Reuse

**Input**: Design documents from `specs/dx12-sampler-descriptor-reuse/`
**Prerequisites**: plan.md, spec.md
**Tests**: Required because this is a behavior-changing bug fix.

## Phase 1: Setup

- [X] T001 Confirm current DX12 descriptor code and existing worktree changes in `Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.*`.
- [X] T002 Add focused DX12 sampler descriptor reuse test file at `Tests/Unit/DX12SamplerDescriptorReuseTests.cpp`.

## Phase 2: Foundational

- [X] T003 [P] Add test coverage for identical sampler table reuse under constrained capacity.
- [X] T004 [P] Add test coverage for distinct sampler states requiring distinct descriptor ranges.
- [X] T005 [P] Add test coverage for final-owner release returning sampler descriptors.

## Phase 3: User Story 1 - Large Imported Prefabs Remain Renderable (Priority: P1)

**Goal**: Identical sampler tables share descriptor heap ranges.

**Independent Test**: `DX12SamplerDescriptorReuseTests.IdenticalSamplerTablesCreateBeyondSingleHeapCapacity`

- [X] T006 [US1] Implement a DX12 sampler descriptor table cache in `Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.h`.
- [X] T007 [US1] Implement cache key construction, descriptor writing, lookup, and final-owner release in `Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp`.
- [X] T008 [US1] Wire the cache through `NativeDX12ExplicitDevice::CreateBindingSet` in `Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp`.

## Phase 4: User Story 2 - Distinct Sampler State Stays Correct (Priority: P2)

**Goal**: Differing sampler descriptors do not alias the same table.

**Independent Test**: `DX12SamplerDescriptorReuseTests.DifferentSamplerTablesUseDifferentDescriptorRanges`

- [X] T009 [US2] Ensure sampler table keys include table ranges and all sampler descriptor fields in `Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp`.
- [X] T010 [US2] Ensure cache compatibility handles and GPU handles remain table-specific in `Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp`.

## Final Phase: Validation & Review

- [X] T011 Run `NullusUnitTests --gtest_filter=DX12SamplerDescriptorReuseTests.*`.
- [X] T012 Run adjacent descriptor tests: `DescriptorAllocatorTests.*:DX12SamplerUtilsTests.*`.
- [X] T013 Run required `/plan-review` quality gate and address P0/P1 findings.
- [X] T014 Summarize validation evidence and remaining DX12 runtime verification limits.

## Dependencies & Execution Order

- Tests T003-T005 must fail before implementation tasks T006-T010.
- T008 depends on T006-T007.
- Validation T011-T014 depends on implementation completion.

## Parallel Opportunities

- T003, T004, and T005 can be authored independently in the same test file but should be run together before implementation.

## Implementation Strategy

1. Create failing focused tests.
2. Implement the smallest DX12 cache that satisfies reuse, distinctness, and release behavior.
3. Run focused and adjacent tests.
4. Run the repository-required quality review loop before completion.
