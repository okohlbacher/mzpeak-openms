# Implementing an mzPeak Writer in OpenMS C++ — Research & Plan

Synthesis of four source-grounded investigations (spec, Rust reference writer, C++ reader scaffolding, dependencies). Format version targeted: **0.9.0**.

## Bottom line
- **Feasible with the existing dependency stack.** Arrow/Parquet 24, Boost.JSON, libzip already linked and all expose the needed write APIs. The **only** new third-party code is one vendored file: **ms-numpress** (`MSNumpress.cpp/.hpp`, BSD-3/Apache-2.0; OpenMS already ships a copy). No linalg dep needed — the delta model is a 3-parameter weighted fit.
- **~40% of the reader scaffolding is reusable** (schema enums, type-dispatch, type tag maps), ~35% needs extension (ArrayIndex/File need programmatic construction + JSON emit; zip/archive/file need write counterparts), ~25% is new (Parquet writer wrapper, encode pipeline, writable spectrum model, `create()` API).
- **Do NOT chase byte-identical Parquet output.** parquet-rs vs parquet-cpp differ in `created_by`, ZSTD streams, page layout. Validate with a **semantic round-trip** (decode both, compare arrays/metadata within tolerance). One targeted byte-exact test for numpress buffers only.
- **Effort:** a conformant MVP (point layout, profile+centroid, no transforms) is a few weeks; full feature parity (chunked, null-marking + WLS delta model, numpress, aux arrays, chromatograms, wavelength, streaming) is a multi-month effort dominated by (1) building the deep nested Arrow schema + field metadata and (2) the chunked/null-marking/delta-model data pipeline.

## 1. What a conforming writer MUST produce (spec)
- **Container:** uncompressed (STORE) ZIP, OR unpacked directory, OR remote prefix. ZIP members MUST be `compress_type=0` (Parquet is already ZSTD-internally compressed; readers reject deflate).
- **Members** (resolved by entity_type+data_kind, not name): `mzpeak_index.json` (req), `spectra_metadata.parquet` + `spectra_data.parquet` (req when spectra present), `spectra_peaks.parquet` (centroids), `chromatograms_{metadata,data}.parquet`, `wavelength_spectra_*` (if present).
- **`mzpeak_index.json`:** `{files[], metadata{}}`; `metadata.version` REQUIRED; SHOULD carry `cv_list`, `file_description` (contents+source_files), `run` (id, default_instrument_id, default_data_processing_id, default_source_file_id), instrument/software/data-processing lists. Reference writer also duplicates these into each metadata Parquet's footer KV.
- **Data tables:** top-level struct node named `point` (point layout) or `chunk` (chunked) — this name IS the layout signal. Entity index column (`spectrum_index` uint64) MUST be first. Primary arrays get short names (`mz`,`intensity`), `sorting_rank:0` on the main axis, `buffer_priority:primary`.
- **Array Index:** JSON in the data file's Parquet footer KV under `<entity>_array_index` (per `schema/array_index.json`); entries: array_name, buffer_format, context, path, data_type, array_type, unit (+ optional buffer_priority, sorting_rank, transform, data_processing_id). Reference writer also replicates as per-leaf Arrow field metadata.
- **Layout rules:** POINT stores values as-is (NO obscuring transform — would break the page index); only zero-run stripping + null-marking allowed. CHUNKED required for any transform (delta MS:1003089 / numpress linear MS:1002312 / SLOF MS:1002314); columns `<axis>_chunk_start/_chunk_end/_chunk_values` + `chunk_encoding`, non-overlapping ascending chunks.
- **Null-marking:** flanking zero-intensity points → null m/z + null intensity; fit WLS `δmz ~ β0+β1·mz+β2·mz²`, store betas in metadata `mz_delta_model` (list<f64>); coordinate array `transform=MS:1003901`, intensity `transform=MS:1003902`. Incompatible with numpress.
- **Invariants (MUST):** write the Parquet **page index**; index columns 0-based incrementing by 1; ranked arrays re-sorted if unsorted (permute all parallel arrays); `number_of_data_points`/`number_of_peaks` consistent with actual rows; use `large_list<u8>` (not binary) for byte columns.
- **Metadata facets** (packed parallel structs): spectrum / scan / precursor / selected_ion (+ activation), with required CV sub-terms for MS2 (MS:1000792 isolation window, MS:1000044 dissociation, MS:1000455 ion selection). Column-name inflection `${CV}_${ACCESSION}_${name}[_unit_${UNIT}]`; repeated params go in `parameters` lists.

