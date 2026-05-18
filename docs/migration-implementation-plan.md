# augene2 Migration And Implementation Plan

## Purpose

This document describes the migration from the current single-purpose MML compiler repository layout to a split architecture with:

- `mugene2`: the existing MML compiler library
- `mugene2-cli`: the existing MML compiler CLI
- `augene2`: the new higher-level project compiler library
- `augene2-cli`: the new higher-level project compiler CLI

It also captures the expected interaction with UAPMD project persistence and graph asset resolution.

This is an implementation plan, not a final format specification.

## Current State

The repository currently contains:

- one static library target: `augene2`
- one CLI target: `augene2-cli`
- one public API focused on MML compilation to SMF or SMF2 clip-like data

The current codebase is already suitable as the foundation for `mugene2`, but it does not yet provide:

- a project data model
- a project compiler layer
- abstract project persistence
- a UAPMD-specific saver/loader
- source-to-graph-asset resolution beyond the low-level `INSTRUMENTNAME` meta event convention

## Scope Boundaries

### In Scope For This Repository

- rename and split modules and build targets
- preserve the existing MML compiler as `mugene2`
- introduce the `augene2` project data model
- introduce the `augene2` project compiler API
- introduce abstract saver/loader interfaces
- implement a UAPMD saver/loader backend
- resolve `INSTRUMENTNAME` to project graph asset names
- emit warnings, not errors, when no graph is resolved for an `INSTRUMENTNAME`

### Out Of Scope For This Repository This Time

- authoring or editing graph node trees in this repository
- generating standalone graph files here
- changing mugene MML syntax for augene-specific needs

Graph authoring remains on the UAPMD side. This repository only needs to resolve and persist graph asset references.

## Architectural Direction

### 1. `mugene2`

`mugene2` remains a dedicated MML compiler library.

Responsibilities:

- parsing mugene MML
- semantic analysis
- include resolution
- diagnostic generation
- emitting:
  - SMF output
  - per-track SMF2 clip data

Non-responsibilities:

- project graph resolution
- project persistence
- UAPMD-specific project layout

### 2. `augene2`

`augene2` becomes a project compiler library that uses `mugene2` as an input compiler component.

Responsibilities:

- define the project data model
- compile source materials into that model
- resolve track-to-graph-asset mapping from `INSTRUMENTNAME`
- hold project-level warnings and compilation decisions
- save/load the project through pluggable persistence backends

### 3. Persistence Backends

`augene2` exposes abstract saver/loader APIs over the `augene2` project model.

The first backend is UAPMD-specific.

This abstraction does not need to be perfectly isolated from UAPMD concerns. Avoiding leakage is desirable, but not mandatory, because the only likely future additional format is the previous Tracktion edit export.

## Source To Graph Mapping Policy

### Canonical Source-Side Selector

`INSTRUMENTNAME` remains the canonical source-side selector for mapping a source MML track to a destination graph asset.

Constraints:

- mugene MML remains general-purpose
- no augene-specific MML directives are added
- graph binding must therefore continue to use existing musical/meta information

### Resolution Semantics

`augene2` resolves the effective `INSTRUMENTNAME` for each relevant compiled track and uses a mapping table or project-supplied registry to produce a graph asset name.

Example outcomes:

- `strings`
- `sf2_realistic`
- `drums_orchestral`

In the `augene2` project model, the resolved value is the graph asset name only.

When saving to UAPMD, the backend converts that name to an external graph file path:

- `strings` -> `graphs/strings.graph.json`
- `sf2_realistic` -> `graphs/sf2_realistic.graph.json`

### Missing Resolution Policy

If an `INSTRUMENTNAME` does not resolve to a graph asset name:

- emit a warning
- do not fail compilation on that basis alone

This matches previous augene behavior.

## UAPMD Coordination

The following UAPMD-side direction is assumed by this plan:

- UAPMD does not support in-project embedded graph definitions for this use case
- `augene2` therefore only needs to target external graph files
- the UAPMD saver writes graph references as relative paths of the form `graphs/<name>.graph.json`

This repository should treat the graph asset name as the canonical value in its own model and let the UAPMD backend derive the external file path.

## Proposed augene2 Project Model

The old `AugeneProject.kt` from augene-ng is a hint, not a strict porting target.

The new C++ model should be shaped around current needs.

### Core Types

The initial model should contain at least:

- `Project`
- `ProjectTrack`
- `ProjectClip`
- `ProjectWarning` or reuse shared diagnostics
- `GraphAssetName`
- `ProjectMetadata`

### Minimum Track Data

Each project track should carry:

- stable track identity within the project
- source origin metadata where useful
- resolved graph asset name, if any
- clip list
- optional source `INSTRUMENTNAME`
- warning state if graph asset resolution failed

### Minimum Clip Data

Each project clip should carry:

- timeline position
- SMF2 clip event data or a clip container representation sufficient for the saver
- timing resolution metadata
- optional clip name / origin information

### Model Philosophy

The `augene2` project model is the central in-memory IR.

Backends should:

- load into this model
- save from this model

They should not become the primary internal model themselves.

## Abstract Saver/Loader API

Introduce an abstract C++ interface for project persistence.

Suggested shape:

- `ProjectStorage` or `ProjectSerializer`
- virtual save API
- virtual load API
- backend-specific implementation such as `UapmdProjectStorage`

The exact signature can be decided later, but it should support:

- input/output path or stream
- optional storage options
- textual error output

## Module Migration Plan

### Phase 1: Rename Existing Compiler To mugene2

Goals:

- preserve behavior
- minimize code movement risk

