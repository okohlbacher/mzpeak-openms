---
phase: 01-reader-lossless-map-prerequisites
verified: 2026-06-14T12:00:00Z
status: passed
score: 3/3
overrides_applied: 0
---

# Phase 1: Reader Lossless-Map Prerequisites — Verification Report

**Phase Goal:** "The mzPeak reader exposes the per-spectrum and array metadata RDR-19 needs to map a lossless experiment."
**Verified:** 2026-06-14
**Status:** PASSED
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | RDR-10b: per-spectrum parameters (flat CvParam list), spectrum_type, observed-mz range, and data_processing_ref are readable and match ground truth on a fixture | VERIFIED | `spectrum_metadata.h:253-271`, `metadata_model.cpp:373-381`, 7 test cases in `spectrum_metadata_test.cpp` pass (`spectrum_type_ms1`, `lowest_observed_mz_index0`, `highest_observed_mz_index0`, `data_processing_ref_empty_all_rows`, `parameters_empty_in_small`, `parameters_populated_in_has_uv`, `parameters_string_arm_unmangled_in_has_uv`); meson test 30/30 OK |
| 2 | RDR-10c: precursor / selected-ion / isolation-window / activation columns read correctly from small.mzpeak MS2 spectra, matching pyarrow/Rust within tolerance; ion-mobility decode is a documented stub (null in every bundled fixture) | VERIFIED | `metadata_model.cpp:561-688` (PASS 2-4); 11 test cases cover isolation window (±1e-3), activation MS:1000133+MS:1000045 value "35" (canonical), selected-ion m/z 810.789428±1e-6 and intensity ±1e-3, multi-row sweep (34 MS2/14 MS1), H1 join (source_index VALUE), H2 attach (precursor_index linear search, no back()); IM fields documented as deferred in `spectrum_metadata.h:61-66`; scan_parameters and scan_windows also verified |
| 3 | RDR-9b: auxiliary_arrays accessible as named, typed arrays, structurally validated (empty-list round-trip + schema parse) vs pyarrow; value-level validation deferred to a populated fixture | VERIFIED | `AuxiliaryArray` struct at `spectrum_metadata.h:144-195`; structural decode at `metadata_model.cpp:384-554`; count-consistency assert (`number_of_auxiliary_arrays == auxiliary_arrays.size()`, throws `ParquetError` on mismatch); `values_decoded` flag distinguishes decoded-empty from undecoded; Part B raw-byte decode fixture-gated with byte-length guard and "UNVERIFIED" comment; 2 structural test cases pass on both bundled fixtures |

**Score: 3/3 truths verified**

---

## Cross-AI Review Fixes Verified

### H1 — Source-index VALUE key-map join (not positional)

**Status: VERIFIED**

`metadata_model.cpp:575-580` — in PASS 2, for every precursor facet row `r`, `opt_int<uint64_t>(prec, "source_index", r)` reads the VALUE of that row's `source_index` column, then `out.find(*si)` looks up in the map keyed by `spectrum.index` VALUE. The chunk-local `r` is never used as a spectrum index. Same pattern appears in PASS 3 (line 643) and PASS 4 (line 705). Test `multirow_sweep_ms2_carry_precursor_ms1_carry_none` asserts EXACTLY 34 MS2 spectra carry a precursor and 14 MS1 carry none across the entire small.mzpeak file, confirming the join is correct across all chunks.

### H2 — Selected-ion attached by (source_index, precursor_index), not precursors.back()

**Status: VERIFIED**

`metadata_model.cpp:667-684` — PASS 3 explicitly searches `it->second.precursors` with a linear scan (`p.precursor_index == pi_idx`). If not found, a holder `PrecursorInfo` is created for that `precursor_index`; the ion is pushed onto `target->selected_ions`. The string `precursors.back()` does not appear anywhere in `metadata_model.cpp`. Test `h2_selected_ion_attach_by_precursor_index` spot-checks spectra indices 2 and 3; both carry `precursor_index == 1` and exactly one selected ion each with correct m/z values.

