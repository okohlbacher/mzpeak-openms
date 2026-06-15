# 02-01 SUMMARY — MzPeakFile::load DATA LAYER (INT-01)

## What was built
Replaced the `MzPeakFile::load` NotImplemented stub in the OpenMS tree with a
real point-layout reader that opens a `.mzpeak` ZIP archive, reads its Parquet
tables via OpenMS's `ZipRandomAccessFile` + Arrow/Parquet, decodes the
point-layout spectra, reconstructs null-marked profile m/z, and builds a
populated `MSExperiment` (peaks, RT in seconds, ms_level, sorted, ranges).

Pipeline (`load`):
1. `File::exists` guard → `Exception::FileNotFound` on a missing path.
2. Read `mzpeak_index.json` bytes from the archive (Arrow `ReadAt`) and parse
   with `nlohmann/json`; select the `entity_type == "spectrum"` members by
   `data_kind`: `"data arrays"` (profile), `"peaks"` (centroid), `"metadata"`.
   Chromatogram members are ignored (per plan scope).
3. Read `spectra_metadata.parquet` `spectrum` struct column → per-index map of
   `{ms_level, retention_time (seconds), mz_delta_model betas}`.
4. Decode `spectra_data.parquet` (profile, `reconstruct=true`) and
   `spectra_peaks.parquet` (centroid, `reconstruct=false`). The point layout is
   a single top-level struct column `point` = `{spectrum_index u64, mz f64
   nullable, intensity f32}`; rows are grouped by `spectrum_index` (contiguous
   runs; multi-row-group tables are concatenated first).
5. Profile m/z null reconstruction (RDR-5): when a spectrum has interior NULL
   m/z, reconstruct via the ported `reconstruct_null_mz_` using the spectrum's
   `mz_delta_model` betas; decline (leave values) if the layout is not the
   interior-paired-null form.
6. Per spectrum: `MSSpectrum` with `Peak1D(mz,intensity)`, `setMSLevel`,
   `setRT(seconds)`, `setNativeID("index=<idx>")`, `sortByPosition()`,
   `addSpectrum`. Finally `sortSpectra(false)` + `updateRanges()`.

The mzpeak `null_fill` core (`predict_delta`, `estimate_median_delta`,
`reconstruct_null_mz`, `median_sorted`) was ported **near-verbatim** as
file-local helpers in `MzPeakFile.cpp` under OpenMS style (C++20, `std::sort` +
`std::erase_if`). No dependency on the mzpeak library. No new translation units
were added — all logic lives in the already-registered `MzPeakFile.cpp`.

## Files
OpenMS tree (`/Users/kohlbach/Claude/OpenMS`, branch `feature/mzpeak-file-handler`):
- `src/openms/source/FORMAT/MzPeakFile.cpp` — full `load` implementation + ported
  null_fill helpers + Arrow/Parquet/JSON readers (store/transform stay
  NotImplemented).
- `src/tests/class_tests/openms/source/MzPeakFile_test.cpp` — real `load`
  section with ground-truth assertions (ctor/dtor sections kept).
- `src/tests/class_tests/openms/data/small.mzpeak` — fixture committed (was
  untracked).
- Header `MzPeakFile.h` unchanged (public API identical to the stub).
- `sources.cmake` unchanged (`MzPeakFile.cpp` already listed).

## Commit hashes
- OpenMS tree: `b8d379d250193ba417f5c67e1e5c5260c4d5c669`
  `feat(MzPeakFile): point-layout load (data layer, INT-01)`
- openms-mzpeak (this summary): committed on `writer_test` (see repo log).

## Ground-truth values used (pyarrow oracle, with null reconstruction replicated)
- Totals: 48 spectra = 14 ms_level==1 (profile, `spectra_data.parquet`) + 34
  ms_level==2 (centroid, `spectra_peaks.parquet`).
- Profile spectrum mzpeak index 0 (id `controllerType=0 controllerNumber=1
  scan=1`): ms_level 1, RT 0.004935 s, 13589 points after reconstruction;
  strictly ascending m/z; no interior zero m/z; first m/z 202.60657495520474,
  last m/z 1999.8404377599534; intensity[0]=0, intensity[1]=1938.117431640625.
- Centroid spectrum mzpeak index 2 (scan=3): ms_level 2, RT 0.011218333333 s,
  485 peaks; first peak (231.3888397216797, 26.54511260986328), last peak
  (1560.7198486328125, 22.973094940185547).
- RT note: mzpeak `time` is stored in **seconds** (constraints.md:
  `retention_time → setRT (seconds)`); stored directly with no unit conversion,
  matching the reference reader.

## Test result
`MzPeakFile_test` compiles, links, and **PASSES**. Verbose run confirms every
assertion executed (count, ms_level split, ranges, profile reconstruction +
first/last m/z + intensity, centroid first/last m/z + intensity, RT in seconds,
FileNotFound on missing file). libOpenMS rebuilds clean (only a benign
duplicate-library link warning for arrow/parquet static libs).

## Style conformance
- `clang-format -i --style=file` applied to both edited files; re-verified with
  `clang-format --style=file f | diff f -` → CLEAN (not --dry-run).
- editorconfig: no trailing whitespace, no tabs, final newline present in both.
- OpenMS conventions: SPDX + `$Maintainer/$Authors: Oliver Kohlbacher$` headers,
  `OPENMS_DLLAPI` on the class (header unchanged), `Exception::*` error paths.

## Deviations / deferrals
- Spectra are stored in archive order (profile table first, then peaks table),
  then `sortSpectra(false)` orders the whole experiment by RT. The test locates
  spectra by native id (`index=N`) rather than positional index, so ordering is
  not asserted positionally.
- Metadata mapping beyond ms_level/RT (polarity, precursor, spectrum_type,
  run-level) is **deferred to plan 02-02** (per plan scope).
- FileHandler content-detection + dispatch deferred to **02-03**.
- Chunked / numpress layouts and chromatogram tables deferred (small.mzpeak is
  point-layout; chromatograms ignored for v1 per plan).
- Ion mobility NULL in fixtures; not handled.
- No profile-fidelity gap: full null reconstruction landed GREEN, so the
  fallback (centroid-only) contingency was not needed.

## Self-Check: PASSED
