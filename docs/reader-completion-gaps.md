# mzPeak OpenMS C++ Reader — Completion Gap Analysis

What must be closed to finish the **reader** end of the implementation. Grounded in empirically probing the fixed reader (PRs #3–#5 applied) against every bundled test file, plus code inspection.

## Empirical coverage (current reader vs. test files)
| File | Layout / content | Result |
|------|------------------|--------|
| `small.mzpeak`, `small.dir` | point, profile+centroid | 48 spectra: **14 ok**, **29 empty** (centroid→peaks table not read), **5 throw** (`index not in column map`) |
| `Example_Processed.img.mzpeak` | point, imaging | **9/9 ok** ✓ |
| `small.chunked.mzpeak` | chunked | **total failure at open**: `column index out of bounds for column: chunk.mz_chunk_values` |
| `small.numpress.mzpeak` | chunked + numpress | **total failure** (same chunked cause) |
| `has_uv.mzpeak` | UV/wavelength + chromatograms | **total failure at open**: `Parquet magic bytes not found` — its `spectra_data.parquet` has **0 rows** (valid Parquet; reader still cannot open it) |

So today the reader fully handles exactly one case: **point-layout profile spectra in a non-empty data table**. Everything else is empty, wrong, or unreadable.

## TIER 1 — Correctness / coverage blockers (reader returns wrong or no data)

**G1. Peaks table not read → centroid spectra empty (Bug B).** `Index::spectra()` (`src/index.cpp:97`) hard-codes `"spectra_data.parquet"` and builds `Spectra` from that one file; `Spectra::fetch` queries only it. In small.mzpeak the 48 spectra are **14 profile + 34 centroid**; the 34 centroid (peaks-only) spectra are unserved (surfacing as 29 empty + 5 throw — see G2). **Close (per codex — do NOT "route to whichever" or silently merge):** model the spectrum's **representation** from metadata — `MS_1000525_spectrum_representation` (profile MS:1000128 / centroid MS:1000127) plus `MS_1003060_number_of_data_points` / `MS_1003059_number_of_peaks` — and read profile from `spectra_data` and centroid from `spectra_peaks` accordingly (a spectrum may legitimately have both). Depends on G13.

**G2. Uncaught throw for spectra absent from the data table (Bug C4).** Spectra whose index is in no `spectra_data` row group throw `index not in column map` (`encoding.h:100`) — 5/48 in small.mzpeak. Coupled to G1 (those indices live only in peaks, or beyond the data range). **Close:** treat "column/rows absent" as empty/route-to-peaks, not an exception.

**G3. Null-marked m/z silently decoded as 0 (Bug C-null).** `decode_point` pushes `0` for null m/z (`encoding.h:130`, `FIXME`). Per spec `signal-data.md#null-marking` + Rust `fill_nulls_for`, null coordinate values MUST be reconstructed from the per-spectrum `mz_delta_model` (WLS δmz~β0+β1·mz+β2·mz²) / segment-median; null intensity is correctly 0. Affects the interior of every null-marked profile spectrum (silent corruption; not caught by endpoint checks). **Close:** read `mz_delta_model` from `spectra_metadata.parquet`; implement segment-median + regression fill; honor `transform` MS:1003901/1003902.

**G4. Chunked layout completely unreadable.** Fails at column-path resolution: `schema->ColumnIndex("chunk.mz_chunk_values")` returns −1 (`src/util/parquet.cpp:241`) because `mz_chunk_values` is a `large_list` whose Parquet leaf path is longer than the dotted array-index `path`. This throws before any decode. Behind it sit more gaps: `decode_array` assumes a single column (`encoding.h:76`, `columns.size()==1`); `decode_chunked` is unimplemented (`encoding.h:87` literally `throw("not implemented")` — also a `const char*` throw, uncatchable as `std::exception`). (Note: `prefix_`'s `"point"` default is NOT a blocker — the JSON ctor overwrites it from the array index's `prefix`, `array_index.cpp:36`.) **Close (layered):** (a) map list/nested column paths to their Parquet leaf index; (b) read `chunk_start`/`chunk_end`/`chunk_values` + `chunk_encoding`; (c) implement `decode_chunked` incl. delta-decode (MS:1003089) and basic (MS:1000576); (d) reconstruct per-chunk values and concatenate. Half the test files (chunked, numpress) need this.

**G5. Numpress not decoded.** Chunked files using numpress linear (MS:1002312) / SLOF (MS:1002314) need byte-column (`*_numpress_*_bytes`, `large_list<u8>`, `buffer_format=chunk_transform`) decoding. **Close:** vendor ms-numpress (OpenMS already ships a copy) and wire it into the chunked decoder. Depends on G4.

**G6. Small/empty Parquet member fails to open (Arrow buffer-size bug).** `has_uv.mzpeak` cannot be opened — its `spectra_data.parquet` is valid Parquet (0 rows, 2606 bytes, PAR1 at both ends) but the reader throws `Parquet magic bytes not found`. **Root cause (per codex):** the Arrow file adapter allocates a fixed ~64 KB buffer and hands it to Arrow **without shrinking to the actual bytes read** (`src/util/arrow.cpp:30`, `:93`), so for any member smaller than the buffer Arrow looks for `PAR1` at the wrong offset. This is a general small-file bug, not specific to empty tables or wavelength. **Close:** return a buffer sized to bytes actually read.

## TIER 2 — Missing entity coverage (whole data categories invisible)

**G7. Chromatograms: no API at all.** `Index` exposes only `spectra()` (`include/mzpeak/index.h:39`). Every test file has `chromatograms_{metadata,data}.parquet` (e.g. has_uv: 738 points) — entirely inaccessible. **Close:** add `Chromatogram`/`Chromatograms` + `Index::chromatograms()`.

**G8. Wavelength / UV spectra: no API.** `wavelength_spectra_{metadata,data}.parquet` (has_uv: 49,920 points) unreachable. **Close:** add wavelength-spectra access (mirrors spectra, no precursor/selected_ion facets).

**G9. Spectrum metadata not surfaced.** `Spectrum` exposes only `mz()`, `intensity()`, `raw_encoded_arrays()`, `array_index()`. The rich per-spectrum metadata in `spectra_metadata.parquet` (ms_level, scan start time, polarity, precursor, selected_ion, isolation window, activation, CV params, base peak/TIC, spectrum representation) is never parsed/exposed. `Metadata` (`src/metadata.cpp`) is a thin wrapper with no model. **Close:** parse the metadata facets and expose them on `Spectrum`/a metadata model. (Large but central to a usable reader.)

**G10. Only m/z + intensity arrays exposed.** `Spectrum` hardcodes two arrays; stored secondary arrays (charge, ion mobility, signal-to-noise, etc.) are dropped. **Close:** generic per-array access keyed by `ArrayType`. NB **auxiliary arrays are separate** (codex): they live in the **metadata** rows (`auxiliary_arrays`, gated by `number_of_auxiliary_arrays`, `spec/.../auxiliary-arrays.md`), not the data table — they need explicit metadata decoding (Rust loads them via `reader.rs:1034`), not just "generic data-array access."

## TIER 3 — Robustness / correctness hardening

**G11. `record_count` fallback broken (Bug I).** `parquet.cpp:136` turns missing count metadata into 0 (so `data_arrays.cpp:211` returns 0 instead of falling back); the stats fallback (`data_arrays.cpp:227`) returns max index, not count (off-by-one). The reference writer emits `*_count` footer KV, so this is normally masked — but it's fragile.

**G12. Query eval reads nullable values without a null check (Bug J).** `data_arrays.cpp:127` calls `Value(i_)` on nullable columns; a predicate on a nullable column reads invalid data. Not triggered today (spectrum_index is non-nullable) but latent.

**G13. File resolution by name, not by `entity_type`+`data_kind`.** `Index::spectra()` matches the literal filename; brittle and blocks G1. **Close:** resolve sources via the index's `files[]` entity_type/data_kind (the schema enums already exist).

**G14. Throw hygiene.** `encoding.h:87` `throw("not implemented")` throws a `const char*` (not a `std::exception`); the unimplemented/peaks paths throw where empty/whole-file errors would be safer. **Close:** consistent `MzPeakError` types; no bare-literal throws.

## TIER 1.5 — Type-system & correctness blockers surfaced by codex review
**G16. No unsigned / list / string / byte-array types.** `PSI::DataType` models only signed `Int32/Int64`, `Float32/Float64`, `ASCII` (`schema/psi/data_type.h:24`). But entity indices are `uint64` and the format uses `large_list`/`large_string`/`large_list<u8>` extensively. The current `uint64` index reads work only by reinterpret "layout luck." **Close:** add unsigned widths + list/string/byte-array data types and casts. (Blocks robust point reads too, and all of chunked/numpress/aux.)

**G17. Wavelength entity naming is wrong (space vs underscore).** The entity→prefix/key derivation would produce `"wavelength spectrum_array_index"` and `"point.wavelength spectrum_index"` (space), but the format uses `wavelength_spectrum_array_index` / `wavelength_spectrum_index` (`array_index.cpp:22`, `constants.rs:9`). So G8 needs the naming fixed, not just a new API.

**G18. Multiple arrays of the same `array_type` break primary selection.** `decode_array` throws unless exactly one column has the requested `array_type` (`encoding.h:74`). The spec allows multiple arrays of a type (e.g. intensity in different units) and designates the primary via `buffer_priority` (`signal-data.md`). **Close:** select the primary by `buffer_priority`, expose the rest.

**G19. Missing row-group statistics are treated as "no match" → queries can silently drop data.** `run_query` returns nullopt when stats are absent and `Query::eval` turns nullopt into `false` (`parquet.cpp:186`, `query.cpp:93`), so a row group lacking stats is pruned out entirely. **Close:** conservatively INCLUDE row groups with missing stats (or use the page index). Correctness bug, not just robustness.

## TIER 4 — Explicitly future (per README "summer 2026")
**G15. Remote/cloud reading** (HTTP range / S3 / object_store prefix). `open()` explicitly rejects non-local paths (`src/open.cpp:24`) while the spec includes remote prefixes. Out of scope for the local-file reader milestone.

## Definition of done (reader)
A complete reader should: open point AND chunked archives (incl. numpress + delta), in zip AND directory AND empty-table forms; return correct m/z+intensity for profile (null-reconstructed) and centroid spectra from both data and peaks tables; expose spectrum metadata, chromatograms, wavelength spectra, and auxiliary/secondary arrays; resolve sources by entity_type/data_kind; and never silently corrupt or hard-crash on a conforming file.

## Suggested closing order (each its own reviewed PR)
1. **G6 + G19** — quick correctness fixes first: the Arrow small-buffer bug (one function, `arrow.cpp`) and the missing-stats "no match" pruning (silent data loss). Both small, both unblock/derisk later testing.
2. **G13 + G1 + G2** — representation-driven source model (profile→data, centroid→peaks) keyed on `MS_1000525` + counts; read peaks table; graceful absent-spectrum (unblocks the 34 centroid spectra + removes throws). *Highest coverage win.*
3. **G16** — type system (unsigned + list/string/byte-array). Foundational: blocks robust point reads (uint64 indices) and is a prerequisite for chunked/numpress/aux. Consider doing early.
4. **G3** — null m/z delta-model reconstruction (silent correctness; pairs with the writer's null-marking).
5. **G4 (+G5, +G18)** — chunked layout (+numpress, +primary-array selection) decoder (unblocks 2 test files; pairs with the writer's chunked encoder).
6. **G7 / G8(+G17) / G9 / G10** — chromatograms, wavelength (with underscore naming), metadata, multi-/auxiliary-array exposure (breadth).
7. **G11 / G12 / G14** — remaining robustness hardening.

Synergy: G3/G4/G5 mirror writer phases P2/P3 — implement encode+decode together for self-checking round-trips.

## Adversarial review note (codex)
This analysis was codex-reviewed. Corrections folded in: G1 close-strategy is representation-driven, not silent-merge (14 profile + 34 centroid in small.mzpeak); G4's `prefix_` default is not a blocker (overwritten from JSON); **G6's true root cause is the Arrow ~64 KB buffer not shrunk to bytes read** (`arrow.cpp:30,93`), not record_count. Added blockers G16 (types), G17 (wavelength naming), G18 (duplicate array_type / primary selection), G19 (missing-stats pruning), and the auxiliary-array clarification on G10.