Tasks:

- rename CMake project targets:
  - `augene2` -> `mugene2`
  - `augene2-cli` -> `mugene2-cli`
- rename exported alias target accordingly
- move public headers from `include/augene2/...` to `include/mugene2/...`
- update namespaces if desired

Notes:

- namespace migration can be separated from target renaming if needed
- a temporary compatibility alias may reduce churn

### Phase 2: Add augene2 Library Skeleton

Goals:

- create a new higher-level module without destabilizing `mugene2`

Tasks:

- add new `augene2` static library target
- add new `include/augene2/...`
- add new `src/augene2/...`
- define empty or minimal project model types
- define high-level diagnostics/warning container

### Phase 3: Add augene2 Project Compiler API

Goals:

- establish the main entry points for higher-level compilation

Tasks:

- define compile options for project compilation
- define source inputs and mapping inputs
- define result type containing:
  - project model
  - diagnostics
  - warnings
- wire `augene2` to call into `mugene2`

### Phase 4: Add INSTRUMENTNAME Resolution

Goals:

- make `augene2` capable of assigning graph asset names

Tasks:

- determine how `mugene2` exposes enough information to recover effective `INSTRUMENTNAME`
- add a resolver input API:
  - mapping table
  - callback
  - registry object
- implement warning-only unresolved graph behavior

Important design question:

The current `mugene2` result is likely too low-level to make robust `INSTRUMENTNAME` extraction pleasant. We may need one of:

- richer compilation result metadata from `mugene2`
- a semantic-side analysis API exposed by `mugene2`
- a dedicated extraction pass shared by both modules

This should be solved before building a large amount of `augene2` logic around brittle event scanning.

### Phase 5: Add Abstract Project Storage API

Goals:

- support persistence without hard-coding the backend into core compiler logic

Tasks:

- define abstract saver/loader base class
- define storage options object if needed
- define backend registration or direct construction strategy

### Phase 6: Add UAPMD Storage Backend

Goals:

- make `augene2` able to save/load the project model in UAPMD form

Tasks:

- implement `UapmdProjectStorage`
- map `augene2` project tracks to UAPMD tracks
- map `augene2` clip data to UAPMD clip data
- persist resolved graph asset names as `graphs/<name>.graph.json`
- handle missing graph asset resolution as a valid but warning-bearing state where possible

Notes:

- this backend only needs to reference graphs
- it does not need to generate graph files in this repository

### Phase 7: Add augene2-cli

Goals:

- expose the new project compiler from the command line

Tasks:

- add new CLI target
- define input conventions for:
  - MML source files
  - graph mapping file or options
  - output project path
  - target format selection
- support UAPMD save path first

### Phase 8: Revisit Tracktion Export

Goals:

- keep future compatibility open without blocking the current UAPMD work

Tasks:

- evaluate whether Tracktion export should be:
  - another backend on the same model
  - a partial exporter with reduced fidelity

This is a later task and should not shape the first UAPMD implementation too heavily.

## Repository Layout Proposal

Suggested target layout after migration:

- `include/mugene2/...`
- `src/mugene2/...`
- `src/mugene2-cli/...`
- `include/augene2/...`
- `src/augene2/...`
- `src/augene2-cli/...`
- `docs/...`

Exact file moves can be staged gradually to reduce churn.

## Diagnostics Policy

### Errors

Use errors for:

- invalid source inputs
- parse or semantic failures
- invalid project persistence input
- impossible saver/loader backend operations
- malformed graph asset mapping data

### Warnings

Use warnings for:

- missing graph asset resolution for `INSTRUMENTNAME`
- no effective `INSTRUMENTNAME` on a track expected to target a graph
- compatibility fallbacks taken during reference resolution

## Testing Plan

### mugene2 Regression Coverage

Keep or add tests for:

- existing MML compilation behavior
- SMF generation
- SMF2 clip generation
- diagnostics

### augene2 Coverage

Add tests for:

- project-model construction from MML inputs
- `INSTRUMENTNAME` extraction
- graph asset resolution
- warning-only unresolved graph behavior
- UAPMD save/load round-trips where feasible

### Integration Coverage

Add end-to-end tests for:

- MML -> `augene2` project -> UAPMD save
- MML with resolved graph asset name
- MML with missing graph asset warning

## Recommended Implementation Order

The preferred order is:

1. rename/split current compiler into `mugene2` + `mugene2-cli`
2. add empty `augene2` target and project model skeleton
3. define high-level compile API for `augene2`
4. solve effective `INSTRUMENTNAME` extraction
5. add graph-reference resolution and warning behavior
6. add abstract storage API
7. implement UAPMD storage backend
8. add `augene2-cli`

This order keeps the currently working compiler stable while allowing the higher-level project work to proceed incrementally.

## Open Questions

The following points still need explicit design decisions:

- whether namespace migration to `mugene2` happens immediately or in a second pass
- how exactly `mugene2` exposes enough metadata for `augene2` to derive effective `INSTRUMENTNAME`
- whether unresolved graph asset tracks should still be emitted as clip-only project tracks in all backends
- what concrete mapping file or resolver API shape `augene2-cli` will accept
- how much of UAPMD data shape should be mirrored directly in the `augene2` model

## Immediate Next Steps

The next concrete work items should be:

1. rename the existing CMake targets to `mugene2` and `mugene2-cli`
2. create the `augene2` library target and basic public headers
3. define the initial `Project`, `ProjectTrack`, and `ProjectClip` types
4. decide the API by which `augene2` obtains resolved graph asset names from `INSTRUMENTNAME`
