# Chromatograms and UV/Vis spectra — read and write

The contract sections are verified against `has_uv.mzpeak` and
`small.chunked.mzpeak` directly, then checked against the Rust reference, the
mzPeak specification and `mzdata` by an adversarial review.

## Where we actually are

Verified by reading the code and probing the fixtures, not assumed.

| | chromatograms | wavelength spectra |
|---|---|---|
| point layout | reads | reads |
| chunked layout | reads (`chunk_*` machinery shared with spectra) | untested — no fixture |
| coalesced arrays | reads correctly | n/a |
| unit of a coalesced array | **not exposed** | n/a |
| non-standard arrays (`ms_level`) | **dropped** | n/a |
| metadata | **none** | **none** |
| `by_id` | **missing** (`read_entity_id_map` is dead code) | **missing** |
| writer | **none** | **none** |

Two defects worth naming separately:

- **`Chromatogram::time()` returns MINUTES.** The array's unit is UO:0000031 in
  every fixture, and nothing converts. `EicPoint::time` from
  `Spectra::extract_ion_chromatogram` is SECONDS. Two chromatogram-shaped APIs
  in one library, differing by 60x. The spectra path already shipped this bug
  once.
- **`read_entity_id_map` is never called**, and its doc comment claims it is
  used by `Chromatograms::by_id()` and `WavelengthSpectra::by_id()`. Neither
  exists.

A hypothesis I checked and had to discard: I expected `Chromatogram::intensity()`
to concatenate `has_uv`'s two intensity columns. It does not. The columns are
complementary (`intensity` non-null on 212 rows, `intensity_f32_au` on the
other 526) because the two chromatograms carry different units — counts
(MS:1000131) versus absorbance units (UO:0000269). That is coalescing, the
reader handles it, and only the *unit* is lost.

## The contract, from ground truth

`mzpeak_index.json` carries a flat `files` list; each entry is
`{name, entity_type, data_kind}` with `entity_type` in
{`spectrum`, `wavelength spectrum`, `chromatogram`} and `data_kind` in
{`data arrays`, `peaks`, `metadata`}. Adding an entity type is adding files
plus index entries — no schema versioning involved.

**Chromatograms** — `chromatograms_data.parquet` +
`chromatograms_metadata.parquet`. Point layout:
`point: struct<chromatogram_index: uint64, time: double, intensity: float, …>`;
chunked layout uses `chunk:` with `time_chunk_start/_end/_values`,
`chunk_encoding`, and `chunk_secondary` arrays. Metadata is a nested table with
`chromatogram` + `precursor` + `selected_ion` struct columns; the primary
struct is `index, id, MS_1000465_scan_polarity, MS_1000626_chromatogram_type,
data_processing_ref, MS_1003060_number_of_data_points, parameters,
auxiliary_arrays, number_of_auxiliary_arrays`.

**Wavelength spectra** — `wavelength_spectra_data.parquet` +
`wavelength_spectra_metadata.parquet`.
`point: struct<wavelength_spectrum_index: uint64, wavelength: float,
intensity: float>` — wavelength is **float32**. Metadata is `spectrum` + `scan`
struct columns, with `index, id, time, MS_1000559_spectrum_type,
MS_1000525_spectrum_representation, lowest/highest observed wavelength,
MS_1003060_number_of_data_points, MS_1003812_lambda_max, base peak intensity,
total ion current, data_processing_ref, parameters, auxiliary_arrays,
number_of_auxiliary_arrays`.

Reference oddities to not reproduce blindly:

- `chromatogram_data_point_count` is **0** in `has_uv.mzpeak` despite 738 rows.
  The same key is 0 for wavelength spectra despite 49,920 rows. Whatever we
  write, we must not depend on this key being meaningful when reading.
- The wavelength column `MS_1003812_lambda_max_unit_UO:0000018` uses a **colon**
  where every other column uses `unit_UO_0000018`. Any name parsing has to
  tolerate both.

## Phases

Each phase ends green on the full suite, with a round trip through our own
reader and, where a fixture allows, a cross-check against the Rust reference.

**Phase A — metadata read for both entity types.**
Add `ChromatogramMetadata` and `WavelengthSpectrumMetadata`, read through the
existing facet reader in `metadata_model.cpp` (chromatograms: primary +
precursor + selected_ion; wavelength: primary + scan). Wire `by_id` using the
already-written, currently-dead `read_entity_id_map`. This is the phase that
makes SRM transitions readable at all.

**Phase B — close the two verified reader defects.**
Expose the unit of a coalesced array so counts and absorbance units are
distinguishable, and settle `time()` against `EicPoint` on seconds. The unit
change is additive; the time change is a **breaking behaviour change** and gets
its own commit and a note in the README.

**Phase C — writer, point layout, both entity types.**
`write_chromatograms_*` and `write_wavelength_spectra_*` alongside the existing
spectra writers, emitting data + metadata + index entries. Point layout only,
matching the current spectra writer's scope.

**Phase D — round-trip and cross-implementation validation.**
Our writer -> our reader (values, metadata, ids), then our writer -> Rust
reference reader, and reference fixtures -> our reader. This is where the
`_data_point_count` question gets settled empirically.

**Phase E — ponytail pass, then a whole-codebase adversarial review.**

