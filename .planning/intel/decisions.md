# Decisions (synthesized from ingest)

No ADRs were ingested. The four source docs are SPEC/DOC class; they carry no
formal architecture-decision records. The design choices below are extracted
from the SPEC-class docs (roadmap-remaining.md, openms-integration-plan.md) as
**de-facto decisions** for downstream traceability. None are LOCKED; all are
overridable by a future ADR.

---

## DEC-arch-hybrid-handler — RDR-19 lands as a native OpenMS hybrid handler (option C)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§1)
- status: proposed (recommended in plan; not locked)
- decision: Implement `MzPeakFile` as a native OpenMS handler in OpenMS style
  (table reading + `MSExperiment` conversion) that **reuses the hard tested
  numeric kernels** (`null_fill`, numpress wrapper) but **re-implements the
  ~200-line chunk-assembly loop natively** against `arrow::ChunkedArray`.
  Rejected: (A) pure native re-implementation (throws away spec work), (B) vendor
  the whole C++23 reader as a subtree (forces a full C++23→C++20 port + two
  styles in-tree).
- scope: where the RDR-19 adapter code lives and how much mzPeak code is reused.

## DEC-data-layer-not-xml — Data/container layer copies columnar handlers, not the XML SAX path
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§0, §2)
- status: proposed
- decision: Read rows via OpenMS's existing `ParquetFile` / `ZipRandomAccessFile`
  helpers with row-group random access (like `SqMassFile`/`CachedMzML`), NOT the
  MzML XML SAX stream. Rationale: Arrow + Parquet + libzip + Boost are **already
  OpenMS deps** (vcpkg.json, package_general.cmake), so no new heavy dependency
  is introduced — the earlier "blocked, needs heavy dep" assumption is void.
- scope: the data/container layer of the OpenMS handler.

## DEC-metadata-mirror-mzml — Metadata layer mirrors MzMLHandler CV dispatch, not SqMassFile
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§4a)
- status: proposed
- decision: mzPeak carries the **same PSI-MS CV accessions** as mzML, so the
  metadata-mapping template is `MzMLHandler::handleCVParam_` (~332-accession
  dispatch keyed by parent context), not `SqMassFile`. v1 ships **option M2**
  (mirror the accession→setter subset mzPeak actually emits, self-contained, no
  mzML regression risk). **M1** (refactor `handleCVParam_` into a shared
  `applyCVParam` helper so mzML and mzPeak map identically) is proposed as a
  follow-up consolidation PR to OpenMS, not v1.
- scope: per-spectrum + run-level metadata mapping strategy.

## DEC-whole-experiment-first — Whole-experiment load before streaming transform
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§3, §8)
- status: proposed
- decision: Implement `load(MSExperiment&)` (whole-experiment) first; add the
  streaming `IMSDataConsumer transform` second. `DetailLevel::MetadataOnly` maps
  to `PeakFileOptions::setMetadataOnly`/`setFillData(false)`.
- scope: handler load API sequencing.

## DEC-pr-target-openms-repo — RDR-19 ships as a PR to OpenMS/OpenMS, not OpenMS/mzpeak
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§9)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (Phase 5)
- status: proposed
- decision: The adapter belongs in the OpenMS tree as an mzPeak file handler
  (OpenMS conventions: CamelCase, OPENMS_DLLAPI, $Maintainer$ headers, Doxygen,
  OpenMS test harness) rather than adding a heavy OpenMS build-dependency to the
  mzPeak library. Architecture decision noted as still-open in roadmap-remaining
  Phase 5; the integration plan resolves it toward "in the OpenMS tree".
- scope: repository / packaging boundary for RDR-19.

## DEC-rdr4-first — RDR-4 unsigned-index foundation is sequenced FIRST and isolated
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (Phase 1, adversarial review §1)
- status: proposed (DONE for RDR-4a this round)
- decision: The narrow unsigned `UInt64` entity-index typing (RDR-4a) is the
  critical path everything else rebases on; do it before the reader coverage
  items (RDR-28/25/22), in its own worktree, keeping the signed path compiling
  until the unsigned path is proven on every fixture. RDR-4b
  (large_list/large_string/large_list<u8>) is a documented follow-on (the chunked
  decoder already casts those directly → not blocking).
- scope: sequencing of the reader foundation.

## DEC-defer-blocked-infra — Blocked/external/perf items are deferred, not in active scope
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (Remaining open, Phase 5)
- status: proposed
- decision: RDR-20 (Parquet AES decryption — no fixtures/keys), RDR-21 (LRU cache)
  / RDR-23 (page-index) (perf only, no behavioral fixture), RDR-14 (remote/cloud,
  P4 future), WRT-1 (writer chunked/numpress emit, separate multi-month project),
  RDR-28b (chunked wavelength, no fixture), RDR-4b are explicitly deferred as
  blocked, no-functional-benefit, or multi-month.
- scope: which open items are out of active scope and why.

## DEC-split-28-and-22 — Split RDR-28 and RDR-22 by fixture availability / cost
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (adversarial review §2/§4, Phase 2/3)
- status: proposed (RDR-28a + RDR-22 buffer DONE this round)
- decision: RDR-28a (chunked chromatograms — fixtures exist in small.chunked /
  small.numpress) is validatable now; RDR-28b (chunked wavelength) deferred (no
  fixture, decoder hard-casts DoubleArray vs Float32). RDR-22a (buffer-backed
  directory) is small; RDR-22b (buffer-backed zip) is large (zip.cpp reopens per
  member) — though roadmap-remaining notes RDR-22 buffer landed this round.
- scope: decomposition of the access-mode items.
