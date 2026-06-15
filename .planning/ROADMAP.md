# Roadmap: mzPeak C++ (OpenMS reader/writer)

## Overview

The standalone mzPeak reader and writer are done and committed (suite 29/29,
cross-impl T1–T5 PASS). This milestone delivers the remaining headline work —
RDR-19: wiring the reader/writer into OpenMS so OpenMS can `load`/`store`
`.mzpeak` into/from `MSExperiment`. The journey: stand up a local `libOpenMS`
build and C++20-compatible cores (Phase 0), close the reader metadata gaps that
bound a lossless map (Phase 1), implement `MzPeakFile::load` + converter +
registration + test (Phase 2), implement `MzPeakFile::store` (Phase 3), optionally
propose the shared-CV-helper refactor upstream (Phase 3b), add streaming
`transform` + `PeakFileOptions` (Phase 4), and finally cross-validate the
mzML→mzpeak→mzML round-trip through OpenMS (Phase 5).

**Standing per-phase gates** (apply to every phase below):
- **Adversarial cross-AI review** (Codex + Vibe) of the phase plan before
  execution — the project's established plan-review convergence gate.
- **Style conformance** — `clang-format -i` + diff-verify (not `--dry-run`) +
  editorconfig on every edited file before committing. For files in the OpenMS
  tree, OpenMS conventions also apply (CamelCase, `OPENMS_DLLAPI`,
  `$Maintainer$`/`$Authors$` headers, Doxygen).
- **Semantic validation only** — decode-and-compare within tolerance (1e-9 m/z,
  1e-6 intensity); never byte-diff Parquet.

## Phases

**Phase Numbering:**
- Integer phases (0, 1, 2, …): planned milestone work.
- Decimal phases (3.1, …): urgent insertions (marked INSERTED).
- Phase 3b is an *optional, propose-to-OpenMS* phase off the critical path.

- [x] **Phase 0: Build & Port Prerequisites** - Build `libOpenMS` locally; port reused cores to C++20 (COMPLETE 2026-06-15)
- [x] **Phase 1: Reader Lossless-Map Prerequisites** - Close RDR-10b/10c/9b reader metadata gaps (COMPLETE 2026-06-14)
- [ ] **Phase 2: MzPeakFile::load + Registration** - Whole-experiment load, converter, FileTypes/FileHandler wiring, test
- [ ] **Phase 3: MzPeakFile::store** - MSExperiment → mzPeak write path with metadata emit
- [ ] **Phase 3b: Shared CV Helper (OPTIONAL, propose to OpenMS)** - Refactor `handleCVParam_` into a shared `applyCVParam`
- [ ] **Phase 4: Streaming transform + PeakFileOptions** - Streaming consumer with RT/mz/ms-level/metadata-only filtering
- [ ] **Phase 5: Cross-Validation** - mzML → mzpeak → mzML through OpenMS yields an equivalent MSExperiment

## Phase Details

### Phase 0: Build & Port Prerequisites
**Goal**: A linkable local `libOpenMS` and C++20-compatible reused cores, so all
subsequent RDR-19 work can compile and be tested.
**Depends on**: Nothing (first phase)
**Requirements**: BUILD-01, PORT-01
**Success Criteria** (what must be TRUE):
  1. `libOpenMS` builds locally and a trivial program linking it compiles and runs.
  2. The reused numeric cores (`null_fill`, numpress wrapper) compile under
     `cxx_std_20` with no C++23-only features, and their existing kernel tests
     stay green.
  3. `MzPeakFile_test` (empty stub) can be added to the OpenMS test harness and
     compile-links against `libOpenMS`.
**Plans**: TBD

