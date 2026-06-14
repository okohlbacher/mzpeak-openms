# Phase 1: Reader Lossless-Map Prerequisites - Research

**Researched:** 2026-06-14
**Domain:** mzPeak C++ reader — per-spectrum metadata extension (`SpectrumMetadata`, `read_spectra_metadata`, auxiliary_arrays, precursor/isolation/activation)
**Confidence:** HIGH (all findings verified directly against fixture Parquet files and source code)

---

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

- Extend `Util::read_spectra_metadata` (`src/util/metadata_model.cpp`) and `SpectrumMetadata` (`include/mzpeak/spectrum_metadata.h`) to expose per-spectrum `parameters` (flat `CvParam` list), `spectrum_type` (`MS:1000559`), lowest/highest observed m/z, and `data_processing_ref`.
- Reuse the existing `CvParam` model from `RunMetadata` (`include/mzpeak/run_metadata.h`) — same `{accession, name, value, unit}` shape — for the per-spectrum parameter list. Do not invent a second param shape.
- Verify column names against the real `spectra_metadata.parquet` schema (pyarrow) before coding.
- Add a typed accessor for `auxiliary_arrays` (name/CV term + decoded float values) so Phase 2 can map them to `MSSpectrum::FloatDataArrays`.
- Mirror the Rust reference's `SpectrumSource` precursor fields for RDR-10c.
- RDR-10c: prefer deferral (option b) unless a fixture is cheaply obtainable; do not write unvalidated decode paths against no ground truth.

### Claude's Discretion

- Exact accessor signatures/names, struct layout, and whether `parameters` is a `std::vector<CvParam>` member on `SpectrumMetadata` vs a parallel map.
- Test granularity, as long as every new field is asserted against pyarrow ground truth.

### Deferred Ideas (OUT OF SCOPE)

- Any OpenMS-side wiring (Phase 2).
- RDR-10c precursor/IM decode if no MSn+IM fixture available.

</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| RDR-10b | Extend `read_spectra_metadata` + `SpectrumMetadata` to expose per-spectrum `parameters` (flat `CvParam` list), `spectrum_type` (`MS:1000559`), lowest/highest observed m/z, and `data_processing_ref` | Column names and Arrow types verified via pyarrow on real fixtures; CvParam struct shape confirmed; extension pattern documented |
| RDR-10c | Precursor / selected-ion / isolation-window / activation / ion-mobility exposed, matching Rust reader fields | **Verdict: PARTIALLY UNBLOCKED** — small.mzpeak carries full precursor/isolation/activation/selected_ion data (34 MS2 spectra); ion mobility is null in all bundled fixtures. Plan should implement precursor+isolation+activation+selected_ion from existing data; IM decode deferred within this phase. |
| RDR-9b | Typed accessor for `auxiliary_arrays`, mapping to `FloatDataArrays` | `auxiliary_arrays` schema fully verified; no fixture currently has populated rows — RDR-9b is schema-observable but data-empty in all bundled files. |

</phase_requirements>

---

## Summary

Phase 1 extends the mzPeak C++ reader with three categories of per-spectrum metadata that the lossless mzPeak→MSExperiment map (Phase 2) needs. All research was performed against the real fixture files and live source code; no training-data assumptions were required for the core findings.

**Critical re-scope finding for RDR-10c:** The CONTEXT.md assumed small.mzpeak was MS1-only and had no precursor data. The fixture is **48 spectra — 14 MS1 + 34 MS2** (verified via pyarrow on `spectra_metadata.parquet`). The `precursor`, `selected_ion`, `scan`, and `activation` top-level columns in `spectra_metadata.parquet` are fully populated with real CID precursor data. Ion mobility (`ion_mobility_value`, `ion_mobility_type`) is null in all rows of every bundled fixture. This means RDR-10c is **substantially unblocked for precursor/isolation/activation/selected-ion**; only the IM subcase needs a new fixture. The planner should implement the full precursor path against small.mzpeak and gate only IM decode as a follow-up.

**Critical finding for RDR-9b:** The `auxiliary_arrays` column exists and has a rich schema in every bundled fixture, but `number_of_auxiliary_arrays` is 0 for every row in every bundled file. The accessor can be written and the round-trip path validated structurally, but value-level ground-truth testing is not possible against bundled fixtures. The planner must decide: write the decoder and validate structurally (schema + empty-list round-trip), or gate behind a fixture with actual aux data.

