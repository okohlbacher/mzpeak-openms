# Requirements (synthesized from ingest)

No PRDs were ingested. Requirements below are extracted from the SPEC-class
roadmap (roadmap-remaining.md, authoritative phasing spine) and the RDR-19 design
SPEC (openms-integration-plan.md), with the reader-backlog DOC providing per-item
done-when detail. Each REQ traces to its source(s). Acceptance criteria are taken
verbatim where the source states a "done-when" / "validate".

Status legend: DONE (shipped on writer_test this round) · OPEN (remaining) ·
DEFERRED (blocked / no-benefit / multi-month).

---

## REQ-rdr19-openms-integration — Wire mzPeak reader/writer into OpenMS data structures
- id: REQ-rdr19-openms-integration
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (Phase 5, RDR-19)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (whole doc)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md (RDR-19)
- status: OPEN (the headline remaining work)
- priority: P2 (highest value)
- description: An OpenMS `MzPeakFile` handler that loads a `.mzpeak` archive into
  `MSExperiment` + `ExperimentalSettings` and stores an `MSExperiment` back to
  `.mzpeak`, registered in OpenMS FileTypes/FileHandler so OpenMS tools consume
  mzPeak natively.
- acceptance:
  - `MzPeakFile::load(filename, MSExperiment&)` round-trips a bundled fixture into
    `MSExperiment` asserting peaks, RT, ms_level, polarity, type, run metadata.
  - `MZPEAK` added to `FileTypes.h` enum + name/extension (`.mzpeak`) tables;
    dispatch added in `FileHandler::loadExperiment`/`storeExperiment`/`getType`;
    content detection (zip whose first member is `mzpeak_index.json`).
  - Per-spectrum mapping per §4 (sort by m/z + `updateRanges()` mandatory).
  - Metadata via §4a M2 (mirror the `handleCVParam_` accession subset).
  - Cross-validate (§8 P5): OpenMS reading C++/Rust-written mzPeak yields the same
    `MSExperiment` as reading the equivalent mzML; mzML→mzPeak→mzML round-trips.
- depends_on: REQ-rdr10-spectrum-metadata, REQ-rdr24-file-metadata, RDR-3 (done),
  REQ-openms-build, REQ-reader-prereqs-10b10c9b, REQ-cpp20-port.
- scope: new OpenMS handler + converter + registration; PR to OpenMS/OpenMS.

## REQ-openms-build — Build libOpenMS locally to compile/test against
- id: REQ-openms-build
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§8 P0)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (Phase 5)
- status: OPEN (prerequisite chore)
- description: A local OpenMS source tree (`~/Claude/OpenMS`) and partial CMake
  build (`~/openms_build`) exist, but `libOpenMS` is NOT built (only deps:
  libOpenSwathAlgo, SQLiteCpp, sqlite3, yaml-cpp). Finishing the build (a long
  compile) is the gating chore — not an architectural blocker.
- acceptance: `libOpenMS` builds and is linkable; `MzPeakFile_test` can compile.
- scope: build-environment prerequisite for RDR-19.

## REQ-reader-prereqs-10b10c9b — Close reader metadata gaps that bound a lossless map
- id: REQ-reader-prereqs-10b10c9b
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§7, §8 P1)
- status: OPEN
- description: Extend the reader so RDR-19 can map a lossless experiment.
- acceptance:
  - RDR-10b: `read_spectra_metadata` + `SpectrumMetadata` expose per-spectrum
    `parameters` (flat CvParam list), `spectrum_type`, observed-mz range,
    `data_processing_ref`.
  - RDR-10c: precursor / selected-ion / isolation-window / activation /
    ion-mobility columns (needs an MSn+IM fixture; mirror the Rust reader fields).
  - RDR-9b: a typed accessor for `auxiliary_arrays` (→ OpenMS FloatDataArrays).
  - Without these a v1 converter maps peaks + core scalars + run metadata only
    (documented as acceptable for a first PR).
- depends_on: RDR-10 (done), RDR-24 (done).
- scope: reader-side prerequisites in the mzPeak lib.

## REQ-cpp20-port — Port the reused numeric cores from C++23 to C++20
- id: REQ-cpp20-port
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§6)
- status: OPEN (constraint, small)
- description: OpenMS targets `cxx_std_20`; the mzPeak reader uses `cpp_std=c++23`.
  Under the hybrid option (C) only the **ported cores** (`null_fill`, numpress
  wrapper) must be C++20 — audit them for C++23-only usage (none obvious;
  `std::bit_cast` + ranges are C++20). The full lib does NOT need porting.
- acceptance: ported cores compile under `cxx_std_20` with no C++23-only features.
- scope: language-standard compatibility for the reused kernels.

## REQ-rdr10-spectrum-metadata — Expose per-spectrum metadata + secondary/auxiliary arrays
- id: REQ-rdr10-spectrum-metadata
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md (RDR-10)
- status: DONE (033e874; per-spectrum scalar metadata via Spectrum::metadata())
- description: Parse ms_level, scan, precursor, selected_ion, isolation window,
  activation, CV params, polarity, base-peak/TIC, secondary + auxiliary arrays.
- acceptance: spot-check ms_level/polarity/precursor and a secondary array vs
  pyarrow. (RDR-10b/10c/9b above are the remaining extensions for RDR-19.)
- scope: reader spectrum-metadata model.

## REQ-rdr24-file-metadata — Parse & expose file-level metadata blocks + format version
- id: REQ-rdr24-file-metadata
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md (RDR-24)
- status: DONE (version 950a76c; run-level blocks 5c7b1c2)
- description: Read `metadata{}` (+ Parquet KV) into typed structs with accessors;
  surface + version-check `metadata.version`. Prerequisite for OpenMS
  `ExperimentalSettings` mapping (RDR-19).