### Phase 1: Reader Lossless-Map Prerequisites
**Goal**: The mzPeak reader exposes the per-spectrum and array metadata RDR-19
needs to map a lossless experiment.
**Depends on**: Phase 0
**Requirements**: RDR-10b, RDR-10c, RDR-9b
**Success Criteria** (what must be TRUE):
  1. Per-spectrum `parameters` (flat CvParam list), `spectrum_type`, observed-mz
     range, and `data_processing_ref` are readable and match the Rust reader on a
     fixture (RDR-10b).
  2. Precursor / selected-ion / isolation-window / activation columns read
     correctly from small.mzpeak's MS2 spectra, matching pyarrow ground truth /
     the Rust oracle within tolerance (RDR-10c); ion-mobility decode is a
     documented stub (null in every bundled fixture).
  3. `auxiliary_arrays` are accessible as named, typed arrays (→ OpenMS
     FloatDataArrays), structurally validated (empty-list round-trip + schema
     parse) vs pyarrow; value-level validation deferred to a populated fixture
     (RDR-9b).
**Plans**: 3 plans
Plans:
- [x] 01-01-PLAN.md — RDR-10b: spectrum_type / observed-mz / data_processing_ref / per-spectrum parameters + new test scaffold
- [x] 01-02-PLAN.md — RDR-10c: precursor / selected-ion / isolation-window / activation + scan params/windows (IM deferred stub)
- [x] 01-03-PLAN.md — RDR-9b: typed auxiliary_arrays accessor (structural) + full phase gate (meson test + e2e)
**Notes**: RDR-10c is substantially unblocked — small.mzpeak carries 34 MS2
spectra with full precursor/isolation/activation/selected-ion data (research
finding); only ion-mobility decode is deferred (null in all fixtures). RDR-9b is
schema-observable but data-empty in all bundled fixtures, so validated
structurally. Pure standalone mzpeak-lib work on branch `writer_test` — no OpenMS
code, so Phase 1 is independently executable ahead of Phase 0.

### Phase 2: MzPeakFile::load + Registration
**Goal**: OpenMS can load a `.mzpeak` archive into a populated `MSExperiment`
(+ `ExperimentalSettings`) via a registered, content-detected file handler.
**Depends on**: Phase 1
**Requirements**: INT-01, INT-02, INT-03, INT-04
**Success Criteria** (what must be TRUE):
  1. `MzPeakFile::load(filename, MSExperiment&)` round-trips a bundled fixture
     into `MSExperiment`, asserting peaks, RT (seconds), ms_level, polarity, type
     (PROFILE/CENTROID), and run metadata; spectra are sorted by m/z and
     `updateRanges()` is called.
  2. `.mzpeak` resolves through `FileHandler` by extension and by content
     detection (zip whose first member is `mzpeak_index.json`), and `getType`
     returns `MZPEAK`.
  3. Recognized PSI-MS accessions map to typed OpenMS setters (mirroring the
     `handleCVParam_` subset, M2); unrecognized accessions fall through to
     `setMetaValue`; run-level blocks populate `ExperimentalSettings`.
  4. `MzPeakFile_test` passes load assertions in the OpenMS test harness.
**Plans**: TBD
**Notes**: Hybrid option C — reuse `null_fill` + numpress cores, re-implement
chunk-assembly natively against `arrow::ChunkedArray` via OpenMS `ParquetFile` /
`ZipRandomAccessFile`. Handle isolation-window offsets (not bounds), map every
selected ion, set IMFormat consistently.
**UI hint**: no

### Phase 3: MzPeakFile::store
**Goal**: OpenMS can store an `MSExperiment` back to `.mzpeak`, re-readable into
an equivalent experiment.
**Depends on**: Phase 2
**Requirements**: INT-05
**Success Criteria** (what must be TRUE):
  1. `MzPeakFile::store(filename, MSExperiment&)` writes a `.mzpeak` using the
     existing point-layout writer, emitting run-level metadata (WRT-2) and CV
     params à la `MzMLHandler::writeCV_`.
  2. A stored `.mzpeak` re-reads through both the C++ and Rust readers into an
     `MSExperiment` equivalent to the source (semantic compare within tolerance).
  3. Store-side `MzPeakFile_test` assertions pass.
**Plans**: TBD

