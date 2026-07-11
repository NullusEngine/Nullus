# Feature Specification: Unity-Aligned Sub-Asset Expansion

**Feature Branch**: `059-fix-subasset-expansion`
**Created**: 2026-07-11
**Status**: Ready for Implementation
**Input**: User description: "Align Asset Browser sub-asset expansion with Unity so filtered child counts and membership are exact, children stay attached to the correct source asset, stale refresh state cannot remain actionable, and grid/list group backgrounds render as continuous clipped segments."

## User Scenarios & Testing

### User Story 1 - Trust Expanded Sub-Assets (Priority: P1)

As an editor user, I can expand an asset and see exactly the generated sub-assets that currently belong to it, so the disclosure count and visible children are reliable.

**Why this priority**: Incorrect counts or children attached to the wrong source can cause users to inspect, drag, or select the wrong asset.

**Independent Test**: Expand multiple assets whose generated children are interleaved in the underlying data, then verify each source displays only its own children and its count equals the displayed membership.

**Acceptance Scenarios**:

1. **Given** two source assets with generated children, **When** both are expanded, **Then** every child appears under its own source in stable snapshot order and each source count equals its displayed child count.
2. **Given** a source type that can generate children but has no current child snapshot, **When** it is shown, **Then** it reports zero children and no speculative child row or disclosure is fabricated.
3. **Given** unsupported generated artifact kinds, **When** the source is expanded, **Then** those artifacts affect neither the displayed membership nor the child count.

---

### User Story 2 - Filter and Refresh Without Stale Actions (Priority: P1)

As an editor user, I can search, filter, refresh, and select sub-assets without an older database or UI result remaining actionable after the current state changes.

**Why this priority**: A visually plausible but stale child can route selection, drag, picker, or thumbnail work to an obsolete asset identity.

**Independent Test**: Change search/type filters and publish replacement asset states while background work is pending, then verify Browser actions/thumbnails use only the latest full presentation and the project-wide picker uses only the latest facade state.

**Acceptance Scenarios**:

1. **Given** a query or type filter, **When** only the source matches, only a child matches, both match, or neither matches, **Then** source context, visible children, and count follow the same filtering rule with no mismatch.
2. **Given** an older Browser presentation result completes after the current asset state, folder, expansion, or filter changes, **When** completion is processed, **Then** the old result is discarded and cannot drive Browser selection, drag, actions, or thumbnail work.
3. **Given** an asset is replaced at the same path with a different authoritative asset identity, **When** an old selection or picker cache remains, **Then** it becomes non-actionable and cannot resolve to the replacement.
4. **Given** snapshot validation or background scheduling fails, **When** the failure is processed, **Then** stale entries are cleared, a contextual diagnostic is retained, and retry occurs only after an explicit retry or a meaningful state change.
5. **Given** the project-wide object picker is open, **When** only the Asset Browser folder, expansion, query, or type filter changes, **Then** the picker remains scoped to the current facade state and is not incorrectly narrowed to Browser-visible groups.

---

### User Story 3 - See Connected Unity-Style Groups (Priority: P2)

As an editor user, I see expanded generated sub-assets as one visually connected group in both grid and list modes, matching Unity's Project window grouping behavior.

**Why this priority**: Connected grouping communicates source ownership and makes repeated scanning easier, but correct membership and action identity are more critical.

**Independent Test**: Expand groups across narrow and wide grid layouts and in list mode, then verify each contiguous visible group segment has one continuous background with correct outer rounding and content clipping.

**Acceptance Scenarios**:

1. **Given** adjacent children from one source in a grid row, **When** the group is drawn, **Then** internal column gaps are covered by one continuous background and only outer segment edges are rounded.
2. **Given** a child group wraps to another grid row, **When** the group is drawn, **Then** each row segment is continuous and the wrap begins a new segment without joining unrelated items.
3. **Given** a list clip begins or ends inside a child group, **When** the visible slice is drawn, **Then** the background remains continuous within the active content clip and does not paint outside it.

### Edge Cases

- A valid snapshot omits a source that is currently displayed.
- A snapshot contains duplicate or invalid canonical source keys.
- Source paths or sub-asset keys contain delimiter characters such as `#`.
- Asset and folder refreshes arrive in either order.
- Search text matches only a generated child's fallback name.
- A panel closes while background presentation or picker work is pending.
- A scheduler rejects work or a worker fails or is cancelled.
- A group contains one child, spans residual grid width, or is clipped at its first or last visible row.
- The presentation epoch reaches its reserved boundary; retry attempts use retained identities and therefore have no numeric wrap boundary.

## Requirements

### Functional Requirements

