---
phase: 01-reader-lossless-map-prerequisites
plan: "02"
subsystem: reader/metadata
tags: [rdr-10c, precursor, selected-ion, isolation-window, activation, scan-windows, arrow, parquet, boost-test, join-by-value, dia-safe]
dependency_graph:
  requires:
    - extract_one_cv_param (plan 01-01)
    - read_cv_params_from_list (plan 01-01)
    - format_double_canonical (plan 01-01)
    - opt_int/opt_double/opt_float/get_string (plan 01-01)
    - SpectrumMetadata (plan 01-01 base struct)
  provides:
    - IsolationWindow struct
    - SelectedIonInfo struct (IM fields as deferred stubs)
    - PrecursorInfo struct (precursor_index as DIA-safe H2 within-spectrum key)
    - ScanWindow struct (float lower/upper_limit matching Arrow float32 schema)
    - SpectrumMetadata::precursors (vector<PrecursorInfo>, empty for MS1)
    - SpectrumMetadata::scan_parameters (vector<CvParam>)
    - SpectrumMetadata::scan_windows (vector<ScanWindow>)
    - source_index VALUE key-map join (H1 fix) in read_spectra_metadata
    - (source_index, precursor_index) selected-ion attach (H2 fix)
  affects:
    - include/mzpeak/spectrum_metadata.h
    - src/util/metadata_model.cpp
    - include/mzpeak/util/metadata_model.h
    - test/spectrum_metadata_test.cpp
    - meson.build
tech_stack:
  added: []
  patterns:
    - Source_index VALUE key-map join: facet row reads source_index VALUE,
      then out.find(*si); NEVER indexes out positionally; log+skip unmatched
    - H2 DIA-safe ion attach: linear search precursors by precursor_index;
      create holder PrecursorInfo if precursor_index not yet present
    - Single-pass table read: ReadTable once, all four facet passes in-memory
    - Nested struct navigation: GetFieldByName("isolation_window") -> StructArray
    - LargeListArray scan_windows: value_offset + value_length iteration (same
      idiom as read_cv_params_from_list from plan 01-01)
    - Float32 scan window limits: ScanWindow uses optional<float> matching the
      Arrow float32 schema (not double as plan spec assumed)
key_files:
  created: []
  modified:
    - include/mzpeak/spectrum_metadata.h
    - src/util/metadata_model.cpp
    - include/mzpeak/util/metadata_model.h
    - test/spectrum_metadata_test.cpp
    - meson.build
decisions:
  - DEC-01-02-float-scan-window: ScanWindow.lower_limit/upper_limit are
    optional<float> not optional<double>; the Arrow schema stores
    MS_1000501/MS_1000500 as float32 (verified via pyarrow); using double
    would require implicit widening and mismatch the type_id() guard in
    opt_float; plan spec said double but schema is authoritative.
  - DEC-01-02-clang-format-meson-excluded: meson.build is not a C++ file;
    clang-format reformatted it incorrectly (URL parsing error, argument
    reflow); reverted and excluded from the style gate.  The plan's verify
    step lists only C++ files; meson.build style is not covered by
    clang-format.
  - DEC-01-02-test-timeout-120s: spectrum_metadata test moved to a separate
    meson test() call with timeout=120s; the 18-case multi-row suite (each
    case opens small.mzpeak independently for isolation) runs in ~33s and
    was hitting the 30s default timeout.  120s gives CI headroom.
  - DEC-01-02-table-read-once: refactored read_spectra_metadata to call
    ReadTable once and keep the table shared across all four passes; the
    original spectrum_column() helper re-read the table, which would have
    caused a second Parquet read for the facet passes (RESEARCH anti-pattern).
metrics:
  duration_minutes: 55
  completed_date: "2026-06-14"
  tasks_completed: 3
  tasks_total: 3
  files_modified: 5
---

# Phase 1 Plan 02: RDR-10c Precursor/Scan Join Summary

**One-liner:** Implemented source_index VALUE key-map join (H1) and
(source_index, precursor_index) DIA-safe ion attach (H2) for precursor,
selected_ion, and scan columns; exposed isolation window, CID activation,
selected-ion m/z/intensity, scan_parameters, and scan_windows with 18-case
multi-row sweep (34 MS2 / 14 MS1) verified against pyarrow ground truth.

## What Was Built

### Task 1: Struct definitions (c258016)

Added to `include/mzpeak/spectrum_metadata.h`:

