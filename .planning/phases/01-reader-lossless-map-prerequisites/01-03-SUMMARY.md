---
phase: 01-reader-lossless-map-prerequisites
plan: "03"
subsystem: reader/metadata
tags: [rdr-9b, auxiliary-arrays, arrow, parquet, structural-decode, fixture-gated,
  boost-test, phase-gate, clang-format]
dependency_graph:
  requires:
    - extract_one_cv_param (plan 01-01)
    - read_cv_params_from_list (plan 01-01)
    - opt_int/opt_double/opt_float/opt_string/get_string (plan 01-01)
    - SpectrumMetadata (plans 01-01 and 01-02 base struct)
    - AuxiliaryArray struct (this plan Task 1)
  provides:
    - AuxiliaryArray struct (CvParam name, data_type opaque string, compression,
      unit, parameters, data_processing_ref, values, values_decoded flag)
    - auxiliary_arrays vector on SpectrumMetadata (empty in all bundled fixtures)
    - structural decode validated: schema parse + empty-list + count-consistency
      assert (number_of_auxiliary_arrays == auxiliary_arrays.size())
    - gated raw-byte VALUE decode (fixture-gated follow-up, byte-length-guarded,
      values_decoded-flagged, documented as unverified)
  affects:
    - include/mzpeak/spectrum_metadata.h
    - src/util/metadata_model.cpp
    - test/spectrum_metadata_test.cpp
tech_stack:
  added: []
  patterns:
    - Decoded-vs-undecoded API contract: values_decoded discriminator flag;
      values_decoded==true&&values.empty() = legitimately empty decoded array;
      values_decoded==false = fixture-gated unverified or byte-length guard reject
    - Structural decode: schema field access + empty-list path exercised on bundled
      fixtures even though item body never executes (0 aux arrays in all fixtures)
    - Count-consistency assert: throw ParquetError on
      number_of_auxiliary_arrays != auxiliary_arrays.size() (T-03-02 corruption)
    - extract_one_cv_param reuse: aux array name decoded via plan-01 helper
      (lone CvParam struct, not a list)
    - Byte-length guard: data.size() % element_size == 0 before any reinterpret;
      misaligned buffer leaves values empty and values_decoded=false (T-03-01)
    - Opaque data_type: switch on lowercase string "float32"/"int32"/"float64";
      never routed through PSI::DataType enum (Pitfall 5)
    - std::memcpy for byte reinterpretation (never direct pointer cast)
key_files:
  created: []
  modified:
    - include/mzpeak/spectrum_metadata.h
    - src/util/metadata_model.cpp
    - test/spectrum_metadata_test.cpp
decisions:
  - DEC-01-03-structural-vs-gated-split: per CONTEXT.md "do not ship unvalidated
    decode paths" and cross-AI review (M1), the work is split: Part A (structural
    schema parse + empty-list + count assert) is implemented and VALIDATED on
    bundled fixtures; Part B (raw-byte VALUE decode) is fixture-gated, marked
    UNVERIFIED in comments, and guarded by values_decoded; value-level correctness
    deferred until a fixture with populated aux data is available.
  - DEC-01-03-data-type-opaque: data_type treated as opaque lowercase Arrow dtype
    string (Pitfall 5) — never routed through any PSI enum; switch on string
    literals "float32"/"int32"/"float64" in Part B.
  - DEC-01-03-memcpy-reinterpret: byte reinterpretation via std::memcpy into a
    local array (not direct pointer cast or reinterpret_cast) for strict-aliasing
    safety; byte-length guard ensures buffer alignment before any memcpy.
metrics:
  duration_minutes: 25
  completed_date: "2026-06-14"
  tasks_completed: 3
  tasks_total: 3
  files_modified: 3
---

# Phase 1 Plan 03: RDR-9b Auxiliary Arrays Accessor Summary

