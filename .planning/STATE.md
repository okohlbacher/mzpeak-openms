---
gsd_state_version: 1.0
milestone: v1.0
milestone_name: milestone
status: executing
stopped_at: Phase 1 Plan 01-02 complete (RDR-10c precursor/scan join + scan windows)
last_updated: "2026-06-14T18:02:00Z"
last_activity: 2026-06-14 -- Phase 1 Plan 01-02 executed (3 tasks, 30/30 green, 18 new test cases)
progress:
  total_phases: 7
  completed_phases: 0
  total_plans: 3
  completed_plans: 2
  percent: 67
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-06-14)

**Core value:** OpenMS can load/store `.mzpeak` into/from `MSExperiment`, and
mzML → mzpeak → mzML through OpenMS yields an equivalent `MSExperiment`.
**Current focus:** Phase 1 — Reader Lossless-Map Prerequisites

## Current Position

Phase: 1 (Reader Lossless-Map Prerequisites) — EXECUTING
Plan: 3 of 3
Status: Plan 01-02 complete; next: 01-03 (RDR-9b auxiliary_arrays accessor)
Last activity: 2026-06-14 -- Plan 01-02 executed (precursor/isolation-window/
activation/selected-ion/scan join + scan_windows; 18 new test cases; 30/30 green)

Progress: [███████░░░] 67%

## Performance Metrics

**Velocity:**

- Total plans completed: 2
- Average duration: ~53 min
- Total execution time: 1.7 hours

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 1 (in progress) | 2 | ~105 min | ~53 min |

**Recent Trend:**

- Last 5 plans: 50min (01-01), 55min (01-02)
- Trend: stable ~53 min/plan

*Updated after each plan completion*

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table (8 de-facto design
decisions treated as LOCKED for this milestone). Most relevant to current work:

- DEC-arch-hybrid-handler: RDR-19 is a native OpenMS hybrid handler (reuse
  `null_fill`+numpress cores, re-implement chunk-assembly natively).

- DEC-data-layer-not-xml: read via OpenMS `ParquetFile`/`ZipRandomAccessFile`,
  not the XML SAX path (deps already present).

- DEC-metadata-mirror-mzml: mirror `MzMLHandler::handleCVParam_` (M2) for v1.
- DEC-whole-experiment-first: `load` before streaming `transform`.

- DEC-01-01-store-spectrum-local: `spectra[i]` returns `Spectrum` by value
  (temporary); always bind to a named local before calling `.metadata()` to
  avoid a dangling reference. Applied throughout spectrum_metadata_test.cpp.

- DEC-01-01-large-list-length: use `la->value_length(row)` not
  `la->value_offset(row+1) - la->value_offset(row)` in LargeListArray
  iteration; safer and avoids off-by-one concern at chunk boundary.

- DEC-01-01-format-canonical: `format_double_canonical` uses ostringstream +
  setprecision(17) + strip trailing zeros; `std::to_string` is forbidden for
  float-arm CvParam values (M2 rule).

### Decisions (plan 01-02)

- DEC-01-02-float-scan-window: ScanWindow.lower/upper_limit are optional<float>
  (not double); Arrow schema stores float32 for MS_1000501/MS_1000500.

- DEC-01-02-table-read-once: read_spectra_metadata calls ReadTable once; all
  four facet passes (spectrum, precursor, selected_ion, scan) share one table.

- DEC-01-02-test-timeout-120s: spectrum_metadata moved to separate meson test()
  with timeout=120s; 18-case suite runs ~34s (too close to 30s default).

- DEC-01-02-clang-format-meson-excluded: meson.build is not C++; clang-format
  incorrectly reformatted it (URL -> goto label); excluded from style gate.

### Pending Todos

- Plan 01-03: RDR-9b (typed auxiliary_arrays accessor; structural validation
  via schema parse + empty-list round-trip; full phase gate).

### Blockers/Concerns

- [Phase 0] `libOpenMS` is NOT yet built locally (only deps). Finishing the build
  is a long compile and gates all RDR-19 work.

## Standing Per-Phase Gates

Apply on EVERY phase before commit (see PROJECT.md Constraints):

1. Adversarial cross-AI review (Codex + Vibe) of the phase plan before execution.
2. Style conformance: `clang-format -i` + diff-verify (not `--dry-run`) +
   editorconfig on every edited file. OpenMS-tree files also follow OpenMS
   conventions (CamelCase, `OPENMS_DLLAPI`, `$Maintainer$`/`$Authors$`, Doxygen).

3. Semantic validation only (1e-9 m/z, 1e-6 intensity); never byte-diff Parquet.

## Deferred Items

Carried forward to a future milestone (see PROJECT.md Out of Scope):

| Category | Item | Status | Deferred At |
|----------|------|--------|-------------|
| Writer | WRT-1 (chunked/numpress/null-mark emit) | Deferred | bootstrap |
| Reader | RDR-20 (AES decryption) | Blocked (no fixtures) | bootstrap |
| Perf | RDR-21 (LRU cache) / RDR-23 (page index) | Deferred | bootstrap |
| Reader | RDR-22b (buffer-backed zip) | Deferred | bootstrap |
| Reader | RDR-28b (chunked wavelength) | Blocked (no fixture) | bootstrap |
| Reader | RDR-14 (remote/cloud) | Deferred (future) | bootstrap |
| Reader | RDR-4b (large_list/string types) | Deferred (no benefit) | bootstrap |
| Integration | M1 shared `applyCVParam` (Phase 3b) | Optional | bootstrap |
| Reader | RDR-10c ion-mobility | Deferred (null in all fixtures) | 01-01 |

## Session Continuity

Last session: 2026-06-14
Stopped at: Phase 1 Plan 01-02 complete (RDR-10c precursor/scan join;
18 test cases including 34 MS2 / 14 MS1 sweep; 30/30 green).
Resume file: None
