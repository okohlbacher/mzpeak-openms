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
- **Done when:** row groups with missing stats are **conservatively included** (or resolved via the page index) rather than pruned. **[DONE]** — `query.cpp` range eval keeps stats-less groups.
- **Validate:** unit test with a predicate over a file/row-group lacking stats; assert all matching rows are returned.

### RDR-26 — Read multiple spectrum tables from one zip archive  ·  P1
- **Symptom:** reading **two** spectrum tables (`spectra_data` + `spectra_peaks`) from a **C++-written** `.mzpeak` zip fails with `IOError: unable to seek`. The directory form works; multi-table **Rust-written** zips (e.g. small.mzpeak: 48/48) also read fine — so it is specific to concurrent `zip_fseek` on the members of a C++-produced archive.
- **Root cause:** the zip reader streams members with `zip_fseek` (`src/zip.cpp` `ZipFile_::seek`); libzip's stored-member seek does not support several members of one archive open at once (which RDR-3 now does). A first attempt to buffer each member in memory caused test timeouts and was reverted — needs careful re-implementation (lazy/windowed buffer, or a per-member fresh archive handle).
- **Done when:** `write_spectra_archive` output with both profile + centroid spectra round-trips through `MzPeak::open(zip)`.
- **Note:** exposed by RDR-3 (two-table reads) + the centroid/peaks writer split. The writer output IS valid (Rust reads it); this is a reader-side zip limitation.

### RDR-27 — Cross-impl centroid read needs spectrum representation  ·  P2
- **Symptom:** the Rust reference `get_spectrum` does not auto-load a C++-written **centroid** spectrum's peaks (panics `NotFound(MZArray)`), though the peaks table is valid (the C++ reader reads it). It loads profile spectra fine.
- **Root cause:** the Rust reader's peak loading is gated by `SignalLoadingPreference` / `MS_1000525_spectrum_representation`, which the C++ writer's minimal metadata table omits (it sets only the counts).
- **Done when:** the writer emits `MS_1000525_spectrum_representation` (centroid `MS:1000127` / profile `MS:1000128`) so the Rust reader loads peaks for centroid spectra (extends the Phase-1b metadata writer).

---

## Milestone R2 — Spectrum coverage (the big win)

### RDR-3 — Resolve spectrum sources by entity_type/data_kind, read the peaks table  ·  P1 · G13+G1+G2 (Bug B, C4)
- **Symptom:** 34/48 centroid spectra in `small.mzpeak` are unserved — 29 return empty m/z, 5 throw `index not in column map` (`include/mzpeak/util/encoding.h:100`).
- **Root cause:** `Index::spectra()` hard-codes `"spectra_data.parquet"` (`src/index.cpp:97`) and `Spectra::fetch` queries only it (`src/spectra.cpp:31`); the `spectra_peaks.parquet` table is never read; absent rows throw instead of returning empty.
- **Done when:** spectrum sources are resolved from the index `files[]` by `entity_type==spectrum` + `data_kind` (the `EntityType`/`DataKind` enums already exist); a spectrum's **representation is metadata-driven** — read `MS_1000525_spectrum_representation` (profile `MS:1000128` / centroid `MS:1000127`) and the `MS_1003060_number_of_data_points` / `MS_1003059_number_of_peaks` counts, pulling profile from `spectra_data` and centroid from `spectra_peaks` (a spectrum may have both); absent rows return empty, never throw. **Do not silently merge tables.**
- **Validate:** `small.mzpeak` → all 48 spectra return non-empty m/z+intensity matching pyarrow ground truth from both tables; 0 throws.
- **Depends on:** independent. Unblocks the largest coverage gain.
- **Reference parity note:** the Rust reader exposes a profile/centroid loading policy (`SignalLoadingPreference`, `hupo-mzpeak/src/reader.rs:86-104,1523-1532`) and supports mixed profile+peak loading per spectrum (`reader.rs:1239-1305`); consider modelling the same preference rather than always returning one representation.

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

### RDR-8 — Chromatogram read API (incl. synthesized TIC/BPC)  ·  P2 · G7
- **Symptom:** chromatograms (every test file has them; has_uv: 738 points) are entirely inaccessible.
- **Root cause:** `Index` exposes only `spectra()` (`include/mzpeak/index.h:39`).
- **Done when:** `Chromatogram`/`Chromatograms` types + `Index::chromatograms()` read `chromatograms_{metadata,data}.parquet` (point + chunked). Also provide the **synthesized TIC/BPC** the Rust reader builds from per-spectrum metadata when no stored chromatogram exists (`hupo-mzpeak/src/reader.rs:1395-1452,1455-1508`) — this needs the spectrum metadata of RDR-10.
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
HTTP-range / S3 / object_store prefix. `open()` currently rejects non-local paths (`src/open.cpp:24`); the spec allows remote prefixes. Targeted "summer 2026" per README. The Rust async stack is `src/reader/object_store_async.rs:426`.

