# Context (synthesized from ingest)

Running notes from the DOC-class sources (reader-backlog.md, e2e-testing.md) plus
context blocks from the SPEC docs. Attributed verbatim-ish per topic.

---

## Topic: Project state (where things stand 2026-06-14)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md
- Existing, working codebase. Status verified against code on branch `writer_test`
  (the backlog doc under-marked several done items). Suite 27/27 at the time of the
  roadmap; task framing reports 29/29 + cross-impl PASS.
- Done reader items: RDR-1/2/3/5/6/7/8/9/10/11/12/13/15/16/17/18/24/26/27/29 —
  point layout (profile+centroid, null-marked m/z, multi-intensity coalescing),
  chunked spectra (basic/delta/numpress-linear+SLOF), chromatogram + wavelength
  point read, per-spectrum + file-level metadata, by-id / RT-range / EIC / batch
  query, zip + directory containers.
- Done writer: point-layout MVP (profile→data, centroid→peaks, metadata table).
- Full e2e harness: `test/e2e_test.cpp` + `scripts/e2e.sh`, T1–T5.
- Shipped this round (2026-06-14, all on writer_test): DOC-1 (backlog reconcile +
  THIRD_PARTY.md, 674f09b), RDR-4a (fbe4b58), RDR-28a (13248fa), RDR-25 (2ffd823),
  RDR-22 open_buffer (8f030a5), WRT-2 (9780a4a).

## Topic: Current reader coverage (pre-this-round empirical snapshot)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md
- The backlog was derived from an empirical probe of the fixed reader (PRs #3–#5
  applied) against every bundled fixture + spec/Rust-reference cross-check,
  adversarially reviewed (codex). At backlog-authoring time the reader fully
  handled exactly one case (point-layout profile spectra in a non-empty data
  table); small.mzpeak centroid, chunked, numpress, has_uv were unserved — those
  gaps are the RDR-1..RDR-29 items, most now DONE.
- Total: 25 tracked reader gaps. Milestones R1 (correctness quick wins) … R8 (e2e
  gaps RDR-28/29). Suggested first PR was RDR-1+RDR-2; biggest coverage win RDR-3
  (peaks table); highest reference-parity value RDR-10 → RDR-24 → RDR-19.

## Topic: Reader dependency graph
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md
```
RDR-1 ──► RDR-9
RDR-3 ──► RDR-5 ──► (writer null-marking)
RDR-3 ──► RDR-10 ──► RDR-15, RDR-16, RDR-19, RDR-24
RDR-4 ──► RDR-6 ──► RDR-7
RDR-16 + RDR-3 + RDR-6 ──► RDR-17 (EIC)
RDR-21 ──► RDR-18 (batch)
RDR-23 ──► RDR-16/RDR-17
RDR-10 + RDR-24 + RDR-3 ──► RDR-19 (OpenMS integration)
```

## Topic: Adversarial-review workflow
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (adversarial review)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (status line)
- Plans are adversarially reviewed by two agents (Codex + Vibe). For the roadmap,
  both independently returned the same MUST-FIXes, revising the plan from
  "parallel-first" to "sequence the foundation (RDR-4 unsigned index FIRST), then
  parallelize". Key corrections: split RDR-28 (28a now / 28b deferred, no fixture),
  RDR-25 is not just "skip decode" (needs a metadata-driven source decision),
  RDR-22 buffer-backed zip is large (only buffer-backed directory is small), don't
  run 28+22+25 together.
- For the RDR-19 plan: codex corrections folded into §1/§4a/§5; the vibe run failed
  to complete. Codex flipped RDR-19 feasibility ("dependency obstacle already
  solved") and corrected the lossiness framing (§5: fidelity equals mzML's, not a
  hard wall) + the chunk-decoder reuse cost (§1: only null_fill + numpress lift
  trivially; chunk-assembly must be rewritten natively).

## Topic: RDR-19 feasibility flip (what changed)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§0)
- The earlier roadmap feared RDR-19 needed a heavy Arrow/Parquet/zip dep added to
  OpenMS and was "blocked: libOpenMS not built". Reality (verified): Arrow +
  Parquet + libzip + Boost are already OpenMS deps; columnar precedent exists
  (ParquetFile, MSExperimentArrowExport). The only real residual constraints are
  the local build chore (P0) and a small C++23→C++20 port of the reused cores.
  Net: RDR-19 is feasible and scoped, not blocked.

## Topic: Two-layer template insight
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (key refinement)
- Treat RDR-19 as two layers with two different precedents: a data/container layer
  (random-access Parquet+zip → copy SqMassFile/ParquetFile/ZipRandomAccessFile,
  NOT XML) and a metadata-mapping layer (PSI-MS CV-param → OpenMS objects → mirror
  MzMLHandler). mzPeak carries the same PSI-MS accessions mzML does, so the mzML
  reader/writer is the right template for metadata; the binary container is where
  it differs.

## Topic: Validation philosophy
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/e2e-testing.md
- Guiding principle (from the writer research): validate semantically (decode and
  compare values within tolerance), not by byte-diffing Parquet. Float tolerance
  1e-9 m/z, 1e-6 intensity. T1+T3 run in the normal `meson test` suite; T2/T4 need
  the Rust toolchain and run via the script (and longer-term an opt-in CI job).
  The `convert` Rust example is mzML→mzpeak only (cannot read mzpeak), so
  `read_spectrum`/`read` are the reader oracles.

## Topic: Local OpenMS build state
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§8)
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (Phase 5)
- An OpenMS source tree (`~/Claude/OpenMS`) and a partial CMake build
  (`~/openms_build`) exist locally; `libOpenMS` is NOT built — only deps
  (libOpenSwathAlgo, SQLiteCpp, sqlite3, yaml-cpp). Finishing the build is a long
  compile and the gating prerequisite for compiling/testing RDR-19.

## Topic: External references
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/reader-backlog.md
- PRs https://github.com/OpenMS/mzpeak/pull/3 and /pull/5 (the fixed-reader base).
- Companion docs (not ingested this round): reader-completion-gaps.md,
  reader-rust-parity.md, writer-implementation-research.md, roadmap.md.
- Rust reference (`hupo-mzpeak`) is the parity oracle throughout: reader.rs
  SpectrumSource / MSDataFileMetadata / ChromatogramSource stack; numerous
  file:line anchors cited for each reader item.
