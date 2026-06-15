# Requirements: mzPeak C++ (OpenMS reader/writer)

Requirements for the OpenMS-integration milestone. Synthesized from the ingest
intel (`roadmap-remaining.md` authoritative phasing spine,
`openms-integration-plan.md` RDR-19 design, `reader-backlog.md`,
`e2e-testing.md`). Status legend: **DONE** (shipped on `writer_test` this round
or earlier) · **OPEN** (active milestone scope) · **DEFERRED** (blocked /
no-benefit / multi-month).

Active-milestone REQs carry stable category IDs (`BUILD-*`, `PORT-*`, `RDR-*`,
`INT-*`) for traceability. DONE/DEFERRED items are listed for context and to
prevent re-planning.

---

## Active (this milestone)

### BUILD — Build environment

#### BUILD-01 — Build `libOpenMS` locally
- traces: REQ-openms-build
- status: OPEN · phase: 0
- A local `libOpenMS` builds and is linkable so RDR-19 code can compile and
  `MzPeakFile_test` can run. The source tree (`~/Claude/OpenMS`) and a partial
  build (`~/openms_build`, deps only) already exist; finishing the compile is the
  gating chore, not an architectural blocker.
- acceptance: `libOpenMS` builds; a trivial program linking it compiles and runs.

### PORT — Language-standard compatibility

#### PORT-01 — Port reused numeric cores to C++20
- traces: REQ-cpp20-port
- status: OPEN · phase: 0
- The reused kernels (`null_fill`, numpress wrapper) compile under `cxx_std_20`
  with no C++23-only features. The full mzPeak lib does not need porting (option
  C). `std::bit_cast` + ranges are C++20, so no blockers are expected.
- acceptance: the ported cores compile under `cxx_std_20`; existing kernel tests
  stay green.

### RDR — Reader prerequisites (lossless-map gaps)

> **DEC-phase1-pyarrow-oracle (orchestrator decision, 2026-06-14):** For Phase 1,
> the new reader metadata fields (RDR-10b/10c/9b) are validated against **pyarrow
> ground truth** on bundled fixtures — the authoritative source-of-truth read
> directly from the actual Parquet. Full **Rust-oracle parity** for these fields is
> **deferred to Phase 5 (INT-07 Cross-Validation)**, the project's defined
> cross-validation phase (mzML→mzpeak→mzML, e2e matrix T2/T4/T5). The Phase 1
> plans' pyarrow assertions fully satisfy the revised acceptance criteria above.

#### RDR-10b — Per-spectrum CvParams / spectrum_type / observed-mz / data_processing_ref
- traces: REQ-reader-prereqs-10b10c9b (10b)
- status: DONE · phase: 1 · plan: 01-01 · date: 2026-06-14
- `read_spectra_metadata` + `SpectrumMetadata` expose per-spectrum `parameters`
  (flat `CvParam` list), `spectrum_type`, observed-mz range, and
  `data_processing_ref`.
- acceptance: per-spectrum CvParams + spectrum_type + observed-mz + dp-ref
  surfaced for a fixture and validated against pyarrow ground truth on bundled
  fixtures; Rust-oracle parity covered by Phase 5 cross-validation (DEC-phase1-pyarrow-oracle).

#### RDR-10c — Precursor / isolation / activation / ion-mobility columns
- traces: REQ-reader-prereqs-10b10c9b (10c)
- status: OPEN · phase: 1
- Precursor / selected-ion / isolation-window / activation / ion-mobility columns
  exposed, mirroring the Rust reader fields. Requires an MSn+IM fixture.
- acceptance (revised for Phase 1 scope): the populated precursor / isolation /
  activation / selected-ion fields in bundled fixtures (small.mzpeak's 34 MS2
  spectra) are validated against pyarrow ground truth now; Rust-oracle parity for
  these fields is covered by Phase 5 cross-validation (DEC-phase1-pyarrow-oracle).
  The IM subcase still requires an MSn+IM fixture and is deferred within Phase 1.

#### RDR-9b — Typed `auxiliary_arrays` accessor
- traces: REQ-reader-prereqs-10b10c9b (9b)
- status: DONE (Plan 01-03, 2026-06-14) · phase: 1
- A typed accessor for `auxiliary_arrays`, mapping to OpenMS `FloatDataArrays`
  (named + CV-annotated).