- **FR-001**: The Asset Browser MUST derive a source's child count and displayed child membership from the same current, validated source data and active filter state.
- **FR-002**: The Asset Browser MUST attach every displayed generated child to exactly one canonical source group and preserve the source snapshot order within that group.
- **FR-003**: The Asset Browser MUST report zero proven children when a current valid source snapshot is absent and MUST NOT infer a child from source capability metadata.
- **FR-004**: The Asset Browser MUST use one exhaustive eligibility rule for both counting and displaying supported generated artifact kinds.
- **FR-005**: Search and type filtering MUST apply the filter truth table below consistently to source visibility, child visibility, and child count; query and type predicates are combined with logical AND for each source or child candidate.
- **FR-006**: Browser presentation results MUST become visible and actionable only when their asset state, folder roots, expansion state, and filter state all match the latest current inputs; project-wide picker results depend on facade state only.
- **FR-007**: Selection, drag, picker, action, and thumbnail routing MUST use structured authoritative asset identity and MUST NOT rebind an old identity to a replacement at the same path.
- **FR-008**: Invalid snapshot data, scheduling rejection, worker failure, and cancellation MUST fail closed by clearing stale actionable data and retaining a contextual diagnostic.
- **FR-009**: Background work MUST be bounded to one active build and at most one latest pending state per consumer, without repeated per-frame retry after an unchanged failure.
- **FR-010**: Expanded generated children MUST use one continuous background per contiguous grid-row or visible-list segment, including the spacing between adjacent children.
- **FR-011**: Group background drawing MUST preserve the active content clip and MUST use rounded corners only on true outer segment edges.
- **FR-012**: Source rows/cards, item-local selection, and hover feedback MUST remain visually and behaviorally distinct from the generated-child group background.
- **FR-013**: A stale or Browser-hidden group MUST NOT remain eligible for Browser workflow actions or thumbnail scheduling. The project-wide object picker MUST expose only identities from the current facade state and MUST remain independent of Browser folder, expansion, and filter visibility.
- **FR-014**: Existing importer outputs, artifact contents, generated-asset mutability, runtime asset identity, and thumbnail generation behavior MUST remain unchanged.
- **FR-015**: Automated tests MUST cover membership/count consistency, state replacement and failure paths, authoritative identity replacement, and pixel-level continuity/clipping in both grid and list geometry.

### Filter Truth Table

`sourceMatch` and `childMatch` already include both active query and type predicates. `matchingChildCount` counts eligible children whose own predicates match.

| Source match | At least one child match | Source visible | Disclosure count | Expanded child rows | Collapsed child rows |
|---|---|---|---:|---|---|
| Yes | No | Yes | 0 | None | None |
| No | Yes | Yes as context | `matchingChildCount` | Matching children only | None |
| Yes | Yes | Yes | `matchingChildCount` | Matching children only | None |
| No | No | No | 0 | None | None |

The builder scans eligible snapshot children even when collapsed so the disclosure count and child-only source context remain exact. Expanding changes emitted child rows, not filter membership or count.

### Key Entities

- **Published Asset State**: One current, validated view of artifact manifests, known-current source paths, and editor asset/sub-asset snapshots.
- **Source Group**: A source asset plus the generated children proven to belong to its canonical path and authoritative asset identity.
- **Presentation State**: The complete source roots, published asset state, expansion choices, and filter choices used to build one visible result.
- **Action Identity**: A structured identity distinguishing folder, source asset, and generated sub-asset and carrying authoritative asset identity where applicable.
- **Group Segment**: A maximal contiguous set of visible children from one source within one grid row or visible list slice.
- **Picker Cache**: A lifetime- and current-facade-state-bound loading, success, failure, or closed view used by project-wide object-reference selection.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Across all automated source/child filter scenarios, 100% of expanded-group disclosure counts equal their emitted matching child rows, while 100% of collapsed-group disclosure counts equal the matching eligible membership that would be emitted after expansion.
- **SC-002**: Across interleaved-source and same-path-replacement tests, 0 children, selections, picker entries, actions, or thumbnail requests resolve through another source or obsolete asset identity.
- **SC-003**: Across stale completion, rejection, exception, cancellation, and teardown tests, 0 stale results become visible or actionable and unchanged failures cause 0 repeated per-frame submissions.
- **SC-004**: Pixel verification reports no uncovered internal group gaps and no group-background pixels outside the active content clip for single-row, wrapped-grid, list, and mid-group clip scenarios.
- **SC-005**: One presentation rebuild completes with linear growth relative to source roots, relevant sub-assets, emitted items, and processed text, while the editor main thread performs only bounded publication work.
- **SC-006**: Existing targeted Asset Browser and asset database regression tests pass, the Editor target builds, and manual narrow/wide grid plus list checks show continuous Unity-style groups.

## Assumptions

- Unity alignment refers to ownership, disclosure/count behavior, and connected visual grouping rather than pixel-perfect reproduction of every Unity version or theme.
- Only currently supported editor-visible generated artifact kinds participate; adding a new artifact kind requires an explicit eligibility decision and test update.
- Existing asynchronous job ownership and editor main-thread publication conventions remain available.
- Importers, artifact serialization, runtime loading, drag payload semantics, and thumbnail generation are outside this feature's behavioral scope.
- Cooperative cancellation and background reclamation are deferred unless measured latency crosses the documented follow-up thresholds.