### M1 — RDR-9b scope discipline (structural validated; raw-byte gated)

**Status: VERIFIED**

`metadata_model.cpp:439-535` — Part B is wrapped in a comment block labeled "FIXTURE-GATED FOLLOW-UP — no bundled fixture carries aux bytes ... so this VALUE decode is UNVERIFIED against ground truth". The `values_decoded` flag is set to `true` only when bytes are successfully decoded. A mandatory byte-length guard at line 487 (`byte_count % element_size != 0`) prevents reinterpretation of misaligned buffers. On all bundled fixtures, `number_of_auxiliary_arrays == 0` so the item body never executes; the structural path (field access + empty-list) is exercised and validated.

---

## Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `include/mzpeak/spectrum_metadata.h` | Extended SpectrumMetadata with RDR-10b/10c/9b fields | VERIFIED | Contains `spectrum_type`, `lowest_observed_mz`, `highest_observed_mz`, `data_processing_ref`, `parameters` (RDR-10b); `AuxiliaryArray` + `auxiliary_arrays` (RDR-9b); `PrecursorInfo`, `SelectedIonInfo`, `IsolationWindow`, `ScanWindow`, `precursors`, `scan_parameters`, `scan_windows` (RDR-10c); 307 lines |
| `src/util/metadata_model.cpp` | 4-pass read: spectrum (PASS 1) + precursor/selected_ion/scan (PASS 2-4); helpers `extract_one_cv_param`, `read_cv_params_from_list`, `format_double_canonical` | VERIFIED | Single `ReadTable` call at line 334; all four passes in-memory; helpers in anonymous namespace (lines 27-271); no second Parquet read |
| `test/spectrum_metadata_test.cpp` | Ground-truth assertions for all three requirements; multi-row sweep | VERIFIED | 20 `BOOST_AUTO_TEST_CASE` functions covering RDR-10b (7 cases), RDR-10c (11 cases), RDR-9b (2 cases); all pass |
| `meson.build` | Registers `spectrum_metadata` test with 120s timeout | VERIFIED | `spectrum_metadata` registered as a dedicated `test()` call with `timeout: 120` (not in the generic `test_names` loop) |

---

## Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `src/util/metadata_model.cpp` | `include/mzpeak/run_metadata.h` | CvParam reuse | VERIFIED | `CvParam` used in `extract_one_cv_param`, `read_cv_params_from_list`, and throughout all struct definitions |
| `test/spectrum_metadata_test.cpp` | `test/files/small.mzpeak` | `MzPeak::open("../test/files/small.mzpeak")` | VERIFIED | Present in every test case; fixture opened correctly |
| `src/util/metadata_model.cpp` | `spectra_metadata.parquet precursor/selected_ion columns` | `GetColumnByName + source_index VALUE join + (source_index, precursor_index) attach` | VERIFIED | `precursor_index` searched linearly in PASS 3 |
| `src/util/metadata_model.cpp` | `read_cv_params_from_list` (plan 01 helper) | `activation.parameters / scan.parameters decode` | VERIFIED | `read_cv_params_from_list` called in PASS 2 (activation), PASS 4 (scan), and in AuxiliaryArray item body |

---

## Data-Flow Trace (Level 4)

| Artifact | Data Variable | Source | Produces Real Data | Status |
|----------|---------------|--------|-------------------|--------|
| `test/spectrum_metadata_test.cpp` | `m0.spectrum_type`, `m0.lowest_observed_mz`, etc. | `read_spectra_metadata` → `metadata_model.cpp:373-381` → Arrow Parquet `spectrum` struct column | Yes — Arrow reads Parquet; `get_string`/`opt_double` extract per-row values from StructArray | FLOWING |
| `precursor_activation_parameters_index2` test | `activation_parameters` CvParam list | PASS 2 → `read_cv_params_from_list(act_struct, "parameters", r)` → `extract_one_cv_param` | Yes — Arrow `LargeListArray` iteration; value union decoded; format_double_canonical for float arm | FLOWING |
| `auxiliary_arrays_empty_all_spectra_small` test | `auxiliary_arrays` | PASS 1 → `LargeListArray` field access → item body never executes (0 items) | Yes — structural path (field access + length=0 guard) is exercised even if item body does not run | FLOWING |