## 2. Reference architecture to mirror (Rust `hupo-mzpeak`)
Pipeline per spectrum: route (mass vs wavelength) → pick profile→data table / centroid→peaks table (split) → check/enforce m/z sortedness → (profile+null_zeros) fit delta model → point path (zero-run strip + null-mark) or chunked path (segment → per-axis encode: basic/delta/numpress → nested StructArray) → buffer → flush row groups. Spectrum-**data** member is streamed eagerly; all other members buffered and written at finish (a STORE-zip has one active entry at a time). Public API: a `builder()` with knobs (compression ZSTD3, chunked strategy, null_zeros, shuffle_mz, buffer_size, row-group/page sizes, array overrides, peak-type registration, per-member encryption), then `write_spectrum`/`write_chromatogram`, `finish()`.

Key algorithms to reimplement exactly (or readers reconstruct different values):
- **WLS delta model** (`filter.rs:107-216`): weights `sqrt(ln(I+1))`; deltas filtered to ≤1.0; design matrix `[1,mz,mz²]`; weighted QR solve; compare vs constant (median-below-median) model, pick by `e_const < e_reg/10`. Store betas.
- **Null-marking** (`is_zero_pair_mask` + `null_delta_encode`): mask values in runs of ≥2 zeros; paired nulls; singleton-at-boundary convention.
- **Zero-run stripping**: collapse runs of zeros to first+last, applied to all parallel arrays.
- **Chunk segmentation** (`null_chunk_every_k`): null-aware, width-based (default 50 m/z), avoid length-1 chunks.
- **Numpress**: linear is an axis codec (start point INCLUDED in chunk values); SLOF/PIC are opaque secondary transforms (`<name>_numpress_slof_bytes`, large_list<u8>).
- **Parquet props**: SortingColumn on index, DELTA_BINARY_PACKED on index cols, BYTE_STREAM_SPLIT on shuffled m/z, bloom filter on index + spectrum.id, page statistics, PARQUET_2_0, `store_schema()` MANDATORY (so large_list/large_string/nested structs round-trip via the ARROW:schema blob).

## 3. What the C++ codebase already gives us (reuse map)
- **REUSE as-is:** `DataKind`, `EntityType`, `BufferFormat`, `PSI::DataType` (+ `dispatch<>` template — invertible, reuse for encode), `PSI::ArrayType`; `parquet_types.h` tag maps (`psi_to_parquet_tag`/`parquet_to_physical_tag` — symmetric).
- **EXTEND:** `Schema::File` (add programmatic ctor + JSON emit), `ArrayIndex` + `ArrayIndex::Column` (add `add_column()` + `to_json()`; `prefix_` hardcoded "point" must become point/chunk-aware), `Archive`/`Directory`/`File` (add write interface), `Zip` (ZIP_CREATE + write members).
- **BUILD NEW:** `ParquetWriter` wrapper (`parquet::arrow::FileWriter` + WriterProperties + store_schema + field/file KV), `Encoding<T>::encode_point/encode_chunked` (inverse of decode), index JSON serializer (boost::json), writable spectrum model (current `Spectrum` has no public ctor; `Spectra` is read-only) → `SpectrumBuilder`/`SpectraWriter`, top-level `create()`/`MzPeakWriter`.
- **Build system:** all four deps already provide write APIs; no new meson deps except vendoring ms-numpress.

