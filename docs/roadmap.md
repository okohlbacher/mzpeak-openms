# mzPeak C++ — Consolidated Roadmap (reader + writer)

One ordered, dependency-aware plan unifying the reader gaps
([reader-backlog.md](reader-backlog.md) RDR-1…25) and the writer phases
([writer-implementation-research.md](writer-implementation-research.md)).
Each phase is a self-contained, testable increment validated by the
forward/reverse + cross-impl harness ([e2e-testing.md](e2e-testing.md)).

## Done (branch `writer_test`)
- **RDR-1** small/empty Parquet open fix (Arrow buffer).
- **Writer P0/P1a/P1b** point directory + zip-STORE archive + spectra_metadata → **T2 cross-impl PASS** (Rust reads C++ output).
- **e2e harness** T1 (forward intra), T2 (forward cross), T3 (reverse intra).

## Phases (ordered)

### Phase 1 — Reader correctness quick wins  *(small, independent, no design)*
- **RDR-2** missing row-group stats → conservatively INCLUDE (not prune).
- **RDR-11** `record_count` fallback (missing count KV → real count, not 0/max).
- **RDR-12** null-check nullable columns in query eval.
- **RDR-14** throw hygiene (replace `throw("...")` const-char* with `MzPeak` exceptions).
Validate: existing suite stays green + targeted unit tests.

### Phase 2 — Reader spectrum coverage  *(the big win)*
- **RDR-3 + RDR-13** route spectrum sources by `entity_type`/`data_kind`; read `spectra_peaks.parquet`; a spectrum reads from whichever table holds its index (representation-aware where metadata is available); absent index → empty, never throw.
Validate: `small.mzpeak` → all 48 spectra non-empty, values match pyarrow; 0 throws.
Depends: none (peaks table shares the point schema the reader already decodes).

### Phase 3 — Reader index metadata + version  *(RDR-24)*
- Parse the index `metadata{}` block; read + **validate** `metadata.version`; surface the run-level blocks (`run`, instrument/software/sample/data_processing/scan_settings/file_description) as accessible JSON/structs.
Validate: open a versioned file; version exposed; unknown major version warns.

### Phase 4 — Writer centroid/peaks split  *(W1c)*
- `SpectrumData` gains a representation flag; centroid spectra written to `spectra_peaks.parquet` (point), profile to `spectra_data.parquet`; metadata `number_of_peaks`/representation set accordingly.
Validate: forward intra (C++ read via Phase 2) + T2 (Rust reads centroids).

### Phase 5 — Null-marking + m/z delta model  *(writer P2 + reader RDR-5, done together)*
- Writer: null-mark flanking zero-intensity points, fit the WLS `δmz~β0+β1·mz+β2·mz²` model, store `mz_delta_model` + transforms MS:1003901/1003902.
- Reader: reconstruct null m/z from the model (segment-median + regression); null intensity = 0.
Validate: encode→decode round trip within tolerance; cross-check vs Rust `fill_nulls_for`.

### Phase 6 — Chunked layout + numpress  *(writer P3 + reader RDR-6/7)*
- Reader: map list/nested column paths; `decode_chunked` (basic MS:1000576, delta MS:1003089); numpress (MS:1002312/1002314) via vendored ms-numpress.
- Writer: chunked encoder + numpress.
Validate: `small.chunked`/`small.numpress` readable (reverse cross); chunked round-trip.

### Phase 7 — Reader breadth  *(RDR-10 / RDR-8 / RDR-9)*
- Per-spectrum metadata exposure; chromatograms API (+ synthesized TIC/BPC); wavelength API.
Validate: has_uv chromatograms/wavelength vs pyarrow; reverse cross.

## Deferred (need external deps or large design — not implementable standalone)
- **RDR-19** OpenMS `MSSpectrum`/`MSExperiment` integration — needs the OpenMS build/headers (this is a standalone lib).
- **RDR-17** EIC pipeline, **RDR-20** Parquet decryption, **RDR-22** mmap, **RDR-23** page-index access, **RDR-15/16/18/21/25** by-id/RT/batch/cache/detail — large; after the above.
- **RDR-14 (G15)** remote/cloud reading.
- **RDR-4** full type system — implemented incrementally as phases require (unsigned index already works for small values; widen when chunked/aux need it).

## Validation invariants (every phase)
Keep `meson test` green; add a regression test per fix; re-run T2 (`scripts/e2e_cross_impl.sh`) after any writer change; semantic compare (not byte-diff).