- acceptance: auxiliary arrays read back as named typed arrays; validated
  structurally (schema + empty-list round-trip) against pyarrow ground truth on
  bundled fixtures (number_of_auxiliary_arrays==0 in all bundled data, so value-
  level validation awaits a populated fixture); Rust-oracle parity covered by
  Phase 5 cross-validation (DEC-phase1-pyarrow-oracle).

### INT — RDR-19 OpenMS integration (decomposed from REQ-rdr19-openms-integration)

#### INT-01 — `MzPeakFile::load` (whole-experiment) + mzPeak→MSExperiment converter
- traces: REQ-rdr19-openms-integration (load)
- status: OPEN · phase: 2
- `MzPeakFile::load(filename, MSExperiment&)` reads a `.mzpeak` archive into
  `MSExperiment` + `ExperimentalSettings`. Hybrid option C: reuse `null_fill` +
  numpress cores, re-implement chunk-assembly natively against
  `arrow::ChunkedArray` via OpenMS `ParquetFile`/`ZipRandomAccessFile`.
  Per-spectrum mapping per CON-data-model-mapping (RT seconds, intensity float,
  m/z double; sort by m/z; `updateRanges()` mandatory).
- acceptance: a bundled fixture round-trips into `MSExperiment` asserting peaks,
  RT, ms_level, polarity, type, and run metadata.

#### INT-02 — FileTypes / FileHandler registration + content detection
- traces: REQ-rdr19-openms-integration (registration)
- status: OPEN · phase: 2
- `MZPEAK` added to `FileTypes.h` enum + name/extension (`.mzpeak`) tables;
  dispatch in `FileHandler::loadExperiment`/`storeExperiment`/`getType`; content
  detection (zip whose first member is `mzpeak_index.json`).
- acceptance: `FileHandler` resolves `.mzpeak` by extension and by content;
  `getType` returns `MZPEAK`.

#### INT-03 — Metadata mapping (M2: mirror `handleCVParam_` subset)
- traces: REQ-rdr19-openms-integration (metadata), CON-mzml-cv-dispatch
- status: OPEN · phase: 2
- Per-spectrum + run-level metadata mapped by mirroring the `MzMLHandler::
  handleCVParam_` accession→setter subset that mzPeak emits (option M2). Run
  metadata → `ExperimentalSettings`. Handle the impedance mismatches in
  CON-impedance-mismatches (isolation-window offsets, every selected ion, IM
  representation, computed base-peak/TIC).
- acceptance: recognized accessions map to typed setters; unrecognized →
  `setMetaValue`; run-level blocks populate `ExperimentalSettings`.

#### INT-04 — `MzPeakFile_test`
- traces: REQ-rdr19-openms-integration (test), CON-mzpeakfile-api
- status: OPEN · phase: 2
- `src/tests/class_tests/openms/source/MzPeakFile_test.cpp` registered in
  `executables.cmake`; fixture data under `data/`. Header carries SPDX BSD-3 +
  `$Maintainer$`/`$Authors$`, `OPENMS_DLLAPI`, `#include <OpenMS/config.h>`.
- acceptance: `MzPeakFile_test` compiles and passes load assertions in the OpenMS
  test harness.

#### INT-05 — `MzPeakFile::store` (MSExperiment→mzPeak) + WRT-2 metadata emit
- traces: REQ-rdr19-openms-integration (store)
- status: OPEN · phase: 3
- `MzPeakFile::store(filename, MSExperiment&)` writes an `MSExperiment` back to
  `.mzpeak` reusing the existing point-layout writer, emitting run-level metadata
  (WRT-2) and CV params à la `MzMLHandler::writeCV_`.
- acceptance: a stored `.mzpeak` re-reads (C++ + Rust) into an equivalent
  `MSExperiment`; store-side `MzPeakFile_test` assertions pass.

#### INT-06 — Streaming `transform` consumer + `PeakFileOptions`
- traces: REQ-rdr19-openms-integration (streaming), DEC-whole-experiment-first
- status: OPEN · phase: 4
- `MzPeakFile::transform(filename, IMSDataConsumer*, skip_full_count,
  skip_first_pass)` streams spectra/chromatograms; `PeakFileOptions` supports
  RT-range / m/z-range / ms-level filtering and metadata-only
  (`DetailLevel::MetadataOnly` → `setMetadataOnly`/`setFillData(false)`).