## 4. Dependencies (feasibility)
| Capability | Approach | New dep? | Byte-compat risk |
|---|---|---|---|
| Parquet write (nested structs, field+file KV, store_schema, stats/page-index, sorting, encodings) | `parquet::arrow::FileWriter` + `WriterProperties::Builder` + `ArrowWriterProperties::store_schema()` | No | HIGH (semantic compare only) |
| Zip STORE container, in-memory members | `zip_open(ZIP_CREATE)`+`zip_source_buffer`+`zip_file_add`+`ZIP_CM_STORE` | No | LOW |
| MS-Numpress linear/slof/pic | vendor `MSNumpress.cpp/.hpp` (1 TU) | Vendor 1 file | MED (match numpress-rs fixed-point+rounding; add byte test) |
| WLS degree-2 delta fit | hand-rolled 3-param weighted QR / normal equations | No | MED (β differ in last ULPs; design-tolerant) |
| Serialize index JSON | `boost::json::serialize` (+ optional 2-space pretty to mirror serde) | No | HIGH for bytes / none for semantics |

## 5. Hardest parts (ranked)
1. WLS delta model + selection (numeric parity, ULP sensitivity).
2. Null tokenizer / fill round-trip (encoder must produce exactly the null layout the decoder expects).
3. Chunk segmentation (off-by-one changes chunk boundaries).
4. Nested StructArray construction (large_list<struct<...>> + chunk_encoding column + field metadata, exact field order via BufferName::Ord).
5. BufferName naming/ordering/metadata contract (interop-critical).
6. Parquet WriterProperties parity (per-column encodings, bloom, sorting, row-group sizing).
Moderate: zero-run strip, delta encode, numpress wiring, streaming-vs-buffered flush sequencing, peaks-apart tempfile→zip. Trivial: constants, index JSON, STORE-zip mechanics, builder setters, CURIE tables.