**CvParam value shape (key for RDR-10b):** The per-spectrum `parameters` column stores each param as `struct<value: struct<integer: int64, float: double, string: large_string, boolean: bool>, accession: string, name: large_string, unit: string>`. The `value` field is a 4-field union (only one arm populated). This differs from the `CvParam` in `run_metadata.h` which stores `value` as a single `optional<string>` (the stringified form). The planner must decide how to handle the richer value union when populating the reused `CvParam` struct — most likely: serialize the non-null arm to string, matching the RunMetadata convention.

**Primary recommendation:** Implement RDR-10b fully (all four fields from `spectrum` struct), RDR-10c precursor+isolation+activation+selected_ion (skipping IM), and RDR-9b as a structural decoder (no value ground-truth available). Three distinct tasks.

---

## Architectural Responsibility Map

| Capability | Primary Tier | Secondary Tier | Rationale |
|------------|-------------|----------------|-----------|
| Per-spectrum scalar fields (RDR-10b) | Reader / metadata layer (`metadata_model.cpp`) | `SpectrumMetadata` struct (header) | Already the site where scalars like ms_level/polarity/retention_time are read; extend the same pattern |
| Per-spectrum CvParam list (RDR-10b) | Reader / metadata layer (`metadata_model.cpp`) | `SpectrumMetadata` struct + `CvParam` from `run_metadata.h` | Same Arrow struct-reading pattern; reuse existing Arrow helper functions |
| Precursor / isolation / activation / selected-ion (RDR-10c) | New metadata facets on `SpectrumMetadata` or companion structs | `metadata_model.cpp` (`read_spectra_metadata`) | Precursor is a separate top-level column in the Parquet schema; must join by `source_index` |
| Auxiliary arrays accessor (RDR-9b) | `metadata_model.cpp` or new `read_auxiliary_arrays` | `SpectrumMetadata` (carries raw aux list) or `Spectrum` accessor | `auxiliary_arrays` is a `large_list` inside the `spectrum` struct; decoded inline during metadata read |
| Test validation | Boost.Test unit tests + pyarrow ground truth | `meson test` + `scripts/e2e.sh` | Existing test pattern; every new field asserted against pyarrow |

---

## Standard Stack

### Core (all already in use — no new dependencies)

| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Apache Arrow C++ | (project dependency) | Read `spectra_metadata.parquet` struct/list/string columns | All existing Arrow reads use `arrow::StructArray`, `arrow::LargeListArray`, `arrow::StringArray` |
| Boost.Test | (project dependency) | Unit tests | All existing tests use `BOOST_AUTO_TEST_CASE` / `BOOST_TEST` |
| pyarrow | (system python3) | Ground-truth validation oracle | Established pattern for all fixture-value assertions |

No new packages needed. No package legitimacy audit required.

---

## Verified Schema: `spectra_metadata.parquet`

**Source:** pyarrow inspection of `test/files/small.mzpeak`, `test/files/has_uv.mzpeak`, `test/files/small.chunked.mzpeak`, `test/files/small.numpress.mzpeak` [VERIFIED: pyarrow on real fixtures]

The table has **four top-level struct columns**: `spectrum`, `scan`, `precursor`, `selected_ion`.

### `spectrum` struct — child fields

| Child name (exact) | Arrow type | Populated? | Notes |
|---|---|---|---|
| `index` | `uint64` | yes, always | Row key |
| `id` | `large_string` | yes | Native spectrum id |
| `MS_1000511_ms_level` | `uint8` | yes | Already read |
| `time` | `double` | yes | Already read |
| `MS_1000465_scan_polarity` | `int8` | yes | Already read |
| `MS_1000525_spectrum_representation` | `string` | yes | Already read |
| `MS_1000559_spectrum_type` | `string` | **yes** — `'MS:1000579'` (MS1) or `'MS:1000580'` (MS2) | **RDR-10b target; NOT yet read** |
| `MS_1000528_lowest_observed_mz_unit_MS_1000040` | `double` | **yes** — e.g. `200.00018816645024` | **RDR-10b target; NOT yet read** |
| `MS_1000527_highest_observed_mz_unit_MS_1000040` | `double` | **yes** — e.g. `1999.9857293095915` | **RDR-10b target; NOT yet read** |
| `MS_1003060_number_of_data_points` | `uint64` | yes | Already read |
| `MS_1003059_number_of_peaks` | `uint64` | yes | Already read |
| `MS_1000504_base_peak_mz_unit_MS_1000040` | `double` | yes | Already read |
| `MS_1000505_base_peak_intensity_unit_MS_1000131` | `float` | yes | Already read |
| `MS_1000285_total_ion_current_unit_MS_1000131` | `float` | yes | Already read |
| `data_processing_ref` | `large_string` | **null in all rows of all bundled fixtures** | **RDR-10b target; always null in bundled data** |
| `parameters` | `large_list<struct<value: struct<integer:int64, float:double, string:large_string, boolean:bool>, accession:string, name:large_string, unit:string>>` | **sparse**: 0/48 rows in small.mzpeak; 212/212 rows in has_uv.mzpeak (1 param each, `MS:1000796` spectrum title) | **RDR-10b target; populated in has_uv** |
| `auxiliary_arrays` | `large_list<struct<data:large_list<uint8>, name:struct<...CvParam...>, data_type:string, compression:string, unit:string, parameters:large_list<...CvParam...>, data_processing_ref:large_string>>` | **0 rows have aux_arrays in ANY bundled fixture** | **RDR-9b target; schema present, data empty** |
| `number_of_auxiliary_arrays` | `uint32` | 0 in all rows of all fixtures | Gating counter for aux array decode |
| `mz_delta_model` | `large_list<double>` | yes (already used by null_fill) | Not a Phase 1 target |