- `IsolationWindow`: target_mz, lower_offset, upper_offset (all float,
  matching the float32 Arrow schema), plus parameters vector.
- `SelectedIonInfo`: selected_ion_mz (double), charge_state (int, nullable),
  intensity (float), ion_mobility_value/type (optional, deferred stubs),
  parameters.  Doxygen explicitly documents IM fields as deferred.
- `PrecursorInfo`: precursor_index (optional<uint64_t>, DIA-safe H2 key —
  not an array position), precursor_id, isolation_window,
  activation_parameters, selected_ions.
- `ScanWindow`: lower_limit, upper_limit (float, matching float32 schema),
  parameters.
- Added to `SpectrumMetadata`: precursors (vector<PrecursorInfo>),
  scan_parameters (vector<CvParam>), scan_windows (vector<ScanWindow>).

### Task 2: Precursor + selected_ion join (GREEN: 2e64711 RED, 6200457 GREEN)

Extended `read_spectra_metadata` in `src/util/metadata_model.cpp`:

**Single table read:** Refactored `spectrum_column()` helper into
`read_metadata_table()` + `spectrum_column_from_table()` so all four
passes share one in-memory Arrow table (no second Parquet read).

**PASS 2 — precursor:** For each chunk-local row r, reads `source_index`
via opt_int<uint64_t>; NULL -> skip (MS1 rows).  `out.find(*si)` -> if
missing, logs to stderr ("precursor source_index N has no matching spectrum
— skipped") and continues; never silently drops (T-02-01).  Builds
PrecursorInfo: precursor_index, precursor_id, isolation_window (nested
StructArray via GetFieldByName("isolation_window"), then
MS_1000827/828/829 via opt_float), activation_parameters (via
read_cv_params_from_list on the activation sub-struct).

**PASS 3 — selected_ion:** Runs AFTER PASS 2 so precursors exist.
H2: reads both source_index (-> spectrum) and precursor_index (-> which
precursor).  Linear-searches `it->second.precursors` for matching
precursor_index; if not found, creates a holder PrecursorInfo.  NEVER
uses precursors.back().  IM fields left as nullopt (deferred).

### Task 3: Scan parameters + scan_windows + style gate (6200457)

**PASS 4 — scan:** Same source_index join.  `scan_parameters` via
read_cv_params_from_list; `scan_windows` via LargeListArray
(value_offset + value_length, same idiom as read_cv_params_from_list),
each item built as ScanWindow (opt_float for lower/upper_limit — float32
in schema, not double as the plan spec said).

**Style gate:** `clang-format -i` on all four C++ files; diff-verify
(not --dry-run) confirmed empty diff for all files.  meson.build excluded
(not a C++ file; clang-format reformatted it incorrectly).

### Tests (18 cases, 34.3s runtime, 33.2s green)

All new test cases:
- `precursor_ms2_index2_has_one_precursor`: H1 join by source_index VALUE
- `precursor_isolation_window_index2`: target_mz 810.789±1e-3, offsets 1.0
- `precursor_activation_parameters_index2`: MS:1000133 (CID) + MS:1000045
  (collision energy value "35" via format_double_canonical, unit UO:0000266)
- `precursor_selected_ion_index2`: mz 810.789428±1e-6, intensity 1994039.125±1e-3
- `ms1_spectrum_has_empty_precursors`: spectrum index 0 carries no precursors
- `multirow_sweep_ms2_carry_precursor_ms1_carry_none`: EXACTLY 34 MS2 with
  precursor, 0 MS2 without, 14 MS1 without, 0 MS1 with (H1 multi-row sweep)
- `h2_selected_ion_attach_by_precursor_index`: for indices 2 and 3, each
  precursor's precursor_index==1 and each has 1 selected ion; index 3
  spot-check: selected_ion_mz 837.344604±1e-6
- `scan_parameters_ms1000800_present`: MS:1000800 (mass resolving power)
  integer-arm value "100000" present on at least one spectrum
- `scan_windows_present_on_ms1_spectrum`: index 0 has window lower=200,
  upper=2000 (float; tolerance 1e-3)
- `scan_windows_source_index_join_correct_for_two_spectra`: H1 join
  verified for indices 0 (200-2000) and 2 (210-1635) independently

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] ScanWindow.lower/upper_limit typed as float, not double**