---

## Milestone R7 — Reference (Rust) capability parity
Capability gaps vs the Rust `MzPeakReader` surfaced by [reader-rust-parity.md](reader-rust-parity.md) — features the reference reader exposes that the C++ reader lacks (beyond the spec/correctness work above).

### RDR-24 — Parse & expose file-level metadata blocks + format version  ·  P2
- **Symptom:** the index `metadata{}` block is **never read** — `Index::parse_index` reads only the JSON `files[]` (`src/index.cpp:84-93`); `Metadata` validates the kind but exposes **no accessors** (`src/metadata.cpp`). Two parts are missing:
  - **`metadata.version`** — the mzPeak format version (`"0.9.0"`). The Rust writer writes it (`MZPEAK_VERSION` / `VERSION_KEY`, `FileIndex::add_version`, commit *"JSON metadata in the index"* 29e59b2); the reader should **read and validate** it (reject/warn on an incompatible major version) instead of ignoring it. *(Our own C++ writer already emits `version`; the C++ reader does not check it.)*
  - **The run-level blocks** — `run`, `instrument_configuration_list`, `software_list`, `sample_list`, `data_processing_method_list`, `scan_settings_list`, `file_description`, `cv_list` — written into `metadata{}` (and Parquet KV) and decoded by Rust (`hupo-mzpeak/src/reader/metadata.rs:404-489`); the reference `small.dir/mzpeak_index.json` carries all seven.
- **Done when:** `metadata{}` (+ Parquet KV) parses into typed structs with accessors, and the format version is surfaced and version-checked. Prerequisite for OpenMS `ExperimentalSettings` mapping (RDR-19). *Distinct from RDR-10 (per-spectrum metadata).*
- **Writer counterpart:** the C++ writer currently emits only `metadata.version` (not the run-level blocks) — writing those blocks is the writer-side dual of this item (tracked in [writer-implementation-research.md](writer-implementation-research.md) §1).

### RDR-15 — Random access by native spectrum id  ·  P2
- **Symptom:** spectra are reachable only by integer position (`Spectra::fetch`, `src/spectra.cpp:31`); external tools reference spectra by native id. Rust: `get_spectrum_by_id` (`reader.rs:213`), `get_spectrum_metadata_by_id` (`reader.rs:1220`) via an id→index offset index.
- **Done when:** an id→index map is built from the metadata `id` column and `Index`/`Spectra` expose by-id lookup. **Depends on:** RDR-10.

### RDR-16 — Retention-time / time-range query  ·  P2
- **Symptom:** no way to select spectra by RT. Rust: `get_spectrum_index_range_for_time_range` (`reader.rs:524`) via a time page index.
- **Done when:** a time index (from the metadata `time` column) backs an RT-range → spectrum-index-range API. **Depends on:** RDR-10, RDR-23.

### RDR-17 — EIC / m/z–ion-mobility signal extraction pipeline  ·  P2
- **Symptom:** a *low-level* executor exists — `Spectra::fetch` builds a `Query` and `DataArrays::read_arrays` prunes row groups + slices batches (`src/spectra.cpp:31-41`, `src/util/data_arrays.cpp:235-286`). What is missing is the **multi-dimensional extraction pipeline** the Rust reader provides: stream the data/peaks tables filtered by (time × m/z × ion mobility × ms-level) to build extracted-ion chromatograms / targeted point sets. Rust: `extract_signal` (`reader.rs:608`), `query_peaks` (`reader.rs:883`), with split-thread parallelism (`reader.rs:639`).
- **Done when:** a range-extraction API returns points/peaks selected across those dimensions (EIC basis). **Depends on:** RDR-16, RDR-3, RDR-6.

### RDR-18 — Batch / bulk read scheduling  ·  P3
- **Symptom:** only single `fetch`; reading a scattered subset re-reads per spectrum. Rust: `get_spectra_batch` (`reader.rs:1311`) sorts indices and reads efficiently.
- **Done when:** a batch read sorts indices and shares row-group reads. **Depends on:** RDR-21.