### `scan` struct — child fields (relevant subset)

| Child name (exact) | Arrow type | Populated? |
|---|---|---|
| `source_index` | `uint64` | yes (join key to spectrum) |
| `scan_index` | `uint64` | yes |
| `MS_1000016_scan_start_time_unit_UO_0000031` | `float` | yes |
| `MS_1000512_filter_string` | `large_string` | yes |
| `MS_1000927_ion_injection_time_unit_UO_0000028` | `float` | yes |
| `ion_mobility_value` | `double` | **null in ALL rows of ALL fixtures** |
| `ion_mobility_type` | `string` | **null in ALL rows of ALL fixtures** |
| `instrument_configuration_ref` | `uint32` | yes |
| `parameters` | `large_list<CvParam>` | yes (e.g. `MS:1000800` mass resolving power) |
| `scan_windows` | `large_list<struct<MS_1000501_scan_window_lower_limit..., MS_1000500_scan_window_upper_limit..., parameters>>` | yes |

### `precursor` struct — child fields

| Child name (exact) | Arrow type | Populated? |
|---|---|---|
| `source_index` | `uint64` | **yes — 34/48 rows populated** (join key to spectrum.index) |
| `precursor_index` | `uint64` | yes |
| `precursor_id` | `large_string` | yes |
| `isolation_window.MS_1000827_isolation_window_target_mz` | `float` | **yes** — e.g. `810.789` |
| `isolation_window.MS_1000828_isolation_window_lower_offset` | `float` | **yes** — e.g. `1.0` |
| `isolation_window.MS_1000829_isolation_window_upper_offset` | `float` | **yes** — e.g. `1.0` |
| `isolation_window.parameters` | `large_list<CvParam>` | yes (empty in bundled data) |
| `activation.parameters` | `large_list<CvParam>` | **yes** — CID (`MS:1000133`) + collision energy (`MS:1000045`, `35.0 UO:0000266`) |

### `selected_ion` struct — child fields

| Child name (exact) | Arrow type | Populated? |
|---|---|---|
| `source_index` | `uint64` | **yes — 34/48 rows** (join key) |
| `precursor_index` | `uint64` | yes |
| `MS_1000744_selected_ion_mz_unit_MS_1000040` | `double` | **yes** — e.g. `810.789428710938` |
| `MS_1000041_charge_state` | `int32` | null in bundled data |
| `MS_1000042_intensity_unit_MS_1000131` | `float` | **yes** — e.g. `1994039.125` |
| `ion_mobility_value` | `double` | **null in ALL rows** |
| `ion_mobility_type` | `string` | **null in ALL rows** |
| `parameters` | `large_list<CvParam>` | yes (empty list in bundled data) |

---

## Architecture Patterns

### System Architecture Diagram

