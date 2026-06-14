---
phase: 01-reader-lossless-map-prerequisites
plan: "01"
subsystem: reader/metadata
tags: [rdr-10b, spectrum-metadata, cvparam, arrow, parquet, boost-test]
dependency_graph:
  requires: []
  provides:
    - SpectrumMetadata::spectrum_type
    - SpectrumMetadata::lowest_observed_mz
    - SpectrumMetadata::highest_observed_mz
    - SpectrumMetadata::data_processing_ref
    - SpectrumMetadata::parameters (vector<CvParam>)
    - extract_one_cv_param (file-local helper, reusable by 01-02/01-03)
    - read_cv_params_from_list (file-local helper, reusable by 01-02/01-03)
    - format_double_canonical (M2 determinism, reusable by 01-02/01-03)
  affects:
    - include/mzpeak/spectrum_metadata.h
    - src/util/metadata_model.cpp
    - include/mzpeak/util/metadata_model.h
    - test/spectrum_metadata_test.cpp
    - meson.build
tech_stack:
  added: []
  patterns:
    - Arrow LargeListArray value_offset + value_length iteration (mirrors
      read_mz_delta_models)
    - extract_one_cv_param single-item factoring pattern for 01-02/01-03 reuse
    - format_double_canonical deterministic double serialization (M2)
key_files:
  created:
    - test/spectrum_metadata_test.cpp
  modified:
    - include/mzpeak/spectrum_metadata.h
    - src/util/metadata_model.cpp
    - include/mzpeak/util/metadata_model.h
    - meson.build
decisions:
  - Store Spectrum in a named local before taking a reference to its metadata();
    spectra[i] returns a by-value temporary so const auto& m = spectra[i].metadata()
    is UB (dangling reference). All test cases use auto s = spectra[i] first.
  - Use value_length(row) not value_offset(row+1)-value_offset(row) for the
    large_list iteration to avoid the off-by-one at the last row of a chunk.
  - format_double_canonical uses ostringstream + setprecision(17) + strip trailing
    zeros; std::to_string is forbidden for float-arm values (yields "35.000000"
    not "35").
metrics:
  duration_minutes: 50
  completed_date: "2026-06-14"
  tasks_completed: 4
  tasks_total: 4
  files_modified: 5
---

# Phase 1 Plan 01: RDR-10b Scalar Fields + Parameters CvParam List Summary

**One-liner:** Extended SpectrumMetadata with spectrum_type / observed-mz /
data_processing_ref / parameters via Arrow LargeList decode; factored
extract_one_cv_param + read_cv_params_from_list + format_double_canonical
helpers for reuse by plans 01-02 and 01-03.

## What Was Built

### Task 1: Test scaffold (4e00a45)

Created `test/spectrum_metadata_test.cpp` with the repo license banner,
`BOOST_TEST_MODULE SpectrumMetadata`, and a smoke test that opens
`small.mzpeak` and asserts `ms_level == 1` on index 0 (pre-existing field).
Registered `spectrum_metadata` in `meson.build` `test_names` list.

### Task 2: RDR-10b scalar fields (a006264)

Added to `SpectrumMetadata`: `spectrum_type` (string), `lowest_observed_mz`
(optional<double>), `highest_observed_mz` (optional<double>),
`data_processing_ref` (string), and `parameters` (vector<CvParam>).

Extended `read_spectra_metadata` per-row loop to fill all four scalar fields
using the existing `get_string` / `opt_double` helpers and the exact column
names from the pyarrow-verified schema. Updated `metadata_model.h` Doxygen.

Test cases assert: `spectrum_type == "MS:1000579"` for index 0,
`lowest_observed_mz` within 1e-9 of 200.00018816645024,
`highest_observed_mz` within 1e-9 of 1999.9857293095915, and
`data_processing_ref.empty()` for all 48 rows.

### Task 3: Parameters CvParam list + factored helpers (8abd3b7)

Added three reusable file-local helpers to `metadata_model.cpp`:

