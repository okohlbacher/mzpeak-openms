---
phase: 03-mzpeakfile-store
plan: 01
type: summary
status: PASSED
---

# 03-01 SUMMARY — MzPeakFile::store (point-layout round-trip)

## What was implemented

`MzPeakFile::store(const String& filename, const MapType& map)` in the OpenMS
tree (`src/openms/source/FORMAT/MzPeakFile.cpp`), the symmetric inverse of the
Phase-2 `load`. Writes an `MSExperiment` to a `.mzpeak` archive that `load`
re-reads into an equivalent experiment.

Flow:
1. Split spectra by `getType()`: `PROFILE` (and unknown) → `spectra_data`
   point columns; `CENTROID` → `spectra_peaks`. Each spectrum is copied and
   `sortByPosition()`-ed so points are ascending m/z; the global spectrum
   position `i` is the `spectrum_index` (and the `index=i` native id).
2. Build per-spectrum metadata rows.
3. Write Parquet members + `mzpeak_index.json` into a `File::TempDir`.
4. `ZipArchiveFile::zipDirectory(dir, filename)` → output `.mzpeak`.

`FileHandler::storeExperiment` gains a `case FileTypes::MZPEAK →
MzPeakFile().store(filename, exp)` dispatch (`FileHandler.cpp`).

## Table schemas written (match what Phase-2 load reads)

**spectra_data.parquet / spectra_peaks.parquet** — single top-level `point`
struct column (all children nullable):
- `spectrum_index` uint64
- `mz` float64 (LITERAL m/z, no null-marking / delta model)
- `intensity` float32

Data table always present (may be empty); peaks table only when ≥1 centroid
spectrum exists. Points emitted per spectrum in ascending m/z order.

**spectra_metadata.parquet** — single `spectrum` struct column, `index` as the
FIRST child (load reads it positionally), all children nullable:
- `index` uint64
- `id` large_utf8 (spectrum native id, else `index=i`)
- `MS_1000511_ms_level` uint8
- `time` float64 (RT in **seconds** — required for RT round-trip; the
  mzpeak-lib metadata writer omits this, so it was added deliberately)
- `MS_1000465_scan_polarity` int8 (+1 positive / −1 negative / 0 unknown)
- `MS_1000525_spectrum_representation` utf8 (`MS:1000128` profile /
  `MS:1000127` centroid)
- `MS_1003060_number_of_data_points` uint64
- `MS_1003059_number_of_peaks` uint64

This is the subset of the mzpeak-lib `spectrum` struct that the Phase-2 load
actually consumes (it ignores the richer columns present in the reference
`small.mzpeak`).

**mzpeak_index.json** — `files[]` (each `{name, entity_type:"spectrum",
data_kind}` with kinds `"data arrays"`, `"peaks"` (if any centroid),
`"metadata"`) + `metadata{version:"0.9.0"}` (minimal).

## Parquet / zip choices

- Parquet write: `parquet::arrow::WriteTable` (OpenMS ParquetFile idiom), one
  bounded row group (`min(num_rows, 1<<20)`, positive even when empty).
- Zip compression: **STORED** (uncompressed members), via
  `ZipArchiveFile::zipDirectory`, whose documented behaviour is `ZIP_CM_STORE`.
  Parquet is already internally compressed, the mzpeak format mandates STORE,
  and STORE keeps members randomly accessible for the libzip-backed reader.
  Empirically the round-trip load succeeds.

## Round-trip tolerances achieved

Driven from `src = load(small.mzpeak)` (48 spectra) → `store` → `rt = load`:
- `rt.size() == src.size()` (48); MS1 count preserved (14).
- Per spectrum (matched by native id): `ms_level` exact, `type` exact,
  RT within **1e-6** absolute, peak count exact.
- First/last peak m/z within **1e-6** absolute; first/last intensity within
  **1e-3** relative.

`MzPeakFile_test` PASSES (all sections, run with
`DYLD_FRAMEWORK_PATH=/Library/Frameworks`), including the existing load
assertions (unchanged) and the new store/round-trip + FileHandler
store-dispatch sections.

## Style conformance

- `clang-format -i` applied to `MzPeakFile.cpp` and `MzPeakFile_test.cpp`
  (diff-verified clean against the project `.clang-format`); rebuilt + retested
  after formatting.
- `FileHandler.cpp` (legacy) edited minimally; new `MZPEAK` case matches the
  surrounding `case` style. No-trailing-whitespace / final-newline checked on
  edited regions (pre-existing legacy trailing-ws lines left untouched to keep
  the diff minimal).

## Commit hashes

- OpenMS (`feature/mzpeak-file-handler`): **4adc66a** —
  `[mzpeak] Implement MzPeakFile::store (point-layout round-trip)`
  (MzPeakFile.h, MzPeakFile.cpp, FileHandler.cpp, MzPeakFile_test.cpp).

## Deferrals to 03-02

- **Rust cross-impl re-read** of the C++-written archive (page-index /
  index-first-child hardening, ZSTD/SortingColumn/store_schema parity — the
  store currently writes plain `WriteTable` without explicit page index /
  sorting columns / stored Arrow schema).
- **Run-level metadata emit (WRT-2)**: instrument / software / source-file /
  sample from `ExperimentalSettings` → `metadata{}` (only `version` is emitted
  now).
- **Precursor / selected-ion facet emit** for MS2 (isolation window,
  activation, selected ion) — not written, so precursor info does not yet
  round-trip on store.
- **Null-marked / delta-model / chunked m/z** emit (WRT-1) — store writes
  literal m/z only.
- Per-spectrum CV params, `mz_delta_model`, and the richer reference columns
  are not emitted.

## Self-Check: PASSED