```
spectra_metadata.parquet
  ├── spectrum struct      ──read──►  SpectrumMetadata (existing + extensions)
  │     ├── [already read]: index, id, ms_level, time, polarity, representation,
  │     │    number_of_data_points, number_of_peaks, base_peak_mz/int, TIC
  │     ├── [RDR-10b NEW]: MS_1000559_spectrum_type → SpectrumMetadata::spectrum_type
  │     ├── [RDR-10b NEW]: MS_1000528_lowest_observed_mz → SpectrumMetadata::lowest_observed_mz
  │     ├── [RDR-10b NEW]: MS_1000527_highest_observed_mz → SpectrumMetadata::highest_observed_mz
  │     ├── [RDR-10b NEW]: data_processing_ref → SpectrumMetadata::data_processing_ref
  │     ├── [RDR-10b NEW]: parameters (large_list<CvParam>) → SpectrumMetadata::parameters
  │     └── [RDR-9b NEW]:  auxiliary_arrays (large_list<AuxArray>) → SpectrumMetadata::auxiliary_arrays
  ├── precursor struct     ──join by source_index──►  SpectrumMetadata::precursor (NEW, RDR-10c)
  │     ├── isolation_window (target_mz, lower_offset, upper_offset)
  │     └── activation.parameters (CvParam list)
  └── selected_ion struct  ──join by source_index──►  PrecursorInfo::selected_ions (NEW, RDR-10c)
        ├── MS_1000744_selected_ion_mz, MS_1000041_charge_state, MS_1000042_intensity
        └── [ion_mobility: null in all fixtures — decode deferred]

read_spectra_metadata(Parquet&)
  → passes spectrum struct → existing scalar reads + new fields
  → joins precursor/selected_ion by source_index → builds PrecursorInfo
  → returns map<uint64_t, SpectrumMetadata>
```

### Recommended Project Structure (changes only)

```
include/mzpeak/
├── spectrum_metadata.h     ← extend SpectrumMetadata (RDR-10b fields + PrecursorInfo)
│                              + add PrecursorInfo / AuxiliaryArray structs
└── run_metadata.h          ← no changes; CvParam reused as-is

src/util/
└── metadata_model.cpp      ← extend read_spectra_metadata to read new fields,
                               join precursor/selected_ion tables, decode aux_arrays

test/
└── spectrum_metadata_test.cpp  ← NEW: per-field assertions vs pyarrow ground truth
```

### Pattern 1: Extending `read_spectra_metadata` — scalar fields (RDR-10b)

Follows the exact pattern already used for `MS_1000511_ms_level` etc. in `metadata_model.cpp:186-200`. Use the existing `get_string`, `opt_double`, `opt_int` helpers already defined in that file.

```cpp
// In the per-row loop of read_spectra_metadata, inside the `spectrum` struct:
m.spectrum_type = get_string(spectrum, "MS_1000559_spectrum_type", r);
m.lowest_observed_mz =
    opt_double(spectrum, "MS_1000528_lowest_observed_mz_unit_MS_1000040", r);
m.highest_observed_mz =
    opt_double(spectrum, "MS_1000527_highest_observed_mz_unit_MS_1000040", r);
m.data_processing_ref = get_string(spectrum, "data_processing_ref", r);
// Returns empty string when null — consistent with existing `id`/`representation` pattern.
```

### Pattern 2: Reading `parameters` — per-spectrum CvParam list (RDR-10b)

The `parameters` child is a `large_list<struct<value:union-struct, accession:string, name:large_string, unit:string>>`. The `value` field is a 4-arm union struct (only one arm non-null). The existing `CvParam` struct stores `value` as `optional<string>`.

**Strategy:** When materializing each parameter, serialize the non-null arm of the value union to string form — matching how RunMetadata's JSON parser stores values. This keeps one `CvParam` type throughout.

```cpp
// Helper to extract one CvParam from a row of the parameters list:
static std::string extract_cv_value(const arrow::StructArray& val_struct, int64_t row)
{
    // Try each arm; return first non-null as string
    if (auto* f = val_struct.GetFieldByName("string"))
        if (!f->IsNull(row)) return ...LargeStringArray...->GetString(row);
    if (auto* f = val_struct.GetFieldByName("float"))
        if (!f->IsNull(row)) return std::to_string(...DoubleArray...->Value(row));
    if (auto* f = val_struct.GetFieldByName("integer"))
        if (!f->IsNull(row)) return std::to_string(...Int64Array...->Value(row));
    if (auto* f = val_struct.GetFieldByName("boolean"))
        if (!f->IsNull(row)) return ...BooleanArray...->Value(row) ? "true" : "false";
    return {};
}
```

Note: this is the only non-trivial new Arrow read pattern; everything else reuses the existing helper functions.

### Pattern 3: Joining precursor / selected_ion (RDR-10c)

The `precursor` and `selected_ion` columns are separate top-level columns in `spectra_metadata.parquet`, joined to `spectrum` via `source_index == spectrum.index`. The join is a simple map lookup during the same pass.