**One-liner:** AuxiliaryArray struct with decoded-vs-undecoded contract, structural
decode (schema parse + empty-list + count-consistency assert) validated on all
bundled fixtures, and fixture-gated raw-byte VALUE decode (byte-length-guarded,
values_decoded-flagged, documented as unverified), closing Phase 1.

## What Was Built

### Task 1: AuxiliaryArray struct + accessor (30c546f)

Added to `include/mzpeak/spectrum_metadata.h`:

- `AuxiliaryArray` struct with Doxygen and snake_case fields:
  - `CvParam name` — CV term identifying the array (decoded via
    `extract_one_cv_param`, reusing the plan-01 helper).
  - `std::string data_type` — opaque lowercase Arrow dtype string (e.g.
    "float32"); NOT routed through any PSI enum (Pitfall 5).
  - `std::string compression`, `unit`, `data_processing_ref`.
  - `std::vector<CvParam> parameters`.
  - `std::vector<float> values` — decoded values (empty when source data is
    empty OR when VALUE decode was not performed / byte-length guard rejected).
  - `bool values_decoded = false` — discriminator: `true` only when the raw
    `data` buffer was actually decoded into `values` (including the
    legitimately empty case); `false` when the fixture-gated path was not taken
    or a misaligned buffer was rejected.
- Full Doxygen on `values` and `values_decoded` documenting the
  decoded-vs-undecoded API contract: callers MUST check `values_decoded`.
- Added `std::vector<AuxiliaryArray> auxiliary_arrays` to `SpectrumMetadata`
  with Doxygen: "Empty in all bundled fixtures (number_of_auxiliary_arrays==0);
  schema parsing + empty-list handling are validated structurally; raw-byte
  VALUE decode is fixture-gated follow-up (see values_decoded). (RDR-9b)".

### Task 2: Structural decode + gated VALUE decode (01be7cf)

Extended `read_spectra_metadata` in `src/util/metadata_model.cpp`:

**Added includes:** `<cstdint>`, `<cstring>` (for `std::memcpy`).

**PART A — Structural decode (VALIDATED):**  In PASS 1 per-row loop, reads
`spectrum.auxiliary_arrays` (large_list<struct>):
- GetFieldByName("auxiliary_arrays") → if absent/NULL, leaves empty.
- Casts to `arrow::LargeListArray`; iterates `[value_offset(r), value_length(r))`.
- Each item struct → builds an `AuxiliaryArray`:
  - `name` via `extract_one_cv_param(*name_struct, begin + k)` (plan-01 helper
    reused; not re-inlined).
  - `data_type`, `compression`, `unit`, `data_processing_ref` via `get_string`.
  - `parameters` via `read_cv_params_from_list(aux_items, "parameters", begin+k)`.
- Count-consistency assert: reads `number_of_auxiliary_arrays` (uint32) via
  `opt_int<uint64_t>`; throws `ParquetError` if declared count !=
  `m.auxiliary_arrays.size()` (T-03-02 corruption signal).
- On all bundled fixtures: every row has 0 aux arrays, so the item body never
  executes — but the schema field access + empty-list path IS exercised.

**PART B — Raw-byte VALUE decode (FIXTURE-GATED, NOT validated):**  In the
item body, after structural fields, reads `data` (large_list<uint8>):
- Zero bytes → `values.clear(); values_decoded = true` (legitimately empty).
- Non-zero bytes: switch on `data_type` string (Pitfall 5, not PSI enum):
  "float32"/"int32" → element_size=4; "float64" → element_size=8; unknown →
  `values_decoded=false`, decode deferred.
- Byte-length guard (T-03-01): `data.size() % element_size == 0` required;
  on mismatch → `values empty, values_decoded=false`, no reinterpret.
- When guard passes: `std::memcpy` into a local buffer for each element
  (strict-aliasing safe); converts to float; `values_decoded=true`.
- Wrapped in a clearly-marked comment: "FIXTURE-GATED FOLLOW-UP — no bundled
  fixture carries aux bytes (number_of_auxiliary_arrays==0 everywhere), so this
  VALUE decode is UNVERIFIED against ground truth".

