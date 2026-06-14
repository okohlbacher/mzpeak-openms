# mzPeak C++ (OpenMS reader/writer)

## What This Is

A C++ library implementing the HUPO-PSI mzPeak mass-spectrometry file format,
together with an OpenMS `FORMAT` handler (`MzPeakFile`) that wires that library
into OpenMS data structures. The standalone reader and writer are already built,
tested, and committed; this milestone is the remaining work — making OpenMS able
to load and store `.mzpeak` natively into/from `MSExperiment`.

The audience is OpenMS itself and its tool ecosystem: once `MzPeakFile` is
registered in `FileTypes`/`FileHandler`, any OpenMS tool consumes mzPeak the way
it consumes mzML today.

## Core Value

OpenMS can load and store `.mzpeak` into/from `MSExperiment` (spectra,
chromatograms, run-level metadata), and the result cross-validates against the
mzML round-trip: mzML → mzpeak → mzML through OpenMS produces an equivalent
`MSExperiment`. If everything else is deferred, this round-trip equivalence is
the milestone.

## Requirements

### Validated

<!-- Shipped and confirmed valuable on branch writer_test this round. -->

- ✓ RDR-10 — per-spectrum scalar metadata via `Spectrum::metadata()` (033e874)
- ✓ RDR-24 — file-level metadata blocks + format version (950a76c / 5c7b1c2)
- ✓ RDR-4a — `UInt64` unsigned entity-index type system (fbe4b58)
- ✓ RDR-28a — chunked-layout chromatogram read (13248fa)
- ✓ RDR-25 — detail-level / metadata-only read mode (2ffd823)
- ✓ RDR-22 — in-memory / buffer archive source (`open_buffer`, 8f030a5)
- ✓ WRT-2 — writer emits run-level `metadata{}` blocks (9780a4a)

(~20 reader/writer features total are DONE and committed; suite 29/29,
cross-impl T1/T2/T3/T4/T5 PASS. The seven above are the items most relevant to
this milestone — they are the lossless-map prerequisites for RDR-19.)

### Active

<!-- This milestone's scope. Building toward these. -->

- [ ] REQ-openms-build — build `libOpenMS` locally so RDR-19 can compile/test
- [ ] REQ-cpp20-port — port the reused numeric cores (`null_fill`, numpress
  wrapper) from C++23 to C++20
- [ ] REQ-reader-prereqs-10b10c9b — close reader metadata gaps (RDR-10b
  per-spectrum CvParams/spectrum_type/observed-mz/data_processing_ref; RDR-10c
  precursor/isolation/activation/ion-mobility; RDR-9b auxiliary_arrays accessor)
- [ ] REQ-rdr19-openms-integration — `MzPeakFile` handler: `load`, `store`,
  FileTypes/FileHandler registration, mzPeak→MSExperiment + MSExperiment→mzPeak
  converters, streaming `transform` + `PeakFileOptions`, cross-validation

### Out of Scope

<!-- Deferred to a later milestone / backlog. Reasons recorded to prevent re-adding. -->

- WRT-1 (writer chunked/delta/numpress + null-marking emit) — a separate
  multi-month project; the v1 store path reuses the existing point-layout writer
- RDR-20 (Parquet modular AES decryption) — blocked: no encrypted fixtures/keys
- RDR-21 (LRU row-group cache) / RDR-23 (page/offset-index selection) — perf
  only, no behavioral fixture, weak validation; RDR-21 touches paths just
  changed by RDR-4a
- RDR-22b (buffer-backed *zip* source) — larger than the landed directory/buffer
  source (zip.cpp reopens per member); useful but deferred
- RDR-28b (chunked wavelength) — no fixture; decoder hard-casts DoubleArray while
  wavelength is Float32
- RDR-14 (remote/cloud: HTTP-range / S3 / object_store) — P4 future, network infra
- RDR-4b (large_list/large_string/large_list<u8> modeling) — no functional
  benefit today; the chunked decoder already casts those directly
- M1 (refactor OpenMS `handleCVParam_` into a shared `applyCVParam` helper) — a
  consolidation PR proposed to OpenMS as an *optional* follow-on (Phase 3b), not
  on the milestone critical path

## Context

- **Existing, working codebase.** Active branch `writer_test`. ~20 reader/writer
  features already DONE and committed; full suite 29/29, cross-impl matrix PASS.
  The remaining work is OpenMS integration (RDR-19) plus its prerequisites.
- **Two-layer template insight.** RDR-19 is two layers with two precedents: a
  data/container layer (random-access Parquet+zip → copy `SqMassFile` /
  `ParquetFile` / `ZipRandomAccessFile`, NOT the XML SAX path) and a
  metadata-mapping layer (PSI-MS CV-param → OpenMS objects → mirror
  `MzMLHandler`). mzPeak carries the same PSI-MS accessions mzML does, so the
  mzML reader/writer is the right template for metadata.
- **RDR-19 feasibility flip.** The earlier roadmap feared RDR-19 needed a heavy
  Arrow/Parquet/zip dependency added to OpenMS and was "blocked". Reality
  (verified): Arrow + Parquet + libzip + Boost are already OpenMS deps; columnar
  precedent exists (`ParquetFile`, `MSExperimentArrowExport`). The only residual
  constraints are the local `libOpenMS` build chore and the small C++23→C++20
  core port. RDR-19 is feasible and scoped, not blocked.
- **Validation philosophy.** Validate semantically — decode and compare values
  within tolerance (1e-9 m/z, 1e-6 intensity), never byte-diff Parquet
  (parquet-cpp vs parquet-rs differ in `created_by`, ZSTD streams, page layout).
  T1/T3 run in `meson test`; T2/T4/T5 need the Rust toolchain via scripts.