### RDR-19 — OpenMS object integration  ·  P2
- **Symptom:** the reader yields a bespoke `MzPeak::Spectrum` of raw mz/intensity vectors (`include/mzpeak/spectrum.h:40-65`, `// FIXME: level?`); there is no bridge to `OpenMS::MSSpectrum`/`MSExperiment`. Rust implements the full mzdata `SpectrumSource`/`MSDataFileMetadata`/`ChromatogramSource` stack (`reader.rs:129-290`).
- **Done when:** an adapter yields OpenMS spectra/experiment objects so downstream OpenMS tools consume mzPeak. **Depends on:** RDR-10, RDR-24, RDR-3. *This is arguably the point of an OpenMS reader.*

### RDR-20 — Parquet modular decryption  ·  P3
- **Symptom:** AES-encrypted Parquet members are unreadable; no decryption path in `Util::Parquet`. Rust: `from_path_with_decryption` (`hupo-mzpeak/src/archive/sync.rs:971`), `with_file_decryption_properties` (`sync.rs:749`).
- **Done when:** `FileDecryptionProperties` (Arrow C++) is plumbed through with a key API.

### RDR-21 — Row-group / peak data cache  ·  P3
- **Symptom:** every `fetch` re-reads and re-decodes (`src/spectra.cpp:40`); region-local access is O(n) re-reads. (There is a tiny iterator-dereference cache, `include/mzpeak/util/enumerable_proxy.h:95-108`, but it is not a decoded row-group/peak cache.) Rust: LRU `CacheBuffer` (`hupo-mzpeak/src/reader/cache.rs:266-388`), tunable (`reader.rs:318`).
- **Done when:** an LRU cache of decoded row groups is reused across reads. Perf only.

### RDR-23 — Page/offset-index random access (vs row-group pruning)  ·  P3
- **Symptom:** random access prunes only at row-group granularity (`find_row_groups`, `include/mzpeak/util/parquet.h:82`), so a single-spectrum read pulls an entire row group. Rust uses the Parquet page/offset index (impl `hupo-mzpeak/src/reader/index.rs:88,160-223,282-349,721-744`; consumer `reader.rs:471-499`).
- **Done when:** page-index-driven selection reads only the relevant pages. **Related:** RDR-2 (row-group pruning correctness).

### RDR-22 — In-memory / memory-mapped archive reading  ·  P3
- **Symptom:** reads always go through libzip/`File::read/seek` (`include/mzpeak/file.h`); there is no way to read from an mmap or an in-memory buffer. Rust: `memmap` and `from_buf` (`reader.rs:1986-1994`, `archive/sync.rs:1154-1186`).
- **Done when:** the archive abstraction supports a memory-backed source (mmap'd unpacked files and/or an in-memory byte buffer), avoiding copies on large files.

### RDR-25 — Detail-level (metadata-only) mode  ·  P4
- **Symptom:** the reader always loads full arrays. Rust: `set_detail_level` (`reader.rs:209`) for metadata-only scans.
- **Done when:** a detail-level flag skips array decode when only metadata is needed.

---

## Dependency graph (summary)
```
RDR-1 ──► RDR-9
RDR-2   (independent)
RDR-3 ──► RDR-5 ──► (writer null-marking)
RDR-3 ──► RDR-10 ──► RDR-15 (by-id), RDR-16 (RT), RDR-19 (OpenMS), RDR-24 (file metadata)
RDR-4 ──► RDR-6 ──► RDR-7
RDR-16 + RDR-3 + RDR-6 ──► RDR-17 (EIC)
RDR-21 ──► RDR-18 (batch)
RDR-23 ──► RDR-16/RDR-17 (finer selection)
RDR-10 + RDR-24 + RDR-3 ──► RDR-19 (OpenMS integration)
```
Suggested first PR: **RDR-1 + RDR-2** (small, pure correctness). Biggest coverage win: **RDR-3** (peaks table). Highest reference-parity value once correctness lands: **RDR-10 → RDR-24 → RDR-19** (per-spectrum + file metadata → OpenMS object integration), which is what makes mzPeak consumable by OpenMS tools.

## Backlog at a glance
- **R1 quick correctness:** RDR-1, RDR-2
- **R2 spectrum coverage:** RDR-3, RDR-4
- **R3 lossless:** RDR-5
- **R4 chunked/numpress:** RDR-6, RDR-7
- **R5 entity/metadata breadth:** RDR-8, RDR-9, RDR-10
- **R6 hardening:** RDR-11, RDR-12, RDR-13
- **R7 Rust capability parity:** RDR-15…RDR-25 (by-id, RT, EIC, batch, OpenMS integration, decryption, cache, page-index, mmap, detail-level, file metadata)
- **Future:** RDR-14 (remote/cloud)

Total: 25 tracked reader gaps.