### Task 3: Phase gate (c315282)

**clang-format -i:** Applied to all four C++ files edited across all three plans
of Phase 1:
- `include/mzpeak/spectrum_metadata.h` — clean (no drift).
- `src/util/metadata_model.cpp` — minor spacing normalised by formatter.
- `include/mzpeak/util/metadata_model.h` — clean (no drift).
- `test/spectrum_metadata_test.cpp` — minor spacing normalised.

**Diff-verify:** `clang-format --style=file f | diff f -` empty on all four files.

**Editorconfig:** No trailing whitespace, final newline present, LF line endings
on all files.

**Full meson test suite:** 30/30 OK. `spectrum_metadata` test now has 20 cases
(18 from plan 01-02 + 2 new RDR-9b structural cases), runtime ~43s (within the
120s timeout set in plan 01-02).

**scripts/e2e.sh:** T1/T3 PASS, E2E PASS, T2 PASS, T5 PASS.

### Tests (2 new cases)

- `auxiliary_arrays_empty_all_spectra_small`: iterates all spectra in
  small.mzpeak; asserts `auxiliary_arrays.empty()` on each with no crash; if
  the count-consistency assert fires, `read_spectra_metadata` throws
  ParquetError and the test fails — no explicit extra check needed.
- `auxiliary_arrays_empty_has_uv`: same check on has_uv.mzpeak (cross-fixture
  structural validation).

Both test cases include a comment documenting that VALUE-level aux assertions
are deferred pending a populated fixture, and that `values_decoded` is the
decoded-vs-undecoded discriminator.

## Deviations from Plan

None — plan executed exactly as written. The structural-vs-gated split, the
count-consistency assert, the byte-length guard, the `values_decoded` flag, the
opaque data_type handling, and the `extract_one_cv_param` reuse were all
specified in the plan and implemented as specified.

## Known Stubs

- `AuxiliaryArray::values` and `values_decoded`: the raw-byte VALUE decode
  (Part B) is implemented but UNVERIFIED — no bundled fixture carries aux bytes.
  `values_decoded` is always `false` on any data returned from bundled fixtures
  (because no aux bytes exist to decode).  This is intentional per the
  structural-vs-gated split; the stub is documented in both the header and the
  test file, and `values_decoded` lets Phase 2 detect it at runtime.
  Future work: obtain or construct a fixture with populated auxiliary_arrays
  and add VALUE-level assertions.

## Threat Flags

No new network endpoints, auth paths, or schema changes at trust boundaries
beyond the already-present Parquet file trust boundary.

Threat mitigations implemented as planned:
- T-03-01: opaque data_type switch + mandatory `data.size() % element_size == 0`
  byte-length guard + `std::memcpy` (no raw pointer cast); misaligned buffer
  leaves values empty and `values_decoded=false`.
- T-03-02: `number_of_auxiliary_arrays == auxiliary_arrays.size()` assert;
  throws ParquetError on mismatch (corruption signal).
- T-03-04: VALUE decode is fixture-gated, documented "UNVERIFIED" in comments,
  and flagged via `values_decoded`; only the structural path is shipped as
  validated (CONTEXT.md honored).

## Self-Check: PASSED

Files present:
- include/mzpeak/spectrum_metadata.h FOUND
- src/util/metadata_model.cpp FOUND
- test/spectrum_metadata_test.cpp FOUND
- .planning/phases/01-reader-lossless-map-prerequisites/01-03-SUMMARY.md (this file)

Commits present:
- 30c546f feat(01-03): define AuxiliaryArray struct + auxiliary_arrays accessor (RDR-9b)
- 01be7cf feat(01-03): structural aux decode (validated) + gated raw-byte VALUE decode (RDR-9b)
- c315282 style(01-03): clang-format conformance + phase gate (all files clean, 30/30 green, e2e PASS)

Full test suite: 30/30 OK (spectrum_metadata 20/20 including 2 new RDR-9b structural
tests; e2e PASS).