- **Local build state.** `~/Claude/OpenMS` source tree and a partial
  `~/openms_build` exist; `libOpenMS` is NOT yet built (only deps:
  libOpenSwathAlgo, SQLiteCpp, sqlite3, yaml-cpp). Finishing it is a long compile.
- **Reference oracle.** The Rust `hupo-mzpeak` reader is the parity oracle for
  reader fields throughout; the OpenMS `MzMLHandler` is the template for metadata
  mapping.

## Constraints

- **Tech stack**: OpenMS handler is C++20 (`cxx_std_20`); the standalone mzPeak
  meson library is C++23. Only the *reused cores* must compile as C++20 — audit
  for C++23-only usage (`std::bit_cast` + ranges are C++20, so none expected).
- **Dependencies**: Arrow / Parquet / libzip / Boost are already OpenMS deps —
  reuse them, add NO new heavy dependency. Existing columnar precedent:
  `FORMAT/ParquetFile.{h,cpp}`, `TransitionParquetFile`, `MSExperimentArrowExport`.
- **Metadata mapping mirrors `MzMLHandler`**: the mzML reader/writer is the
  template. v1 ships option M2 (mirror the emitted accession→setter subset,
  self-contained, no mzML regression risk). Do NOT re-derive a parallel CV table.
- **Repository boundary**: RDR-19 ships as a PR to OpenMS/OpenMS, not
  OpenMS/mzpeak. OpenMS conventions apply there: CamelCase, `OPENMS_DLLAPI`,
  `$Maintainer$`/`$Authors$` headers, Doxygen, OpenMS test harness.
- **Style conformance every phase** (standing gate): the repo enforces
  `.clang-format`. Mandatory per-phase pass — `clang-format -i` on every edited
  file + diff-verify (not `--dry-run`) + editorconfig — before committing.
- **Adversarial cross-AI review every phase** (standing gate): each phase plan is
  reviewed by two agents (Codex + Vibe) before execution. This is the project's
  established gate (it already revised the roadmap from "parallel-first" to
  "sequence the foundation first") and maps to the plan-review convergence step.
- **Semantic validation only** (no Parquet byte-diff): tolerance 1e-9 m/z,
  1e-6 intensity; `updateRanges()` mandatory; sort by m/z before any range API.
- **Isolation windows are offsets**, not absolute bounds (sign errors silently
  corrupt widths); map every selected ion, not just the first; SRM keeps both
  Precursor and Product.

## Key Decisions

<!-- The 8 de-facto design decisions from intel, treated as LOCKED for this milestone. -->

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| DEC-arch-hybrid-handler: RDR-19 is a native OpenMS hybrid handler (option C) — reuse the tested numeric kernels (`null_fill`, numpress), re-implement the ~200-line chunk-assembly loop natively against `arrow::ChunkedArray` | Keeps the hard-tested kernels; avoids a full C++23→C++20 port of the whole lib (rejected B) and avoids throwing away spec work (rejected A) | — Locked |
| DEC-data-layer-not-xml: read rows via OpenMS `ParquetFile` / `ZipRandomAccessFile` with row-group random access (like `SqMassFile`/`CachedMzML`), NOT the XML SAX path | Arrow/Parquet/libzip/Boost are already OpenMS deps — no heavy dep needed; columnar precedent exists | — Locked |
| DEC-metadata-mirror-mzml: metadata layer mirrors `MzMLHandler::handleCVParam_` CV dispatch; v1 ships M2 (mirror the emitted subset), M1 shared-helper consolidation proposed later | mzPeak carries the same PSI-MS accessions as mzML; M2 is self-contained with no mzML regression risk | — Locked |
| DEC-whole-experiment-first: implement `load(MSExperiment&)` whole-experiment first, then the streaming `IMSDataConsumer transform`; `DetailLevel::MetadataOnly` → `PeakFileOptions::setMetadataOnly` | Simpler correct baseline before streaming; metadata-only mode already exists in the reader | — Locked |
| DEC-pr-target-openms-repo: the adapter lands in the OpenMS tree as an mzPeak handler, not by adding a heavy build-dep to the mzPeak lib | OpenMS conventions and test harness fit the handler; avoids polluting the lib | — Locked |
| DEC-rdr4-first: the unsigned `UInt64` entity-index foundation (RDR-4a) is sequenced first and isolated, signed path kept compiling until proven | Critical-path foundation everything rebases on; predicate/stats >INT64_MAX is the easy-to-miss part | ✓ Good (RDR-4a DONE, fbe4b58) |
| DEC-defer-blocked-infra: blocked/external/perf/multi-month items (RDR-20/21/23/14, WRT-1, RDR-28b, RDR-4b) are deferred, not active scope | No fixtures (20/28b), perf-only (21/23), future infra (14), separate project (WRT-1) | — Locked |
| DEC-split-28-and-22: split RDR-28 and RDR-22 by fixture availability / cost (28a / 22 buffer landed; 28b / 22b deferred) | 28a + buffer source are validatable now; 28b lacks a fixture, 22b zip is large | ✓ Good (28a + buffer DONE) |

---

## Evolution

**After each phase transition:**
1. Requirements invalidated? → move to Out of Scope with reason
2. Requirements validated? → move to Validated with phase reference
3. New requirements emerged? → add to Active
4. Decisions to log? → add to Key Decisions
5. "What This Is" still accurate? → update if drifted

**After this milestone:** review Out of Scope (RDR-20/21/22b/23/28b/14, WRT-1,
M1) for promotion into the next milestone; re-confirm Core Value.

---
*Last updated: 2026-06-14 after ingest-driven project bootstrap*