Explicitly deferred, with reasons rather than silence: `auxiliary_arrays` (a
nested blob-with-parameters structure — better absent than half-written, and
nothing in the fixtures populates it), the chunked layout in the *writer*
(reading it already works; writing it is a separate body of work the spectra
writer also lacks), and a `products` facet for SRM until the review says
whether the reference ever writes or reads one.

## Tests and fixtures

Existing coverage to extend: `chromatograms_test`, `chunked_chromatograms_test`,
`wavelength_spectra_test`.

New fixtures needed, in order of necessity:

1. **SRM/MRM chromatogram** — nothing bundled has a precursor or product, so
   transitions are wholly untested. Hand-built, since no converter output is
   available.
2. **Chunked wavelength spectra** — `small.chunked.mzpeak` has chunked
   chromatograms but no chunked UV, so that reader path has never run.
3. **Writer round-trip fixtures** — generated by the new writer, so they are
   outputs rather than committed inputs.

Gate for every phase, matching the peak-path work: a digest over every decoded
chromatogram and UV spectrum, so a change to *when* things decode cannot alter
*what* they decode to.

## What the reference review changed

### There is no single reference contract — there are three

1. **Legacy nested metadata**, which is what our `has_uv.mzpeak` fixture is:
   one file with outer `chromatogram`/`precursor`/`selected_ion` structs, index
   strings spelled `wavelength spectrum` and `data arrays`, and no
   `column_mapping`.
2. **Current split metadata**, which is what the Rust writer emits today: each
   facet its own flat file, index strings spelled `wavelength_spectrum` and
   `data_arrays`, plain column names plus an explicit `column_mapping`.
3. **What the Rust reader accepts**, which is the split form only. Its ID
   indexer asks for top-level `id` and `index`, so it cannot open the nested
   fixture as metadata at all.

So "both layouts exist" is true and "the reference supports both end to end" is
false. **Our reader must accept both** (the same problem the spectra path
already solved) and **our writer must emit the split form**, or nothing else
can read what we produce.

### Reject SRM products; do not write them

The product facet is specified, `mzdata` models it, and a product owns the Q3
isolation window — but the Rust writer never iterates products, no product file
is ever emitted, `ChromatogramMetadataFacet` has no product map, and a
chromatogram/product index entry reaches a `todo!()`. Writing one would produce
a file that **panics** the reference reader.

An SRM trace with Q1 `500.2 m/z` and Q3 `184.1 m/z` therefore round-trips
through the reference losing `184.1`, and the result still looks like a valid
chromatogram. We will **reject** chromatograms carrying products with a clear
error rather than silently dropping the transition. Losing Q3 quietly is the
worst available option.

### Reference bugs we will not reproduce

- `ChromatogramType::to_curie()` in `mzdata` emits `MS:1000472`/`MS:1000473`
  for SIM/SRM while its own reader expects `MS:1001472`/`MS:1001473`. Emit the
  latter.
- Base-peak intensity is initialised to zero, so an all-negative UV spectrum
  gets `lambda_max = 0` and `BPI = 0`. Fixture spectrum 104's true maximum is
  −0.0863 at 362 nm. Either compute these honestly or omit them.
- Lowest/highest observed wavelength are derived from the *unsorted* input
  after a sorted copy has been written, so input `[400, 210, 300]` is stored
  ascending while the metadata claims low 400, high 300.
- `copy_metadata_to_index()` runs only in the chromatogram close branch, so a
  wavelength-only archive can lose `version`, the CV list and all run metadata.
- Scan windows are mapped to m/z units even for wavelength spectra. Omit the
  list until the wavelength-window unit is settled rather than writing
  nanometres labelled as m/z.
- Current Rust loses UV acquisition time on read three separate ways. Emit both
  the primary `time` and the scan start time anyway; the defect is theirs.

### Additional defects confirmed in our reader

- **The array-index grouping key omits `unit`.** `ArrayIndex::dimensions()`
  chunks on `array_name + data_type + array_type`, so counts (`MS:1000131`) and
  absorbance (`UO:0000269`) collapse into one logical dimension. On this
  fixture the numbers come out right only because the two columns are disjoint
  per entity; when both are non-null on one row the decoder throws. Adding
  `unit` to the key is the fix, and it is what makes an honest `intensity()`
  unit accessor possible at all.
- **We trust `chromatogram_count` / `wavelength_spectrum_count` outright.** A
  writer emitting `0` — and the reference does emit `0` for several count keys
  — yields a silently empty collection. Rust ignores these keys entirely. Fall
  back to the data-derived count rather than reporting empty.
- Float32 wavelength widening into `double` was checked and **is correct**: the
  decoder dispatches on the array index's declared type and widens values, it
  does not reinterpret bytes.

## Revised phases

**Phase A — metadata read, both layouts, both entity types.** Includes the
count fallback and `by_id` via the already-written, currently-dead
`read_entity_id_map`.

**Phase B — units.** Add `unit` to the dimension grouping key; expose the unit
of each array; settle `time()` against `EicPoint`'s seconds. The time change is
breaking and gets its own commit.

**Phase C — writer, point layout, split metadata, both entity types.** Emits
`column_mapping`, accurate counts, and correct SIM/SRM accessions. Rejects
products and rejects equal-length signal arrays it cannot preserve.

**Phase D — round trip through our reader, then cross-validate against the Rust
reference in both directions.**

**Phase E — ponytail pass, then a whole-codebase adversarial review.**

Still deferred, now with the review's backing: `auxiliary_arrays`, chunked
*writing*, and any product support.