- acceptance: streaming a fixture through a counting consumer yields the same
  spectrum/chromatogram count as `load`; option filters narrow the output.

#### INT-07 — Cross-validation: mzML → mzpeak → mzML equivalence
- traces: REQ-rdr19-openms-integration (cross-validate), CON-e2e-matrix,
  CON-semantic-compare
- status: OPEN · phase: 5
- OpenMS reading a C++/Rust-written mzPeak yields the same `MSExperiment` as
  reading the equivalent mzML; mzML→mzpeak→mzML through OpenMS round-trips to an
  equivalent `MSExperiment`. Semantic compare only (1e-9 m/z, 1e-6 intensity).
- acceptance: the round-trip test passes within tolerance and is wired into the
  e2e scripts.

### INT (optional, propose-to-OpenMS)

#### INT-08 — M1: shared `applyCVParam` helper in OpenMS (optional)
- traces: REQ-rdr19-openms-integration (M1 follow-on), DEC-metadata-mirror-mzml
- status: OPEN (optional) · phase: 3b
- Refactor OpenMS `handleCVParam_` into a shared `applyCVParam` helper so mzML
  and mzPeak map identically. A consolidation PR proposed to OpenMS; off the
  milestone critical path.
- acceptance: mzML regression suite stays green; mzPeak reuses the shared helper.

---

## Done (context — do not re-plan)

| ID | Description | Status | Commit |
|----|-------------|--------|--------|
| RDR-10 | Per-spectrum scalar metadata via `Spectrum::metadata()` | DONE | 033e874 |
| RDR-24 | File-level metadata blocks + format version | DONE | 950a76c / 5c7b1c2 |
| RDR-4a | `UInt64` unsigned entity-index type system | DONE | fbe4b58 |
| RDR-28a | Chunked-layout chromatogram read | DONE | 13248fa |
| RDR-25 | Detail-level / metadata-only read mode | DONE | 2ffd823 |
| RDR-22 | In-memory / buffer archive source (`open_buffer`) | DONE | 8f030a5 |
| WRT-2 | Writer emits run-level `metadata{}` blocks | DONE | 9780a4a |
| RDR-1..29 (rest) | Point/chunked/numpress decode, queries, containers, e2e harness (T1–T5) | DONE | (writer_test) |

## Deferred (future milestone / backlog — not active)

| ID | Description | Reason |
|----|-------------|--------|
| WRT-1 | Writer chunked/delta/numpress + null-marking emit | Separate multi-month project |
| RDR-20 | Parquet modular (AES) decryption | Blocked: no encrypted fixtures/keys |
| RDR-21 | LRU row-group cache | Perf only; touches RDR-4a paths; weak validation |
| RDR-23 | Page/offset-index selection | Perf only; no behavioral fixture |
| RDR-22b | Buffer-backed *zip* source | Larger (zip reopens per member); useful but deferred |
| RDR-28b | Chunked wavelength | No fixture; decoder hard-casts DoubleArray vs Float32 |
| RDR-14 | Remote/cloud (HTTP-range / S3 / object_store) | P4 future, network infra |
| RDR-4b | large_list/large_string/large_list<u8> modeling | No functional benefit; chunked decoder casts directly |

---

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| BUILD-01 | Phase 0 | Pending |
| PORT-01 | Phase 0 | Pending |
| RDR-10b | Phase 1 | DONE (Plan 01-01, 2026-06-14) |
| RDR-10c | Phase 1 | DONE (Plan 01-02, 2026-06-14) |
| RDR-9b | Phase 1 | DONE (Plan 01-03, 2026-06-14) |
| INT-01 | Phase 2 | Done |
| INT-02 | Phase 2 | Done |
| INT-03 | Phase 2 | Done |
| INT-04 | Phase 2 | Done |
| INT-05 | Phase 3 | Pending |
| INT-08 (M1, optional) | Phase 3b | Pending |
| INT-06 | Phase 4 | Pending |
| INT-07 | Phase 5 | Pending |

**Active-milestone coverage:** 13/13 active requirements mapped (12 critical-path
+ 1 optional Phase 3b). No orphans.

---
*Last updated: 2026-06-14 after ingest-driven project bootstrap*