- `format_double_canonical(double) -> string`: deterministic serialization;
  strips trailing zeros (35.0 -> "35", 1.5 -> "1.5"). Uses ostringstream +
  setprecision(17). Forbidden: std::to_string.

- `extract_one_cv_param(const StructArray&, int64_t k) -> CvParam`: reads
  accession (string), name (large_string), unit (string), and the value union
  struct (arms: "integer"/Int64, "float"/Double, "string"/LargeString,
  "boolean"/Bool) from item k of a flat items array. Security: every
  GetFieldByName is null-checked (T-01-01 mitigated).

- `read_cv_params_from_list(parent, field_name, row) -> vector<CvParam>`:
  resolves the large_list field, calls value_offset(row) + value_length(row),
  iterates [begin, begin+length), calls extract_one_cv_param per item.

Called `read_cv_params_from_list(spectrum, "parameters", r)` in the per-row
loop.

Test cases assert: `parameters.empty()` for all 48 small.mzpeak rows;
`parameters.size() == 1` for has_uv index 0 with accession "MS:1000796",
name "spectrum title", value starting with "TOFsulfas".

### Task 4: Style conformance gate (69b2aeb)

Ran `clang-format -i` on all four edited files. Diff-verify confirms empty
diff. editorconfig clean (LF, no trailing whitespace, final newlines).

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Dangling reference in test cases from temporary Spectrum**

- **Found during:** Task 3 — `parameters_populated_in_has_uv` failed even though
  debug output showed the data was read correctly.
- **Issue:** `const auto& m0 = spectra[0].metadata()` takes a reference to the
  metadata field of a temporary `Spectrum` returned by `spectra[0]`. The temporary
  is destroyed at the end of the full-expression, leaving `m0` dangling. The
  existing tests for scalar fields happened to pass because the stack memory
  was not yet overwritten, but it is UB.
- **Fix:** Rewrote all test cases to store `auto s = spectra[i]` in a named local
  before taking `const auto& m = s.metadata()`.
- **Files modified:** `test/spectrum_metadata_test.cpp`
- **Commit:** 8abd3b7

**2. [Rule 1 - Bug] LargeList iteration used value_offset(row+1) for end**

- **Found during:** Task 3 implementation review.
- **Issue:** The initial implementation used `la->value_offset(row+1)` for the
  end of the slice. For the last row in a chunk, `row+1` equals `la->length()`
  which is a valid index in Arrow's offset buffer (it stores N+1 offsets), but
  the safer canonical form is `value_length(row)` which directly gives the
  count without requiring the +1 index. Changed proactively.
- **Fix:** Use `la->value_length(row)` and iterate `begin + k` for `k` in
  `[0, length)`.
- **Files modified:** `src/util/metadata_model.cpp`
- **Commit:** 8abd3b7

## Known Stubs

None — all new fields are fully populated from the Parquet data for the
applicable fixtures. `data_processing_ref` is an empty string for all bundled
fixtures (null in the source), which is the correct observable value per
the API contract documented in the header.

## Threat Flags

No new network endpoints, auth paths, file access patterns, or schema changes
at trust boundaries introduced. The Parquet file trust boundary was already
present. T-01-01 (value-union null checks) is mitigated by design in
`extract_one_cv_param` (every `GetFieldByName` result is null-checked).

## Self-Check: PASSED

All files present:
- include/mzpeak/spectrum_metadata.h FOUND
- src/util/metadata_model.cpp FOUND
- test/spectrum_metadata_test.cpp FOUND
- .planning/phases/01-reader-lossless-map-prerequisites/01-01-SUMMARY.md FOUND

All commits present:
- 4e00a45 test(01-01): add spectrum_metadata test scaffold (Wave 0)
- a006264 feat(01-01): RDR-10b scalar fields on SpectrumMetadata + reader extension
- 8abd3b7 feat(01-01): RDR-10b parameters CvParam list + factored decode helpers
- 69b2aeb style(01-01): clang-format conformance gate for all Task 2-3 edited files

Full test suite: 30/30 OK (includes spectrum_metadata + all prior tests).
