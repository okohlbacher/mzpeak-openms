# C++ Reader vs Rust Reference — Capability Parity

A capability comparison of the OpenMS C++ reader against the Rust reference
`mzpeak_prototyping::MzPeakReader` (`hupo-mzpeak/src/reader.rs` + `src/reader/*`
+ `src/archive/sync.rs`). This complements the spec-driven gap analysis
([reader-completion-gaps.md](reader-completion-gaps.md)): it lists capabilities
the **reference implementation** exposes that the C++ reader lacks, including
gaps the spec pass did not surface (by-id access, RT queries, extraction,
encryption, caching, framework integration). New gaps become backlog items
RDR-15…RDR-25 ([reader-backlog.md](reader-backlog.md)).

## Net-new gaps (not in RDR-1…14), ranked by impact

| ID | Gap | Rust (file:line) | C++ absence | Effort |
|----|-----|------------------|-------------|--------|
| **RDR-24** | File-level mzML metadata blocks (run, instrument config, software, sample, data_processing, scan_settings, file_description) not parsed | `reader/metadata.rs:404-489` | `Index::parse_index` reads only `files[]` (`src/index.cpp:84-93`); `Metadata` is an empty stub (`src/metadata.cpp`) | M |
| **RDR-19** | No OpenMS object integration — no bridge to `MSSpectrum`/`MSExperiment` | trait impls `reader.rs:129-290` | `Spectrum` yields raw mz/intensity vectors only (`include/mzpeak/spectrum.h:40-65`, `// FIXME: level?`) | L |
| **RDR-16** | No retention-time / time-range query | `get_spectrum_index_range_for_time_range` `reader.rs:524` | no time index / RT API | M |
| **RDR-17** | No EIC / m/z-IM extraction *pipeline* (a low-level executor exists; the multi-dim RT×mz×IM×ms-level extraction does not) | `extract_signal` `reader.rs:608`, `query_peaks` `reader.rs:883` | low-level executor present (`src/spectra.cpp:31-41`, `src/util/data_arrays.cpp:235-286`); no range-extraction pipeline | L |
| **RDR-15** | No random access by native spectrum id | `get_spectrum_by_id` `reader.rs:213`, `get_spectrum_metadata_by_id` `reader.rs:1220` | `Spectra::fetch` integer-position only (`src/spectra.cpp:31`) | S-M |
| **RDR-20** | No Parquet modular decryption (AES, per-file keys) | `from_path_with_decryption` `sync.rs:971`, `with_file_decryption_properties` `sync.rs:749` | no decryption path in `Util::Parquet` | M |
| **RDR-21** | No data cache layer (LRU row-group/peak cache) | `CacheBuffer` `reader/cache.rs:266`, `set_spectrum_row_group_cache_size` `reader.rs:318` | every `fetch` re-reads + re-decodes (`src/spectra.cpp:40`) | M |
| **RDR-23** | Row-group-statistics pruning only — no page/offset-index granularity | `PageIndex` `reader/index.rs:88`, `query_pages` `reader.rs:471` | `find_row_groups(Query)` prunes at row-group granularity (`include/mzpeak/util/parquet.h:82`) — a single spectrum read pulls a whole row group | L |
| **RDR-18** | No batch / bulk read scheduling | `get_spectra_batch` `reader.rs:1311` | one-at-a-time `EnumerableProxy::fetch` | S (needs RDR-21) |
| **RDR-22** | No memory-mapped reading | `memmap` `reader.rs:1986`, `from_buf` `reader.rs:1991` | libzip/`File::read/seek` per read (`include/mzpeak/file.h`) | M |
| **RDR-25** | No `DetailLevel` (metadata-only) mode | `set_detail_level` `reader.rs:209` | always reads full arrays | S |

(Async/object-store/cloud is the existing **RDR-14**; this analysis confirms the Rust async stack in `reader/object_store_async.rs:426`.)

## Capabilities mapping to ALREADY-tracked gaps
By-id chromatogram/TIC/BPC (`reader.rs:1374,1397`) → RDR-8; wavelength by id (`reader.rs:1348`) → RDR-9; per-spectrum `SpectrumDescription` (`get_spectrum_metadata reader.rs:926`) → RDR-10; distinct peaks table (`get_spectrum_peaks_for reader.rs:824`) → RDR-3; auxiliary arrays (`reader.rs:1108`) → RDR-10; m/z delta model (`reader.rs:1135`) → RDR-5; chunked (`reader.rs:491`) → RDR-6; numpress → RDR-7; ion-mobility filter (`reader.rs:691`) → subsumed by RDR-17.

## Parity wins (C++ already has)
- **Archive abstraction** (zip + directory): `Archive`/`Zip`/`Directory` mirrors Rust's `ArchiveSource`/`DispatchArchiveSource`.
- **Index `files[]` parsing**: both parse `mzpeak_index.json` (`src/index.cpp:60-94`). (C++ does not yet route by `data_kind` — that's RDR-3/13.)
- **Raw signal access** (narrow): C++ decodes point m/z + intensity (`src/spectrum.cpp:36-42`); note `raw_encoded_arrays()` returns still-*encoded* Arrow material, not a fully typed `BinaryArrayMap` like Rust.
- **Typed predicate *expression* API**: C++ `Query` (`Predicate<T>`, `&&`/`||`/`!`, point + range eval, `query.h:30-202`) is a *more explicit public expression API* than the Rust reader surfaces (Rust uses internal Arrow `RowFilter` closures). The model is a C++ strength; only the executor wiring (RDR-17) is missing. (RDR-12 tracks its nullable-eval correctness.)
- **Index-ordered iteration**: `EnumerableProxy<Spectrum>` partially matches Rust's `Iterator` (minus reset / detail-level / lazy-metadata).

## Structural note
The Rust `MzPeakSpectrumFacet` trait (`reader.rs:1594`) generalizes metadata/data/cache access across mass, wavelength and chromatogram modalities. The C++ side has only the single `Spectra` facet, so wavelength (RDR-9) and chromatogram (RDR-8) are **structurally** absent, not merely unwired — worth considering a facet abstraction when implementing them.

## Verification
This analysis was adversarially verified by codex against both codebases. All net-new gaps (RDR-15…RDR-25) confirmed distinct from RDR-1…14; Rust evidence spot-checked (by-id, RT range, extract_signal/query_peaks, LRU cache, decryption, file-level KV metadata all accurate). Corrections folded in: RDR-17 (a low-level executor exists — the *extraction pipeline* is what's missing), RDR-21 (a tiny iterator-deref cache exists but is not a data cache), RDR-22 (in-memory + mmap, not mmap alone), RDR-23 (page-index impl citations), RDR-8 (Rust also *synthesizes* TIC/BPC from spectrum metadata), and the RDR-3 `SignalLoadingPreference` note.
