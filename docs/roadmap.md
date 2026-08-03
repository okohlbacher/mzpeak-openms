# mzPeak C++ — Consolidated Roadmap (reader + writer)

One ordered, dependency-aware plan unifying the reader gaps
([reader-backlog.md](reader-backlog.md) RDR-1…25) and the writer phases
([writer-implementation-research.md](writer-implementation-research.md)).
Each phase is a self-contained, testable increment validated by the
forward/reverse + cross-impl harness ([e2e-testing.md](e2e-testing.md)).

## Done (merged to `trunk`)
- **RDR-1** small/empty Parquet open fix (Arrow buffer).
- **Writer P0/P1a/P1b** point directory + zip-STORE archive + spectra_metadata → **T2 cross-impl PASS** (Rust reads C++ output).
- **e2e harness** T1 (forward intra), T2 (forward cross), T3 (reverse intra).
- **Split-metadata ("v2") layout, reader and writer** — both generations read
  through one code path; the writer emits the split layout and the current Rust
  reference reads it. **T2 and T5 both PASS.** See
  [e2e-testing.md](e2e-testing.md#split-metadata-layout-the-v2-layout).
- **Reader correctness** (each validated against the Rust reference):
  null-marked m/z now reconstructed from its own adjacent run (was ~2.15 Da out
  on every profile spectrum); null-marked intensity reads as 0 rather than
  being delta-interpolated into negative values; the renamed
  `wavelength_spectrum` entity is recognised again.
- **Thread safety** — lazy peak decode serialised with `std::call_once` and
  shared across copies; ThreadSanitizer reports 0 races.

## Resolved: the imaging point-count difference was a harness bug

`Example_Processed.img.mzpeak` appeared to decode 2837 points against the
reference's 3007. It does not. The reference example writes an MGF block
*before* its `Raw Data:` section, and the comparison script skipped only one
line — so 170 MGF header lines were being counted as data points. Extracting
from `Raw Data:` onward gives 2837 on both sides, with m/z agreeing to 6.1e-05
(the imaging file stores float32 coordinates).

Only that fixture is affected: for `small.mzpeak` the reference emits
`Raw Data:` as its first line, so the earlier point/chunked comparisons against
it stand unchanged.

## Fixed since the review

- Multi-scan spectra report their earliest scan (92b8372).
- `Spectra` is non-copyable/non-movable, so its fetch callback cannot be left
  bound to another object (b599ba4).
- Decoded cardinality is checked: m/z against intensity, and both against the
  count the file declares for itself.
- Mixed collections size from BOTH tables, so centroid-only tail spectra are no
  longer omitted.
- Null reconstruction operates on the whole entity in the point layout too, not
  per Arrow record batch. Point and chunked decoding are now bit-identical, and
  the reconstructed-point error against the reference fell from 2.5e-06 to
  8.5e-07 Da.
- Unrecoverable null shapes are rejected instead of synthesised: a lone
  interior null is refused, per the spec's rule that unpaired nulls may appear
  only as the first or last value of an array.
- Coalesced point columns refuse a row where two sibling columns both carry a
  value; they may be in different units and preferring one would be a guess.
- `EnumerableProxy::Iterator` no longer declares defaulted move operations
  taking `const&&`, which is ill-formed and made the header fail to compile on
  GCC.
- `chunk_end` validation tightened to 1e-9 relative and extended with
  start <= end and chunk-ordering checks.

## Backlog — validation against real vendor files

Two capabilities are implemented and spec-conformant but have never been run
against real data, because no such file is available here. Neither should be
trusted numerically until one is.

- **Ion mobility** (`Spectrum::ion_mobility_array()`,
  `SelectedIonInfo::ion_mobility_lower/_upper_limit`). No bundled fixture
  carries a mobility array, so the tests pin only the CV mapping and the
  absent-data behaviour. Needs one real diaPASEF run: check that every MS2 scan
  reports a non-null mobility, that the array is parallel to m/z, and that the
  window limits partition the frame's peaks without overlap.
- **Bruker TDF "ims-compact"**. Exercised end to end by a hand-built fixture
  (`test/files/ims_compact.dir`), not a vendor archive. Needs one real TDF
  conversion: check the reconstructed m/z against the vendor's own values, and
  that Int32 intensities survive.

## Open — from the 2026-08-03 adversarial review (Codex)

Verified against the code but NOT yet fixed. Ordered by severity. Each is a
*silent* failure unless noted.

- **Coalesced intensity arrays do not expose their unit.** `has_uv` stores
  detector counts and absorbance as sibling columns; each chromatogram
  populates exactly one, so its values are internally consistent, but nothing
  in the API says WHICH unit they are in. The silent half is fixed — a row
  carrying both is now refused rather than resolved by preference — but the
  remaining gap is a missing accessor, i.e. a feature, not a defect.

- **Checked and NOT a defect — do not re-chase.** The numpress path skips null
  reconstruction on the assumption that numpress output is dense. One reviewer
  argued the reference converts zero to null and then fills for numpress-linear
  on the main axis, which would make our zeros silently wrong. Measured on
  `small.numpress.mzpeak` spectrum 0: 13589 points, **zero** m/z equal to 0.0,
  fully monotonic, leading values matching the point layout to ~1e-8 (numpress
  loss). The reference writer encodes the already-reconstructed dense array, so
  the assumption holds for its output. Revisit only if a writer is found that
  null-marks *and* numpress-encodes the same axis.

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

## Review corrections
- **Label fix:** throw hygiene is **RDR-13**, not RDR-14 (RDR-14 = remote/cloud). The Phase-1 commit used the wrong id; the fix itself is correct.
- **Phase 2 done as "whichever table holds the index"** (verified: small.mzpeak 48/48, centroid values match pyarrow). The *fully* correct form is representation-driven (`MS_1000525` + counts) for spectra that carry BOTH profile and centroid — a refinement to apply once the metadata reader (Phase 3) lands. Sizing uses the data table's `spectrum_count` (=48 total), which is why the collection exposes all 48.
- **Profile m/z interiors are still `0`** where null-marked (39,968 nulls) until RDR-5 — Phase-2 tests assert only centroid values + profile endpoints, never profile interiors, so nothing passes falsely.
- **RDR-4 (types) is not really deferrable** — uint64 indices work only for small fixture values; needed properly for Phase 5 `mz_delta_model` lists, Phase 6 nested/byte arrays, Phase 7 aux arrays. Pull it earlier.
- **RDR-2 caveat:** including stats-less row groups is safe for the sorted contiguous `spectrum_index` equality used today, but could yield false positives for non-contiguous predicates given `query_batch`'s first→last slice. Fine for current usage; revisit with RDR-17.
- **Phase 6 concrete blockers:** array-index paths like `chunk.mz_chunk_values` don't match the physical leaf `…list.item` (`parquet.cpp:243`); `decode_array` assumes one column (`encoding.h:74`); and `buffer_format_from_string` never parses `"chunk_transform"` → falls back to `Point` (`buffer_format.cpp:37`). Split Phase 6 into (path mapping + multi-column decode) → (delta) → (numpress).
- **Phase 3 fixture:** `small.dir`/`has_uv` index metadata have run-level blocks but **no** top-level `metadata.version` (their "version" strings are software versions). Validate version-read against a writer-generated fixture + a synthetic unknown-major.
- **Corrected order:** Phase1 → RDR-4 min types → minimal metadata reader → RDR-3 (done) → writer split → RDR-5 → RDR-24 → chunked → numpress → breadth.

## Validation invariants (every phase)
Keep `meson test` green; add a regression test per fix; re-run T2 (`scripts/e2e_cross_impl.sh`) after any writer change; semantic compare (not byte-diff).
