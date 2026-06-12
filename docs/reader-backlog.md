# mzPeak C++ Reader — Backlog

Actionable backlog to finish the **reader** end of the implementation. Each item is self-contained: symptom/evidence, root cause (`file:line`), definition of done, how to validate, and dependencies. Derived from an empirical probe of the **fixed reader** (PRs [#3](https://github.com/OpenMS/mzpeak/pull/3)–[#5](https://github.com/OpenMS/mzpeak/pull/5) applied) against every bundled test file, plus spec/Rust-reference cross-check, adversarially reviewed (codex). Full reasoning: [reader-completion-gaps.md](reader-completion-gaps.md).

## Context: current coverage
The reader today fully handles exactly **one** case — point-layout profile spectra in a non-empty data table.

| Test file | Content | Result |
|---|---|---|
| `small.mzpeak` / `small.dir` | point, profile+centroid | 48 → 14 profile ok, **34 centroid unserved** (29 empty + 5 throw) |
| `Example_Processed.img.mzpeak` | point imaging | 9/9 ok |
| `small.chunked.mzpeak` | chunked | unreadable (fails at open) |
| `small.numpress.mzpeak` | chunked + numpress | unreadable |
| `has_uv.mzpeak` | UV/wavelength + chromatograms | unreadable (small/0-row member) |

Priority key: **P0** correctness/crash, **P1** coverage blocker, **P2** breadth, **P3** hardening, **P4** future.
Traceability: each item notes its gap id (G#) and, where applicable, the original bug letter.

---

## Milestone R1 — Correctness quick wins (small, high-confidence)

### RDR-1 — Small/empty Parquet member fails to open  ·  P0 · G6
- **Symptom:** `MzPeak::open("has_uv.mzpeak")` throws `Parquet magic bytes not found in footer`. Reproduces from the unpacked directory; pyarrow reads the same member fine.
- **Evidence:** `has_uv`'s `spectra_data.parquet` is valid Parquet, 0 rows, 2606 bytes, `PAR1` at both ends.
- **Root cause:** the Arrow file adapter allocates a fixed ~64 KB buffer and returns it **without truncating to the actual bytes read** (`src/util/arrow.cpp:30`, `:93`), so Arrow looks for the `PAR1` footer at the wrong offset for any member < 64 KB.
- **Done when:** the adapter returns a buffer sized to bytes actually read; `has_uv.mzpeak` and any small/0-row member open without error.
- **Validate:** add a test opening `has_uv.mzpeak` (expect open OK, 0 spectra in the data table) and a tiny synthetic Parquet member.

### RDR-2 — Missing row-group stats are pruned as "no match" (silent data loss)  ·  P0 · G19
- **Symptom:** a predicate query can silently skip row groups that lack column statistics, dropping valid data.
- **Root cause:** `run_query` returns `nullopt` when stats are absent (`src/util/parquet.cpp:186`) and `Query::eval` turns `nullopt` into `false` (`src/query.cpp:93`), so a stats-less row group is excluded.
- **Done when:** row groups with missing stats are **conservatively included** (or resolved via the page index) rather than pruned.
- **Validate:** unit test with a predicate over a file/row-group lacking stats; assert all matching rows are returned.

---

## Milestone R2 — Spectrum coverage (the big win)

### RDR-3 — Resolve spectrum sources by entity_type/data_kind, read the peaks table  ·  P1 · G13+G1+G2 (Bug B, C4)
- **Symptom:** 34/48 centroid spectra in `small.mzpeak` are unserved — 29 return empty m/z, 5 throw `index not in column map` (`include/mzpeak/util/encoding.h:100`).
- **Root cause:** `Index::spectra()` hard-codes `"spectra_data.parquet"` (`src/index.cpp:97`) and `Spectra::fetch` queries only it (`src/spectra.cpp:31`); the `spectra_peaks.parquet` table is never read; absent rows throw instead of returning empty.
- **Done when:** spectrum sources are resolved from the index `files[]` by `entity_type==spectrum` + `data_kind` (the `EntityType`/`DataKind` enums already exist); a spectrum's **representation is metadata-driven** — read `MS_1000525_spectrum_representation` (profile `MS:1000128` / centroid `MS:1000127`) and the `MS_1003060_number_of_data_points` / `MS_1003059_number_of_peaks` counts, pulling profile from `spectra_data` and centroid from `spectra_peaks` (a spectrum may have both); absent rows return empty, never throw. **Do not silently merge tables.**
- **Validate:** `small.mzpeak` → all 48 spectra return non-empty m/z+intensity matching pyarrow ground truth from both tables; 0 throws.
- **Depends on:** independent. Unblocks the largest coverage gain.

### RDR-4 — Type system: unsigned, list, string, byte-array  ·  P1 · G16
- **Symptom:** `uint64` entity-index reads "work" only by reinterpreting signed bits; `large_list`/`large_string`/`large_list<u8>` columns can't be modeled.
- **Root cause:** `PSI::DataType` models only signed `Int32/Int64`, `Float32/Float64`, `ASCII` (`include/mzpeak/schema/psi/data_type.h:24`); the format uses unsigned indices and 32-/64-bit list/string/byte variants pervasively.
- **Done when:** `DataType` + the Parquet type-tag maps cover `UInt32/UInt64`, `list`/`large_list`, `string`/`large_string`, and `large_list<u8>`; the spectrum-index path uses the correct unsigned type.
- **Validate:** index values decode correctly across the full uint64 range; chunked/aux columns (RDR-6/RDR-10) can be typed.
- **Note:** foundational — prerequisite for chunked (RDR-6), numpress, and auxiliary arrays. Consider doing early in R2.

---

## Milestone R3 — Lossless profile fidelity

### RDR-5 — Reconstruct null-marked m/z from the delta model  ·  P1 (silent correctness) · G3 (Bug C-null)
- **Symptom:** interior null m/z in profile spectra decode to `0.0` — silent corruption not caught by endpoint checks.
- **Root cause:** `decode_point` pushes `0` for null values (`include/mzpeak/util/encoding.h:130`, `FIXME`).
- **Done when:** null coordinate values are reconstructed per spec (`signal-data.md#null-marking`): read the per-spectrum `mz_delta_model` (from `spectra_metadata.parquet`), apply the segment-median + WLS regression fill (mirror Rust `fill_nulls_for`, `hupo-mzpeak/src/filter.rs:543`); null **intensity** stays `0`; honor `transform` `MS:1003901`/`MS:1003902`.
- **Validate:** round-trip against a null-marked profile spectrum; reconstructed m/z monotonic and within tolerance of the reference reader's output.
- **Depends on:** RDR-3 (metadata access path). **Writer synergy:** pairs with writer null-marking (do encode+decode together).

---

## Milestone R4 — Chunked & Numpress layouts

### RDR-6 — Decode the chunked layout  ·  P1 · G4
- **Symptom:** `small.chunked.mzpeak` is unreadable — `open()` throws `column index out of bounds for column: chunk.mz_chunk_values`.
- **Root cause (layered):** (1) nested/list column-path resolution fails — `schema->ColumnIndex("chunk.mz_chunk_values")` returns −1 because the list leaf path is longer (`src/util/parquet.cpp:241`); (2) `decode_array` assumes a single column (`encoding.h:76`); (3) `decode_chunked` is unimplemented and uses a bare `throw("not implemented")` (`encoding.h:87`).
- **Done when:** list/nested column paths map to their Parquet leaf index; `chunk_start`/`chunk_end`/`chunk_values`/`chunk_encoding` are read; `decode_chunked` reconstructs per-chunk values for basic (`MS:1000576`) and delta (`MS:1003089`) encodings and concatenates them.
- **Validate:** `small.chunked.mzpeak` reads back equal (within tolerance) to `small.mzpeak`/pyarrow ground truth.
- **Depends on:** RDR-4 (list types). **Writer synergy:** pairs with writer chunked encoder.

### RDR-7 — Decode Numpress chunk transforms  ·  P1 · G5+G18
- **Symptom:** `small.numpress.mzpeak` unreadable (chunked + numpress).
- **Root cause:** `chunk_transform` byte columns (`*_numpress_linear_bytes`/`*_numpress_slof_bytes`, `large_list<u8>`) aren't parsed — `buffer_format` parsing falls back to point (`src/schema/buffer_format.cpp:37`); no numpress decode. Also `decode_array` throws on duplicate `array_type` rather than selecting the primary by `buffer_priority` (`encoding.h:74`) — needed when a transformed surrogate column coexists with the values column.
- **Done when:** numpress linear (`MS:1002312`) and SLOF (`MS:1002314`) decode via vendored **ms-numpress** (OpenMS already ships a copy); primary array selected by `buffer_priority`.
- **Validate:** `small.numpress.mzpeak` reads back equal to the un-numpressed ground truth; one byte-exact numpress decode unit test.
- **Depends on:** RDR-6.

---

## Milestone R5 — Entity & metadata breadth

### RDR-8 — Chromatogram read API  ·  P2 · G7
- **Symptom:** chromatograms (every test file has them; has_uv: 738 points) are entirely inaccessible.
- **Root cause:** `Index` exposes only `spectra()` (`include/mzpeak/index.h:39`).
- **Done when:** `Chromatogram`/`Chromatograms` types + `Index::chromatograms()` read `chromatograms_{metadata,data}.parquet` (point + chunked).
- **Validate:** TIC/SIC time+intensity match pyarrow ground truth.

### RDR-9 — Wavelength/UV spectra read API (+ correct naming)  ·  P2 · G8+G17
- **Symptom:** `wavelength_spectra_*` (has_uv: 49,920 points) inaccessible.
- **Root cause:** no API; **and** the entity→prefix/key derivation produces a space, `"wavelength spectrum_index"` / `"wavelength spectrum_array_index"`, instead of underscores `wavelength_spectrum_*` (`src/schema/array_index.cpp:22`; cf. `hupo-mzpeak/src/constants.rs:9`).
- **Done when:** wavelength-spectra access (mirrors spectra, no precursor/selected_ion facets) with correct underscore naming for prefix, index column, and `*_array_index` KV key.
- **Validate:** `has_uv.mzpeak` wavelength arrays read back equal to ground truth (requires RDR-1).
- **Depends on:** RDR-1.

### RDR-10 — Expose spectrum metadata, secondary & auxiliary arrays  ·  P2 · G9+G10
- **Symptom:** only `mz()`/`intensity()` are exposed; ms_level, scan, precursor, selected_ion, isolation window, activation, CV params, polarity, base-peak/TIC, etc. are dropped, as are secondary arrays (charge, ion mobility, S/N) and auxiliary arrays.
- **Root cause:** `Spectrum` exposes only m/z, intensity, raw arrays, array index (`include/mzpeak/spectrum.h:37`); `Metadata` validates kind and reads nothing (`src/metadata.cpp:36`).
- **Done when:** the metadata facets are parsed onto a spectrum-metadata model; generic per-array access keyed by `ArrayType`; **auxiliary arrays** decoded from the metadata rows (gated by `number_of_auxiliary_arrays`; mirror Rust `reader.rs:1034`) — these are a *metadata* concern, distinct from data-table arrays.
- **Validate:** spot-check ms_level/polarity/precursor and a secondary array against pyarrow.
- **Depends on:** RDR-3 (metadata path), RDR-4 (types).

---

## Milestone R6 — Robustness hardening

### RDR-11 — Fix record_count fallback  ·  P3 · G11 (Bug I)
- **Root cause:** missing count metadata becomes `0` (`src/util/parquet.cpp:129`) so the stats fallback never runs; the fallback then returns **max index, not count** (`src/util/data_arrays.cpp:209`).
- **Done when:** absent `*_count` KV falls through to a correct count (row count or max-index+1), not 0/off-by-one.

### RDR-12 — Null-check nullable columns in query evaluation  ·  P3 · G12 (Bug J)
- **Root cause:** `Value(i_)` is read on nullable columns without an `IsNull` check (`src/util/data_arrays.cpp:127`).
- **Done when:** predicate evaluation skips/handles nulls correctly. (Latent today — index column is non-nullable.)

### RDR-13 — Throw hygiene  ·  P3 · G14
- **Root cause:** `throw("not implemented")` throws a `const char*` (uncatchable as `std::exception`) (`include/mzpeak/util/encoding.h:87`); inconsistent error types.
- **Done when:** all error paths throw a `MzPeak` exception type; no bare-literal throws.

---

## Future (out of scope for the local-file reader milestone)

### RDR-14 — Remote/cloud reading  ·  P4 · G15
HTTP-range / S3 / object_store prefix. `open()` currently rejects non-local paths (`src/open.cpp:24`); the spec allows remote prefixes. Targeted "summer 2026" per README.

---

## Dependency graph (summary)
```
RDR-1 ──► RDR-9
RDR-2   (independent)
RDR-3 ──► RDR-5 ──► (writer null-marking)
RDR-3 ──► RDR-10
RDR-4 ──► RDR-6 ──► RDR-7
RDR-4 ──► RDR-10
```
Suggested first PR: **RDR-1 + RDR-2** (small, pure correctness). Biggest coverage win: **RDR-3**.