```cpp
// After reading the `spectrum` column into the output map,
// do a second pass over `precursor` and `selected_ion`:
auto prec_col = table->GetColumnByName("precursor");
// For each row r:
//   uint64 source_index = prec_arr->field("source_index")->Value(r)
//   if source_index is null → skip (MS1 spectrum has no precursor row)
//   look up out[source_index], fill out[source_index].precursor = PrecursorInfo{...}
```

This is a separate pass over the same already-read table; no second Parquet read is needed.

### Pattern 4: Decoding `auxiliary_arrays` (RDR-9b)

Each row's `auxiliary_arrays` is a `large_list` of structs. Each element carries:
- `data`: `large_list<uint8>` — raw bytes (encoding TBD by `data_type` + `compression`)
- `name`: full CvParam struct (same shape as `parameters` items)
- `data_type`: string (Arrow dtype name, e.g. `"float32"`)
- `compression`: string (e.g. `"none"`)
- `unit`: string
- `parameters`: `large_list<CvParam>` (additional CV terms)
- `data_processing_ref`: `large_string`

Since `number_of_auxiliary_arrays` is 0 in all bundled fixtures, the decoder body will be exercised only structurally (non-null path never triggered). Write the decode skeleton to match the spec; add a comment noting the fixture gap.

```cpp
struct AuxiliaryArray {
    CvParam name;               // CV term identifying the array
    std::string data_type;      // "float32", "float64", etc.
    std::string compression;    // "none", etc.
    std::string unit;
    std::vector<CvParam> parameters;
    std::string data_processing_ref;
    std::vector<float> values;  // decoded; empty if data is empty
};
```

### Anti-Patterns to Avoid

- **Re-reading the Parquet file for each column:** all four top-level columns (`spectrum`, `scan`, `precursor`, `selected_ion`) must be read in a single `ReadTable` call (already how `metadata_model.cpp` works) and processed in memory. Do not open the file twice.
- **Throwing on missing precursor column:** an MS1-only file may have a `precursor` column with all-null `source_index`; skip gracefully rather than throwing.
- **Asserting value identity on `data_processing_ref`:** it is null in all bundled fixtures; tests should assert the field is readable and is empty/nullopt, not a particular string.
- **Modeling the value union as a new type:** keep `CvParam.value` as `optional<string>` (matching RunMetadata) — serialize the union arm to string at read time.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Arrow struct field access | custom field-index map | `StructArray::GetFieldByName(name)` | Already the established idiom in `metadata_model.cpp:40-110` |
| CvParam parsing | new param model | `CvParam` from `include/mzpeak/run_metadata.h` | Reuse locked by CONTEXT.md; keeps Phase 2 dispatch uniform |
| Large-list iteration | manual offset arithmetic | `LargeListArray::value_offset(i)` / `value_length(i)` | Already used in `metadata_model.cpp:147-152` |
| Parquet open/close | custom handle management | `Util::Parquet` RAII wrapper | Existing pattern; `spectrum_column()` helper already does the `ReadTable` |

---

## RDR-10c Verdict: Partially Unblocked

**FINDING (verified):** `small.mzpeak` contains **34 MS2 spectra** with fully populated precursor data: `isolation_window` (target m/z + lower/upper offsets), `activation.parameters` (CID method + energy), and `selected_ion` (m/z + intensity). This fixture was derived from `test/files/small.mzML` which has MS2 entries (confirmed via grep).

**Ion mobility:** `ion_mobility_value` and `ion_mobility_type` are null in every row of every bundled fixture (`spectra_metadata.parquet` scan struct + selected_ion struct). Ion mobility decode cannot be ground-truth validated against bundled data.

**Plan recommendation:**
- **Ship in this phase:** precursor (target_mz, lower/upper offset), activation (CvParam list), selected_ion (mz, charge, intensity) — all fully testable against small.mzpeak with pyarrow ground truth.
- **Defer within this phase:** ion mobility decode only. No new fixture needed for the precursor path.
- **Fixture path for IM if ever needed:** the Rust `convert` example converts mzML→mzpeak; an IM-mzML (e.g. from a TIMS or Waters instrument) would generate an IM fixture. No such mzML exists in the repo; generating one is out of scope for Phase 1.

This changes the phase scope from the CONTEXT.md's "defer RDR-10c entirely" to "implement the precursor/activation/selected_ion subset of RDR-10c; defer IM-specific fields."

---

## Runtime State Inventory

Not applicable — this is a greenfield extension of the C++ reader library with no rename/migration. No runtime state is affected.

---

## Common Pitfalls

### Pitfall 1: `parameters` column name mismatch for the per-spectrum list