---

## Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| Full meson test suite including spectrum_metadata | `meson test -C build` | 30/30 OK; spectrum_metadata ran in 44.95s (within 120s timeout) | PASS |
| e2e pipeline | `scripts/e2e.sh` | T1/T3 PASS, E2E PASS, T2 PASS, T5 PASS | PASS |
| clang-format conformance on all 4 edited C++ files | `clang-format --style=file f \| diff f -` | Empty diff on all four files | PASS |

---

## Probe Execution

No explicit probe scripts declared for this phase. `scripts/e2e.sh` serves as the integration gate and passed (see Behavioral Spot-Checks above).

---

## Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|---------|
| RDR-10b | 01-01 | Per-spectrum parameters (CvParam list), spectrum_type, observed-mz range, data_processing_ref readable; match ground truth on fixture | SATISFIED | `spectrum_type_ms1`, `lowest_observed_mz_index0`, `highest_observed_mz_index0`, `data_processing_ref_empty_all_rows`, `parameters_populated_in_has_uv` all pass with pyarrow ground-truth values |
| RDR-10c | 01-02 | Precursor / selected-ion / isolation-window / activation columns read correctly; ion-mobility a documented stub | SATISFIED | 11 test cases covering isolation window, CID+collision-energy activation, selected-ion m/z+intensity, 34 MS2/14 MS1 multi-row sweep, H1+H2 fix verification; IM fields declared as `nullopt` stubs with Doxygen |
| RDR-9b | 01-03 | auxiliary_arrays accessible, structurally validated; value-level decode deferred | SATISFIED | `AuxiliaryArray` struct with `values_decoded` flag; structural decode + count-consistency assert validated on both fixtures; raw-byte decode fixture-gated with byte-length guard and "UNVERIFIED" comment |

---

## Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| (none found) | — | — | — | — |

No TBD/FIXME/XXX markers. No stub return-null or hardcoded empty data. No unresolved debt. The ion-mobility stub (`nullopt` on `ion_mobility_value`/`ion_mobility_type`) and the RDR-9b raw-byte VALUE decode are both intentional, fully documented deferred items with zero impact on the phase goal — they are not empty stubs but explicitly-scoped follow-ups with `values_decoded` discriminating the undecoded case.

---

## Human Verification Required

None. All success criteria are verifiable programmatically. The test suite provides ground-truth assertions against pyarrow-verified values. The e2e pipeline confirms no regressions.

---

## Gaps Summary

No gaps. All three success criteria are fully verified in the codebase. The phase goal — "the mzPeak reader exposes the per-spectrum and array metadata RDR-19 needs to map a lossless experiment" — is achieved:

- RDR-10b (spectrum scalar fields + per-spectrum CvParam list): VERIFIED against small.mzpeak and has_uv.mzpeak with pyarrow ground truth.
- RDR-10c (precursor/isolation/activation/selected-ion join + scan add-on): VERIFIED with 11 test cases including the critical multi-row sweep (34 MS2/14 MS1) and both cross-AI review fixes (H1 source_index VALUE join, H2 precursor_index attach).
- RDR-9b (auxiliary_arrays structural decode + values_decoded flag + fixture-gated raw-byte decode): VERIFIED structurally; value-level deferral is documented and intentional per CONTEXT.md.

---

_Verified: 2026-06-14T12:00:00Z_
_Verifier: Claude (gsd-verifier)_