### Phase 3b: Shared CV Helper (OPTIONAL, propose to OpenMS)
**Goal**: mzML and mzPeak map CV params through one shared helper, eliminating the
M2 mirrored subset — proposed upstream to OpenMS, off this milestone's critical path.
**Depends on**: Phase 3
**Requirements**: INT-08 (M1, optional)
**Success Criteria** (what must be TRUE):
  1. `handleCVParam_` is refactored into a shared `applyCVParam` helper that mzML
     continues to use with the full mzML regression suite green.
  2. The mzPeak handler reuses the shared helper instead of its mirrored subset.
**Plans**: TBD
**Notes**: OPTIONAL. Skip without blocking Phase 4/5 if upstream timing or review
makes it impractical this milestone; it is a consolidation PR, not a deliverable.

### Phase 4: Streaming transform + PeakFileOptions
**Goal**: OpenMS can stream a `.mzpeak` through an `IMSDataConsumer` with
RT/mz/ms-level/metadata-only filtering.
**Depends on**: Phase 3 (Phase 3b optional, not required)
**Requirements**: INT-06
**Success Criteria** (what must be TRUE):
  1. `MzPeakFile::transform(filename, IMSDataConsumer*, skip_full_count,
     skip_first_pass)` streams spectra/chromatograms and yields the same
     spectrum/chromatogram count as `load` on the same fixture.
  2. `PeakFileOptions` RT-range, m/z-range, and ms-level filters narrow the
     streamed output as configured.
  3. `DetailLevel::MetadataOnly` (`setMetadataOnly`/`setFillData(false)`) streams
     ids/ms_level/time with empty peak arrays.
**Plans**: TBD

### Phase 5: Cross-Validation
**Goal**: The milestone's defining round-trip holds: mzML → mzpeak → mzML through
OpenMS produces an equivalent `MSExperiment`.
**Depends on**: Phase 4
**Requirements**: INT-07
**Success Criteria** (what must be TRUE):
  1. OpenMS reading a C++/Rust-written `.mzpeak` yields the same `MSExperiment`
     as reading the equivalent mzML (semantic compare, 1e-9 m/z / 1e-6 intensity).
  2. mzML → mzpeak → mzML through OpenMS round-trips to an equivalent
     `MSExperiment`.
  3. The cross-validation is wired into the e2e scripts (extending the T1–T5
     matrix) and runs green.
**Plans**: TBD

## Progress

**Execution Order:**
Phases execute in numeric order: 0 → 1 → 2 → 3 → (3b optional) → 4 → 5

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 0. Build & Port Prerequisites | 3/3 | COMPLETE | 2026-06-15 |
| 1. Reader Lossless-Map Prerequisites | 3/3 | COMPLETE | 2026-06-14 |
| 2. MzPeakFile::load + Registration | 0/TBD | Not started | - |
| 3. MzPeakFile::store | 0/TBD | Not started | - |
| 3b. Shared CV Helper (optional) | 0/TBD | Not started | - |
| 4. Streaming transform + PeakFileOptions | 0/TBD | Not started | - |
| 5. Cross-Validation | 0/TBD | Not started | - |

### Phase 1 Plan Progress

| Plan | Description | Status | Commits |
|------|-------------|--------|---------|
| 01-01 | RDR-10b: spectrum_type / observed-mz / data_processing_ref / parameters | DONE | 4e00a45, a006264, 8abd3b7, 69b2aeb |
| 01-02 | RDR-10c: precursor / isolation-window / activation / scan params+windows | DONE | c258016, 2e64711, 6200457, 10641ab |
| 01-03 | RDR-9b: auxiliary_arrays accessor + phase gate | DONE | 30c546f, 01be7cf, c315282 |

Phase 1 complete: 30/30 tests green; e2e PASS; clang-format all-clean.
RDR-9b structural decode (schema parse + empty-list + count-consistency assert)
validated; raw-byte VALUE decode is fixture-gated (deferred to Phase 2 fixture).

---
*Last updated: 2026-06-14 after Phase 1 completion (Plan 01-03)*
