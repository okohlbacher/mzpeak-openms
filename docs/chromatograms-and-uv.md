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

> **Status note (added after implementation).** This document is the PLAN that
> preceded the chromatogram/UV work; the list below records what was true when
> it was written.  Since then:
>
> - the **count** defect is FIXED -- `Chromatograms`/`WavelengthSpectra` no
>   longer take a declared count of zero at face value and fall back to the
>   data-derived count (`src/chromatograms.cpp`);
> - the **grouping-key** defect is STILL PRESENT as described:
>   `ArrayIndex::dimensions()` chunks on `array_name + data_type + array_type`
>   only (`src/data/array_index.cpp`).  Phase B below proposed adding `unit` to
>   that key; that is NOT what was done.  Coalesced arrays are instead handled
>   as complementary columns, with the unit exposed through the
>   `intensity_unit()` accessors, and the decoder still throws if two
>   complementary columns are both non-null on one row.

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

---

# What was built

All five phases landed on `feat/chromatogram-uv-metadata`.

## Reading

`Chromatogram::metadata()` and `WavelengthSpectrum::metadata()` return
`ChromatogramMetadata` / `WavelengthSpectrumMetadata`, read through the same
facet machinery as spectra, so the legacy nested layout and the split layout
both work. `Chromatograms` and `WavelengthSpectra` gained `index_for_id` and
`by_id`, which finally use the `read_entity_id_map` that had been sitting in
`metadata_model.cpp` uncalled, with a doc comment claiming callers that did not
exist.

The precursor and selected-ion passes are now templates shared with the spectra
reader rather than a second copy. The join is subtle — H1 by `source_index`
VALUE, H2 by `(source_index, precursor_index)`, never positional — and two
copies of it would have drifted.

`Chromatogram::time()` reports SECONDS. It converts using the unit declared in
the array index and throws on a unit that is neither minutes nor seconds rather
than guessing. This is a **breaking change**: values are 60x their previous
magnitude for every file that stores minutes, which is every file seen so far.

`intensity_unit()` (both entity types) and `wavelength_unit()` report the unit
of the column that actually supplied the values. Without it, `has_uv`'s TIC in
detector counts and its DAD trace in absorbance units arrive through one
unitless accessor.

## Writing

`write_run_directory` / `write_run_archive` take a `RunContents` carrying all
three entity types together. Metadata is emitted in the split layout, the only
one the current reference reader resolves through.

Three refusals, each covering something the reference does silently:

| refused | why |
|---|---|
| a chromatogram whose type implies an SRM/MRM product | the product facet has no writer anywhere and an unimplemented reference reader path, so the file would carry Q1, lack Q3, and look valid |
| chromatograms in different intensity units | carrying both needs a column per unit, which this writer does not emit; writing them anyway labels absorbance as detector counts |
| mismatched parallel arrays | caught before anything is written |

Summary fields are computed from the data actually written, so the two
reference-writer defects are not reproduced: a maximum seeded at zero (which
records lambda max 0 and base peak 0 for an all-negative absorbance spectrum)
and an observed range derived from the unsorted input after a sorted copy has
been written (which can report a low above its high). Both are pinned by tests.

Run-level metadata is emitted unconditionally, not only when chromatograms are
present — the reference copies it in its chromatogram close path alone, so a
wavelength-only archive loses the version, CV list and run description.

## Cross-implementation status

The Rust reference opens a mixed archive written here and reads its spectra
correctly.

It **refuses a chromatogram-only or UV-only archive** with "Spectrum data entry
not found". That is a limitation of the reference reader rather than of the
file: nothing in the specification requires an archive to contain mass spectra,
and a UV-only run from a diode-array detector is a real thing. Our reader opens
such an archive.

The reference's `read` example only iterates spectra, so it cannot confirm our
chromatogram and UV tables value-for-value. That validation stays open.

## Still deliberately absent

`auxiliary_arrays` (a nested blob-with-parameters structure; nothing in any
fixture populates it), chunked *writing* for any entity type (reading works;
the spectra writer lacks it too), and any product support. Wavelength
`scan_windows` are not read: the reference maps their limits to m/z units even
for UV spectra, and reading them would launder nanometres-labelled-as-m/z into
the API.

## What the final review changed

Two independent reviews of the completed change. One **critical** defect and
several real interoperability breaks, all fixed:

- **Undefined behaviour.** Both collection constructors evaluated `*count` on an
  empty `std::optional` whenever the metadata file declared no entity count and
  the signal table had rows. The count keys are not required, and `Index`
  leaves the value unset whenever the metadata member is absent. There is now a
  test that removes the metadata member from a written run and reads it back.
- **The reference reader could not load our chromatograms.** It opens the
  precursor and selected-ion facets unconditionally, so a plain TIC without
  them failed with `NotFound`. Both are emitted with their schema and zero rows,
  exactly as the spectra writer already did.
- **An empty unit was treated as minutes.** An absent unit reached
  `Chromatogram::time()` and was silently multiplied by 60 — so a chunked file
  that stores seconds and omits the optional `buffer_priority` would have had
  every time inflated 60x with no signal. An absent or conflicting unit is now
  refused, and the chunked path records a unit even when no entry is primary.
- **An empty intensity unit produced an invalid array index.** `"unit":""`
  violates the schema and the reference's CURIE parser rejects it. Refused at
  write time.
- **`cv_list` was absent or malformed.** Conformance requires every CURIE prefix
  used to be declared; nothing emitted one, and `RunMetadata`'s own default used
  `fullName`/`URI` instead of `full_name`/`uri` and omitted UO despite every
  unit CURIE being a UO term. One definition now serves both.
- **SIM was misclassified as product-bearing.** Only SRM/MRM has a product.
  Selected ion monitoring names a Q1 and nothing else, so flagging it claimed a
  lost product that never existed. The writer still refuses both, now for their
  own stated reasons — it can carry neither a Q1 nor a Q3.
- **UV/Vis lost information the reference could have read.** `spectrum_type` and
  `spectrum_representation` were not written, the summary columns declared no
  unit (an absorbance base peak was indistinguishable from a detector count),
  and there was no scan facet — and the reference ignores the primary `time`
  column outright, so acquisition time was invisible to it.
- Smaller: the UV total ion current accumulated in `float` (a thousand small
  samples beside one large one vanished); NaN axis values broke the sort order
  the footer promises; and several `data_kind: "metadata"` members for one
  entity could hand a facet table to the primary reader, which then returned an
  empty map with no error.

`test/chunked_chromatograms_test.cpp` was **never in the build** — the chunked
chromatogram path had no automated coverage at all, and the file still asserted
the pre-change minutes convention. It is now wired up and correct.

### Known limitations, recorded rather than hidden

- **`column_mapping` is parsed but not consulted.** Field resolution works from
  hard-coded names plus three aliases, so a split-layout file that names a
  column something else and maps it to the right CV term reads as absent. The
  reference resolves through the mapping.
- **Dimensions are grouped without regard to physical dtype**, so a file storing
  the same semantic array as float32 and float64 siblings can concatenate them.
- **Chunked multi-unit arrays are neither coalesced nor unit-aligned**; only the
  point layout handles the sibling-column case.
- **Metadata decoding assumes the reference writer's exact Arrow widths** — a
  float64 where the reference writes float32 reads as null.
- A precursor whose `precursor_index` is null cannot be told apart from another
  null one, so selected ions attach to the first.

None of these is a regression; all predate this work or follow from the
decoder's existing shape, and each needs more than a local fix.