**What goes wrong:** Calling `spectrum->GetFieldByName("parameters")` on the `spectrum` StructArray succeeds, but the value union struct inside each list item has child fields `integer`, `float`, `string`, `boolean` (not `int64`, `double`, etc.). Accessing the wrong subfield name returns nullptr.
**Why it happens:** Arrow struct field names come verbatim from the Parquet schema; these are lowercase short names, not Arrow type names.
**How to avoid:** Always use exact child names: `"integer"`, `"float"`, `"string"`, `"boolean"`. Cast to `arrow::Int64Array`, `arrow::DoubleArray`, `arrow::LargeStringArray`, `arrow::BooleanArray` respectively.
**Warning signs:** `GetFieldByName` returning nullptr for a field you expect to exist.

### Pitfall 2: `precursor` column has null `source_index` for MS1 rows

**What goes wrong:** Trying to read `precursor_arr->field("source_index")->Value(r)` for an MS1 spectrum row will dereference a null value.
**Why it happens:** The `precursor` column exists for every row in the table but `source_index` is null when the spectrum has no precursor.
**How to avoid:** Always null-check `source_index` before reading the precursor subfields. The existing `opt_int` helper pattern applies here.
**Warning signs:** Crashes or garbage values when iterating all 48 rows of small.mzpeak's precursor column.

### Pitfall 3: `data_processing_ref` is null in all bundled fixtures

**What goes wrong:** A test that asserts a non-empty `data_processing_ref` will always fail.
**Why it happens:** The bundled fixtures happen to not carry per-spectrum data processing refs (the ref lives at run level in `RunMetadata`).
**How to avoid:** Test that the field is readable and is empty/optional-absent for all bundled spectra. Do not write a positive-value assertion.

### Pitfall 4: `parameters` column at `spectrum` level vs `scan` level vs `precursor/activation` level

**What goes wrong:** Confused nesting — `scan.parameters` (scan-level CV terms) and `spectrum.parameters` (spectrum-level CV terms) are siblings at different levels of the struct hierarchy. Accessing the wrong parent gives wrong data.
**Why it happens:** Both are named `parameters` at their respective levels.
**How to avoid:** Always start from the correct top-level column (`spectrum`, `scan`, `precursor`, `selected_ion`) before diving into nested fields.

### Pitfall 5: `auxiliary_arrays` decode — `data_type` string is an Arrow dtype name, not a PSI accession

**What goes wrong:** Routing the `data_type` string through the `PSI::DataType` enum parser (which expects `"Float32"`, `"Float64"`, etc. in PSI format) may fail if the stored string uses a different casing or format.
**Why it happens:** Schema is not yet validated against live aux data (all fixtures have 0 aux arrays).
**How to avoid:** Treat `data_type` as an opaque string initially; build a small local switch for `"float32"`, `"float64"`, `"int32"` etc. Do not assume the PSI enum covers all possible values.

### Pitfall 6: Missing clang-format step causes phase gate failure

**What goes wrong:** The per-phase standing gate requires `clang-format -i` on every edited file + diff-verify before commit. Skipping it causes the gate check to fail.
**How to avoid:** Plan must include a Wave-final task: `clang-format -i <all edited files>` then `git diff --exit-code` to confirm no diff remains. This applies to `spectrum_metadata.h`, `metadata_model.cpp`, `metadata_model.h`, and any new test file.

---

## Code Examples

### Read `MS_1000559_spectrum_type` (extending the existing loop)

```cpp
// Source: verified pattern from metadata_model.cpp:186-200 + pyarrow schema check
m.spectrum_type = get_string(spectrum, "MS_1000559_spectrum_type", r);
m.lowest_observed_mz =
    opt_double(spectrum, "MS_1000528_lowest_observed_mz_unit_MS_1000040", r);
m.highest_observed_mz =
    opt_double(spectrum, "MS_1000527_highest_observed_mz_unit_MS_1000040", r);
m.data_processing_ref = get_string(spectrum, "data_processing_ref", r);
```

### Read large_list CvParams from `spectrum.parameters`