## 6. Proposed phased roadmap
- **Phase 0 — Writer foundations (extend scaffolding).** Programmatic `ArrayIndex`/`Column`/`File` + JSON emit; `ParquetWriter` wrapper (store_schema, field+file KV, page index, stats, sorting col, ZSTD); zip STORE writer; index JSON serializer; `Encoding::encode_point`. Validate by writing then reading back with the existing reader.
- **Phase 1 — MVP conformant writer (point layout).** Writable spectrum model + `MzPeakWriter`/`create()`. Emit `spectra_data` (point), `spectra_metadata` (spectrum/scan/precursor/selected_ion facets with required MS2 terms), `spectra_peaks` for centroids, minimal `mzpeak_index.json`, TIC chromatogram. Target: produce a file the Rust reader AND the C++ reader read back semantically equal to `small.unpacked.mzpeak`.
- **Phase 2 — Lossless compression.** Zero-run stripping; null-marking + WLS delta model (`MS:1003901/1003902`, `mz_delta_model`). This pairs naturally with finishing the **reader's** delta-model reconstruction (currently the open Bug C-null) — implement encode+decode together with a round-trip test.
- **Phase 3 — Chunked layout + transforms.** `chunk` schema, segmentation, delta (`MS:1003089`), numpress linear (`MS:1002312`) + SLOF (`MS:1002314`) via vendored ms-numpress; BYTE_STREAM_SPLIT m/z shuffle. (Also needs the reader's `decode_chunked`, currently stubbed.)
- **Phase 4 — Breadth + streaming.** Auxiliary arrays, wavelength spectra, ion mobility, full chromatograms; streaming writer interface (eager data member + buffered rest); builder config knobs. (Cloud/remote-prefix is a separate reader-side track.)

## 7. Validation strategy
- **Semantic round-trip harness:** write with C++ → read with C++ reader AND Rust reader → compare m/z/intensity/metadata within float tolerance; write with Rust → read with C++ → compare. Use the existing test files (`small.unpacked/.chunked/.numpress.mzpeak`) as oracles.
- **Schema conformance:** validate emitted `mzpeak_index.json` + array-index against `spec/schema/*.json` and `hupo-mzpeak/schema/*.json`.
- **One byte-exact test:** numpress buffers vs numpress-rs (catch fixed-point/rounding divergence).
- **Reuse the project's adversarial review pattern (codex+vibe) per phase, and add regression tests like the reader PRs.**

## 8. Risks & open questions
- Byte-identity is NOT a realistic goal for Parquet/JSON; set semantic round-trip as the contract up front (avoids wasted effort).
- WLS β reproduction: if exact βs are ever required, replicate nalgebra's weighted-QR path, not normal equations.
- Spec under-specified points relevant to writing: field-metadata duplication requirement level; `*_count` footer KV (emitted by ref, undocumented); row-group/page tuning (open); `cv_list`/`file_description` conformance level (prose softens to SHOULD); `data_processing_ref` type doc inconsistency (string vs int); `scan_settings_list` has no standalone schema.
- The writer work has natural synergy with two already-identified open reader gaps: **Bug C-null** (delta-model reconstruction) and the **stubbed `decode_chunked`** — doing encode+decode together is more efficient and self-checking.
- Format is v0.9.0 / "living spec" — API stability targeted end of summer 2026; expect churn.

## 9. Corrections from adversarial review (codex)
Codex audited this plan against the repos and flagged the following; all accepted:
1. **Member requirements too broad.** `spectra_data.parquet` is NOT required for centroid-only data (those go to `spectra_peaks.parquet`); spectra are "up to three files" (spec `spectra.md`). Fix the "required when spectra present" wording — required is metadata + at least one of data/peaks.
2. **`_count` footer KVs are effectively REQUIRED for C++ interop, not "open/undocumented."** The C++ reader's `record_count()` (`data_arrays.cpp:209`) relies on `<entity>_count` or row-group stats; absent both it throws, and the stats fallback returns max-index (off-by-one). The writer MUST emit `spectrum_count` etc.
3. **WLS: drop "normal equations"; use QR only** (spec calls QR more stable, `signal-data.md`). Parity trap to document: `select_delta_model` fits on `mz_array[1..]` but computes regression MSE against full `mz_array` (`filter.rs:194`) — reproduce or explicitly reject for reference parity.
4. **Numpress linear: start point is "included in the encoded BYTES," not the visible chunk-values.** The Rust writer stores `mz_chunk_values` as null and writes the buffer to the extra `*_numpress_linear_bytes` large_list<u8> column (`chunk_series.rs:146,711`).
5. **Null-marking + numpress incompatibility has NO reference builder-level guard** (`null_zeros` and `NumpressLinear` are independently configurable, `builder.rs:144`). Add it as a hard writer validation invariant.
6. **`store_schema()` is mandatory for C++ interop, not a spec-level requirement** (Arrow C++ defaults it false; parquet-rs writes `ARROW:schema` by default). State it as an interop requirement.
7. **"~40% reuse" is optimistic** — reuse is mostly enums/parsing/tests; the writer model, Parquet writer, archive writer, and chunk/null decode are essentially new. Replace the percentage with a file-by-file reuse map.
8. **The C++ reader is a weak validation oracle today**: `Index::spectra()` resolves by literal filename (Bug B), no public peaks/chromatogram/wavelength APIs. So **use the Rust reader as the Phase-0/1 oracle**; Phase 1 cannot claim C++-reader semantic equality for centroids/TIC/chunked/null data until reader gaps (Bug B, C-null, decode_chunked) are closed.
9. **JSON-schema validation is insufficient** — `array_index.json` only requires `prefix` (doesn't even declare `entries`). Add custom conformance checks: entries, paths, transforms, buffer_formats, exactly-once chunk support columns, sorted ranked arrays, STORE zip, page index, field metadata, `_count` KVs.
10. **`scan_settings_list.json` DOES exist in `spec/schema/`** (my "no standalone schema" was stale — it's only missing from `hupo-mzpeak/schema/`).
11. **Dependency floor:** `openms-mzpeak/meson.build` requires Arrow/Parquet `>=23.0.0` (installed 24). Either raise the minimum or verify the writer APIs (page index, bloom, sorting cols, store_schema) against Arrow 23.
12. **Semantic round-trip is necessary but not sufficient** — shared writer/reader bugs round-trip clean. Keep the numpress byte-exact test AND structural checks (ZIP method, ARROW:schema, page index/stats, field metadata, array-index JSON, `_count` KVs, canonical schema paths).
