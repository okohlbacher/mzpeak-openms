# Phase 1: Reader Lossless-Map Prerequisites — Validation Architecture

**Extracted from:** 01-RESEARCH.md `## Validation Architecture` (standalone file for the Nyquist Dimension-8 check).
**Phase requirements:** RDR-10b, RDR-10c, RDR-9b.

This is the authoritative test map / Wave-0 scaffold spec for Phase 1. The same
content lives inline in 01-RESEARCH.md; this standalone copy is what the Nyquist
Dimension-8 verification expects.

---

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Boost.Test (header-only, `BOOST_AUTO_TEST_CASE`) |
| Config file | `meson.build` — tests registered as `test(name, executable(...))` |
| Quick run command | `cd build && meson test -t 60 spectrum_metadata` (single suite) |
| Full suite command | `cd build && meson test && scripts/e2e.sh` |

### Phase Requirements → Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| RDR-10b | `spectrum_type` == `"MS:1000579"` for row 0 of small.mzpeak | unit | `meson test spectrum_metadata_test` | ❌ Wave 0 |
| RDR-10b | `lowest_observed_mz` within 1e-9 of pyarrow value for rows 0,1 | unit | `meson test spectrum_metadata_test` | ❌ Wave 0 |
| RDR-10b | `highest_observed_mz` within 1e-9 of pyarrow value | unit | `meson test spectrum_metadata_test` | ❌ Wave 0 |
| RDR-10b | `data_processing_ref` empty/null for all rows in small.mzpeak | unit | `meson test spectrum_metadata_test` | ❌ Wave 0 |
| RDR-10b | `parameters` list populated in has_uv.mzpeak (row 0: `MS:1000796`, spectrum title) | unit | `meson test spectrum_metadata_test` | ❌ Wave 0 |
| RDR-10b | `parameters` list empty (not null, not crashing) in small.mzpeak | unit | `meson test spectrum_metadata_test` | ❌ Wave 0 |
| RDR-10c | `precursor.isolation_window.target_mz` within 1e-3 of 810.789 for spectrum index 2 | unit | `meson test spectrum_metadata_test` | ❌ Wave 0 |
| RDR-10c | `activation.parameters` contains `MS:1000133` (CID) for a MS2 spectrum | unit | `meson test spectrum_metadata_test` | ❌ Wave 0 |
| RDR-10c | `selected_ion.mz` within 1e-6 of 810.789428 for spectrum index 2 | unit | `meson test spectrum_metadata_test` | ❌ Wave 0 |
| RDR-10c | MS1 spectra have no precursor (optional absent / null source_index) | unit | `meson test spectrum_metadata_test` | ❌ Wave 0 |
| RDR-9b | `auxiliary_arrays` reads as empty list (not crash) for all rows in all bundled fixtures | unit | `meson test spectrum_metadata_test` | ❌ Wave 0 |
| All | `meson test` suite stays green (regression) | regression | `meson test` | ✓ existing |
| All | `scripts/e2e.sh` stays green | e2e | `scripts/e2e.sh` | ✓ existing |

### Sampling Rate

- **Per task commit:** `cd build && meson test -t 60 spectrum_metadata_test` (new test only)
- **Per wave merge:** `cd build && meson test` (all unit tests)
- **Phase gate:** `cd build && meson test && scripts/e2e.sh` (full suite including e2e)

### Wave 0 Gaps

- [ ] `test/spectrum_metadata_test.cpp` — covers RDR-10b / RDR-10c / RDR-9b assertions listed above
- [ ] Register in `meson.build` under the test block (pattern: `test('spectrum_metadata_test', executable('spectrum_metadata_test', 'test/spectrum_metadata_test.cpp', dependencies: [mzpeak_dep]))`)

---
