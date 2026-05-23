# Research: Render Framework Optimization

## Decision: Use One Top-Level TODO Tracker With Incremental Test-First Slices

**Rationale**: The user provided a cross-cutting list spanning P0 correctness through P2 tooling. A single persisted tracker keeps priority, status, and verification evidence visible while allowing individual tasks to remain small and testable.

**Alternatives considered**:
- Separate spec per subsystem immediately: better isolation, but loses the user's requested unified checklist.
- Ad-hoc TODO in chat only: easy to lose and impossible to audit.

## Decision: Mark Existing Session Fixes As Completed When Directly Matching Tasks

**Rationale**: Mesh bounds, MultiFramebuffer delayed retirement, texture recreation failure preservation, Texture2D metadata alignment, and cubemap failure propagation were already implemented with targeted tests in this session.

**Alternatives considered**:
- Leave all tasks unchecked until a separate pass: would under-report already validated work.

## Decision: Record External Build Blockers Instead Of Forcing Process Termination For User Apps

**Rationale**: Debug validation may be blocked by a running Editor process holding `NLS_Renderd.dll`. The user owns that process; we should not close it without explicit permission.

**Alternatives considered**:
- Kill Editor automatically: unsafe for user work.
- Ignore the blocked validation: hides important evidence gaps.