- **Found during:** Task 1 + Task 3 implementation — checked pyarrow schema.
- **Issue:** The plan's `<tasks>` specified `optional<double>` for ScanWindow
  lower/upper_limit, but the Arrow schema stores these fields as `float`
  (float32: `MS_1000501_scan_window_lower_limit_unit_MS_1000040: float`).
  Using double would bypass the `type_id() == arrow::Type::FLOAT` guard in
  `opt_float` and return nullopt for all scan windows.
- **Fix:** Changed ScanWindow to `optional<float>` to match the actual schema;
  read via opt_float; test assertions use `static_cast<double>` for comparison.
- **Files modified:** `include/mzpeak/spectrum_metadata.h`,
  `src/util/metadata_model.cpp`, `test/spectrum_metadata_test.cpp`
- **Commit:** c258016 (struct), 6200457 (implementation)

**2. [Rule 3 - Blocking] Test timeout needed 120s, default is 30s**

- **Found during:** Task 2 / GREEN phase — meson test timed out (SIGTERM at 30s).
- **Issue:** 18 independent test cases each opening small.mzpeak take ~33s
  total; the default meson `test()` timeout of 30s killed the suite before
  completion.
- **Fix:** Moved `spectrum_metadata` out of the generic `test_names` loop and
  into a separate `test()` call with `timeout : 120`.
- **Files modified:** `meson.build`
- **Commit:** 6200457

**3. [Rule 1 - Bug] Second Parquet read in original spectrum_column() helper**

- **Found during:** Task 2 implementation — the original `spectrum_column()`
  called `metadata.reader().ReadTable(&table)` on every invocation. Adding
  facet passes would have caused a second (and third, fourth) Parquet read,
  violating the RESEARCH anti-pattern.
- **Fix:** Refactored into `read_metadata_table()` (reads table once) +
  `spectrum_column_from_table()` (returns column from already-read table);
  read_spectra_metadata owns the single ReadTable call.
- **Files modified:** `src/util/metadata_model.cpp`
- **Commit:** 6200457

**4. [Rule 1 - Bug] clang-format must not be applied to meson.build**

- **Found during:** Task 3 style gate — `clang-format -i meson.build`
  reformatted the URL in a comment as a C++ label (`https:` became a goto
  label), mangled the project() call, and broke argument alignment.
- **Fix:** Reverted meson.build via `git checkout -- meson.build`; excluded
  meson.build from the clang-format loop; the plan's verify step only lists
  C++ source files for format checking.
- **Files modified:** None (revert)

## Known Stubs

- `SelectedIonInfo::ion_mobility_value` and `ion_mobility_type`: present as
  `optional<double>` and `optional<std::string>` members but always
  `nullopt`; the decode path is DEFERRED pending an IM fixture (NULL in all
  bundled files). Documented in header and metadata_model.h Doxygen.
- `PrecursorInfo::isolation_window.parameters`: always empty in bundled
  fixtures; the field is populated but reads 0 items. Expected; not a stub
  issue.

These stubs do not prevent the plan's goal (RDR-10c precursor/scan join) from
being achieved; the IM-deferred item is in STATE.md Deferred Items.

## Threat Flags

No new network endpoints, auth paths, or schema changes at trust boundaries
beyond the already-present Parquet file trust boundary.

Threat mitigations implemented as planned:
- T-02-01: source_index VALUE join + stderr log for unmatched rows (no silent
  loss); implemented in all three facet passes.
- T-02-02: (source_index, precursor_index) ion attach with holder-PrecursorInfo
  creation; never attaches to precursors.back().
- T-02-03: IM fields not read; left as nullopt stubs.

## Self-Check: PASSED

Files present:
- include/mzpeak/spectrum_metadata.h FOUND
- src/util/metadata_model.cpp FOUND
- include/mzpeak/util/metadata_model.h FOUND
- test/spectrum_metadata_test.cpp FOUND
- .planning/phases/01-reader-lossless-map-prerequisites/01-02-SUMMARY.md FOUND

Commits present:
- c258016 feat(01-02): define PrecursorInfo/SelectedIonInfo/IsolationWindow/ScanWindow structs
- 2e64711 test(01-02): add failing RDR-10c tests for precursor/scan join (RED)
- 6200457 feat(01-02): read precursor/selected_ion/scan via source_index key-map join
- 10641ab style(01-02): clang-format conformance on spectrum_metadata_test.cpp

Full test suite: 30/30 OK (spectrum_metadata 18/18 green, runtime ~34s).