```cpp
// Source: inferred from pyarrow schema + Arrow C++ API; pattern mirrors
// cv_params_from_json in run_metadata.cpp but operating on Arrow arrays.
static std::vector<CvParam>
read_cv_params_from_list(const std::shared_ptr<arrow::StructArray>& parent,
                         const char* list_field_name,
                         int64_t row)
{
    std::vector<CvParam> out;
    auto list_col = parent->GetFieldByName(list_field_name);
    if (!list_col || list_col->IsNull(row)) return out;
    // list_col is large_list; cast to LargeListArray
    auto la = std::static_pointer_cast<arrow::LargeListArray>(list_col);
    if (la->IsNull(row)) return out;
    auto items = std::static_pointer_cast<arrow::StructArray>(la->values());
    auto begin = la->value_offset(row), end = la->value_offset(row + 1);
    for (int64_t k = begin; k < end; ++k) {
        CvParam p;
        // accession: string
        // name: large_string
        // unit: string
        // value: struct<integer,float,string,boolean> → serialize non-null arm
        out.push_back(p);
    }
    return out;
}
```

### pyarrow ground-truth validation script (test scaffolding)

```python
# To assert new fields against fixture ground truth:
import zipfile, io, pyarrow.parquet as pq
with zipfile.ZipFile("test/files/small.mzpeak") as z:
    with z.open("spectra_metadata.parquet") as f:
        t = pq.read_table(io.BytesIO(f.read()))

sa = t.column("spectrum").combine_chunks()
# spectrum_type for row 0:
assert sa.field("MS_1000559_spectrum_type")[0].as_py() == "MS:1000579"
# lowest_mz for row 0:
assert abs(sa.field("MS_1000528_lowest_observed_mz_unit_MS_1000040")[0].as_py() - 200.00018816645024) < 1e-9

# precursor for row 0 (spectrum index 2):
prec = t.column("precursor").combine_chunks()
assert prec.field("isolation_window").field("MS_1000827_isolation_window_target_mz")[0].as_py() == pytest.approx(810.789, abs=1e-3)
```

---

## State of the Art

| Old Approach (pre-Phase 1) | Current Approach (Phase 1 target) | Impact |
|---|---|---|
| `SpectrumMetadata` exposes 10 scalar fields (ms_level, polarity, RT, representation, counts, base_peak, TIC) | Extend to 15 fields: add spectrum_type, lowest/highest_mz, data_processing_ref, CvParam list, PrecursorInfo | Enables Phase 2 to dispatch all common mzPeak CV accessions via the `handleCVParam_` mirror |
| `read_spectra_metadata` reads only the `spectrum` struct child | Read `spectrum` + join `precursor` + join `selected_ion` in the same pass | Precursor/activation data surfaced for all 34 MS2 spectra in small.mzpeak |
| `auxiliary_arrays` not accessible | `AuxiliaryArray` list on `SpectrumMetadata` (schema + decode skeleton) | Foundation for Phase 2 FloatDataArrays mapping |

**Deprecated/outdated:**
- The CONTEXT.md note "no bundled fixture has MSn precursor columns" is incorrect — small.mzpeak has 34 MS2 spectra with full precursor data. The precursor/activation/selected_ion subset of RDR-10c should be implemented in Phase 1, not deferred.

---

## Assumptions Log

| # | Claim | Section | Risk if Wrong |
|---|-------|---------|---------------|
| A1 | The `value` union in `parameters` items has child names `"integer"`, `"float"`, `"string"`, `"boolean"` as seen in pyarrow output | Schema / Code Examples | Arrow C++ field access would return nullptr; verify with `schema->field(i).name()` during implementation |
| A2 | `data_type` in `auxiliary_arrays` items is a lowercase string like `"float32"` | Architecture Patterns / RDR-9b | Decode routing would fail; no live data to validate against in bundled fixtures |
| A3 | The `large_list<CvParam>` in `selected_ion.parameters` is always empty list (not null) in bundled data | Common Pitfalls | Null-check required if assumption is wrong; low risk since the empty-list vs null distinction is handled by null-check anyway |

---

## Open Questions

1. **How to surface `PrecursorInfo` to callers — on `SpectrumMetadata` or via a separate method on `Spectra`/`Spectrum`?**
   - What we know: `SpectrumMetadata` is a plain struct passed by value; adding a nested `optional<PrecursorInfo>` member is consistent with how `optional<int> ms_level` works.
   - What's unclear: if a spectrum can have multiple precursors (DDA/DIA multiple precursor rows), `optional<PrecursorInfo>` would drop them. The `selected_ion` table has `precursor_index` suggesting multiple selected ions per precursor.
   - Recommendation: model as `optional<PrecursorInfo>` with `PrecursorInfo::selected_ions` as `std::vector<SelectedIonInfo>` to handle multiple ions. For multiple precursors (rare in bundled data), use `std::vector<PrecursorInfo>` on `SpectrumMetadata`. Planner decides.