- acceptance: all seven run-level blocks parse; format version surfaced + checked.
- scope: file-level metadata reader.

## REQ-rdr4-typesystem — Unsigned/list/string/byte-array type system
- id: REQ-rdr4-typesystem
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (Phase 1)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md (RDR-4)
- status: DONE for RDR-4a (UInt64 entity index, commit fbe4b58); RDR-4b DEFERRED
- priority: P1
- description: `DataType` + Parquet type-tag maps cover `UInt32/UInt64`,
  list/large_list, string/large_string, large_list<u8>; the entity-index path
  uses the correct unsigned type.
- acceptance:
  - RDR-4a: all fixtures still read (full suite + e2e + cross-impl); synthetic
    large-index test exercising predicates/stats **above INT64_MAX** (not just
    decode); `record_count()`/Query stats casts covered (the easy-to-miss part).
  - RDR-4b: large_list/large_string/large_list<u8> modeling — DEFERRED, no
    functional benefit today (chunked decoder already casts those directly).
- scope: reader type system, the critical-path foundation.

## REQ-rdr28a-chunked-chromatograms — Read chunked-layout chromatograms
- id: REQ-rdr28a-chunked-chromatograms
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (Phase 2)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md (RDR-28)
- status: DONE (13248fa)
- priority: P3
- description: Route the chunked chromatogram data table through
  `Encoding::decode_chunked` (drop the throw in chromatograms.cpp:34).
- acceptance: small.chunked + small.numpress chromatograms read within tolerance
  vs pyarrow; has_uv point multi-intensity (RDR-29) stays green; e2e assertions.
  RDR-28b (chunked wavelength) DEFERRED — no fixture, decoder hard-casts
  DoubleArray while wavelength is Float32.
- scope: chunked auxiliary-table read path.

## REQ-rdr25-detail-level — Detail-level (metadata-only) mode
- id: REQ-rdr25-detail-level
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (Phase 3)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md (RDR-25)
- status: DONE (2ffd823)
- priority: P4
- description: `Index::spectra(DetailLevel)` skips array decode; data-vs-peaks
  source decided from metadata `number_of_data_points`/`number_of_peaks`, NOT
  decoded-array emptiness.
- acceptance: metadata-only read returns ids/ms_level/time with empty arrays; full
  mode unchanged.
- scope: read detail-level mode (maps to OpenMS PeakFileOptions metadata-only).

## REQ-rdr22-buffer-source — In-memory / buffer archive source
- id: REQ-rdr22-buffer-source
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (RDR-22, Phase 3)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md (RDR-22)
- status: DONE per roadmap-remaining (8f030a5, MzPeak::open_buffer over ZipBuffer)
- priority: P3
- description: Read a `.mzpeak` from an in-memory byte buffer (and/or mmap).
- acceptance: `open_buffer(bytes)` reads a ZIP from memory via a ZipBuffer archive
  over `zip_source_buffer`. NOTE: see INGEST-CONFLICTS INFO — the integration plan
  and earlier roadmap phasing call buffer-backed *zip* large/DEFERRED (RDR-22b),
  while roadmap-remaining (higher precedence) records the buffer source as landed.
- scope: archive source abstraction.

## REQ-wrt2-run-metadata-emit — Writer emits run-level metadata{} blocks
- id: REQ-wrt2-run-metadata-emit
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (WRT-2, Phase 4)
- status: DONE (9780a4a; RunMetadata::to_json serializer + writer overloads)
- priority: P3
- description: Writer emits run-level `metadata{}` via a typed `RunMetadata::to_json()`
  serializer; round-trips through `Index::metadata()`.
- acceptance: round-trips through the C++ reader's `Index::metadata()` and the Rust
  reader.
- scope: writer metadata emit (writer-side dual of RDR-24).

## REQ-wrt1-chunked-emit — Writer chunked/numpress + null-marking emit
- id: REQ-wrt1-chunked-emit
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (WRT-1, Phase 4)
- status: DEFERRED (confirmed out of scope: a separate multi-month project)
- priority: P2
- description: Writer emit of chunked/delta/numpress + null-marking (nested Arrow
  schema + chunk segmentation + null-marking + delta-model fit + numpress encode).
- acceptance: n/a — explicitly out of scope.
- scope: writer encoder breadth.

## REQ-rdr20-aes-decryption — Parquet modular (AES) decryption
- id: REQ-rdr20-aes-decryption
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (RDR-20, Phase 5)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md (RDR-20)
- status: DEFERRED (blocked — no encrypted fixtures/keys)
- priority: P3
- description: Plumb Arrow C++ `FileDecryptionProperties` + a key API.
- scope: encrypted-Parquet read path.

## REQ-rdr21-cache-rdr23-pageindex — LRU cache + page/offset-index random access
- id: REQ-rdr21-cache-rdr23-pageindex
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (RDR-21/23, Phase 5)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md (RDR-21, RDR-23)
- status: DEFERRED (perf only; no behavioral fixture, weak validation)
- priority: P3
- description: RDR-21 LRU cache of decoded row groups; RDR-23 page/offset-index
  selection reading only relevant pages. RDR-21 touches data_arrays/parquet paths
  just changed by RDR-4a.
- scope: read-path performance infra.

## REQ-rdr14-remote-cloud — Remote / cloud reading
- id: REQ-rdr14-remote-cloud
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (RDR-14, Phase 5)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md (RDR-14)
- status: DEFERRED (P4 future — network infra)
- priority: P4
- description: HTTP-range / S3 / object_store prefix reading; `open()` currently
  rejects non-local paths.
- scope: remote archive source (future).