2. **Should `AuxiliaryArray::values` be decoded immediately (in `read_spectra_metadata`) or lazily (on first access)?**
   - What we know: no bundled fixture exercises this path. Eager decode would add dead code in current tests; lazy would complicate the API.
   - Recommendation: decode eagerly during `read_spectra_metadata` (since number_of_auxiliary_arrays==0 in all current fixtures, the loop body never runs; cost is zero). Keeps the API simple.

3. **`scan` struct fields for Phase 1 scope?**
   - What we know: `scan.parameters` (scan-level CV terms like `MS:1000800` mass resolving power) and `scan.scan_windows` are populated in small.mzpeak. These are useful for Phase 2 (they map to `InstrumentSettings` in OpenMS).
   - What's unclear: CONTEXT.md does not mention scan-level CV terms as a Phase 1 target (only spectrum, precursor, aux arrays).
   - Recommendation: defer scan-level CV params to Phase 2. RDR-10b targets only the `spectrum` struct. Planner should confirm.

---

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| python3 + pyarrow | Ground-truth validation | ✓ | python3.7 + pyarrow 0.15 (system) | — |
| meson | Build | ✓ | (present, build confirmed working) | — |
| Arrow C++ headers | `metadata_model.cpp` compile | ✓ | (project dependency, already used) | — |
| Boost.Test | Unit tests | ✓ | (project dependency, all existing tests use it) | — |

No missing dependencies.

---

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Boost.Test (header-only, `BOOST_AUTO_TEST_CASE`) |
| Config file | `meson.build` — tests registered as `test(name, executable(...))` |
| Quick run command | `cd builddir && meson test -t 60 spectrum_metadata` (single suite) |
| Full suite command | `cd builddir && meson test && scripts/e2e.sh` |

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

- **Per task commit:** `cd builddir && meson test -t 60 spectrum_metadata_test` (new test only)
- **Per wave merge:** `cd builddir && meson test` (all unit tests)
- **Phase gate:** `cd builddir && meson test && scripts/e2e.sh` (full suite including e2e)

### Wave 0 Gaps

- [ ] `test/spectrum_metadata_test.cpp` — covers RDR-10b / RDR-10c / RDR-9b assertions listed above
- [ ] Register in `meson.build` under the test block (pattern: `test('spectrum_metadata_test', executable('spectrum_metadata_test', 'test/spectrum_metadata_test.cpp', dependencies: [mzpeak_dep]))`)

---

## Security Domain

Phase 1 is a pure in-process data-structure extension with no network I/O, authentication, user input surfaces, or cryptographic operations. No ASVS categories apply. `security_enforcement` is enabled but no actionable controls are relevant to reading Parquet columns from already-opened files.

---

## Sources

### Primary (HIGH confidence)

- pyarrow direct inspection of `test/files/small.mzpeak`, `test/files/has_uv.mzpeak`, `test/files/small.chunked.mzpeak`, `test/files/small.numpress.mzpeak` — spectra_metadata.parquet schema and data values
- `src/util/metadata_model.cpp` — existing `read_spectra_metadata` implementation (file:line references throughout)
- `include/mzpeak/spectrum_metadata.h` — current `SpectrumMetadata` struct
- `include/mzpeak/run_metadata.h` — `CvParam` struct definition
- `include/mzpeak/util/encoding.h` — existing decode patterns for auxiliary/secondary arrays
- `.planning/phases/mzpeak-openms-01-reader-lossless-map-prerequisites/01-CONTEXT.md` — locked decisions
- `docs/openms-integration-plan.md §7` — reader prerequisites definition
- `docs/reader-backlog.md` — RDR-10/9 history and extension specs

### Secondary (MEDIUM confidence)

- `test/files/small.mzML` — grep-verified MS1+MS2 content (48 spectra with MS level 2 present)
- `src/spectra.cpp` — `read_from` / `fetch` patterns showing how `SpectrumMetadata` is currently used

---

## Metadata

**Confidence breakdown:**
- spectra_metadata.parquet column names and types: HIGH — directly verified via pyarrow
- CvParam value union field names: HIGH — directly observed in pyarrow output
- Precursor/isolation/activation data populated in small.mzpeak: HIGH — directly verified
- Ion mobility null in all fixtures: HIGH — directly verified across all 4 fixtures
- auxiliary_arrays data empty in all fixtures: HIGH — directly verified
- RDR-9b decode skeleton correctness: MEDIUM — no live data to exercise it

**Research date:** 2026-06-14
**Valid until:** Stable (schema is written to fixture files; only changes if fixtures are regenerated)
