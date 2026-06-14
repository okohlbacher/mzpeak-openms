# mzPeak C++ — Remaining-Work Roadmap (post-2026-06-14)

Consolidates every open backlog/gap after the reader-feature, e2e-harness, and
OpenMS-style-conformance rounds. Status verified against the code on
`writer_test` (not just the backlog doc, which under-marked several done items).

## Implemented this round (2026-06-14, all on `writer_test`, suite 27/27)
- **Phase 0 · DOC-1** ✅ backlog reconciled + `THIRD_PARTY.md` (commit 674f09b).
- **Phase 1 · RDR-4a** ✅ unsigned `UInt64` entity-index typing through
  DataType/parquet-tags/Query/stats/record_count; >`INT64_MAX` test; cross-impl
  PASS (commit fbe4b58).
- **Phase 2 · RDR-28a** ✅ chunked-layout chromatograms read (small.chunked +
  small.numpress) via the existing `decode_chunked`; ground-truthed (13248fa).
- **Phase 3 · RDR-25** ✅ detail-level / metadata-only mode
  (`Index::spectra(DetailLevel)`), no array decode (2ffd823).
- **RDR-22** ✅ `MzPeak::open_buffer(bytes)` — read a `.mzpeak` ZIP from an
  in-memory byte buffer via a `ZipBuffer` archive over `zip_source_buffer`
  (8f030a5). (The reviewers' "large" worry didn't materialize — the RDR-26
  per-member pattern maps cleanly onto a buffer source.)
- **WRT-2** ✅ writer emits run-level `metadata{}` blocks via a typed
  `RunMetadata::to_json()` serializer + writer overloads; round-trips through
  `Index::metadata()` (9780a4a).
- **RDR-28b** (chunked wavelength) **deferred**: no bundled fixture.
- **RDR-4b** (large_list/large_string/large_list<u8> typing) **deferred**: the
  chunked decoder already casts those directly; not blocking.

Remaining open — none standalone-feasible-and-valuable; each is blocked,
no-functional-benefit, or multi-month:
- **RDR-19** OpenMS integration — `libOpenMS` not built locally (only deps);
  finishing the OpenMS build is a long compile, and the adapter likely belongs
  in the OpenMS tree (see the Phase-5 note). Highest value but a deliberate,
  separate effort.
- **RDR-20** Parquet AES decryption — no encrypted fixtures/keys.
- **RDR-21** LRU cache / **RDR-23** page-index — perf only; no behavioral
  fixture and weak validation; real but modest, and RDR-21 touches the
  data_arrays/parquet paths just changed by RDR-4a.
- **RDR-4b** large_list/large_string/large_list<u8> typing — no functional
  benefit today (the chunked decoder already casts those directly).
- **RDR-28b** chunked wavelength — no fixture.
- **WRT-1** writer chunked/numpress emit — a separate multi-month project.
- **RDR-14** remote/cloud — P4 future.

## Done (for context — do not re-do)
Reader: RDR-1/2/3/5/6/7/8/9/10/11/12/13/15/16/17/18/24/26/27/29. Point layout
(profile+centroid, null-marked m/z, multi-intensity-column coalescing), chunked
**spectra** (basic/delta/numpress-linear+SLOF), chromatogram + wavelength **point**
read, per-spectrum + file-level metadata, by-id / RT-range / EIC / batch query,
zip + directory containers. Writer: point-layout MVP (profile→data, centroid→peaks,
metadata table). Full e2e harness (`test/e2e_test.cpp` + `scripts/e2e.sh`, T1–T5).

## Open items inventory

| Id | Title | Pri | Feasible here? | Touches |
|----|-------|-----|----------------|---------|
| RDR-28 | Chunked-layout **chromatograms + wavelength** | P3 | ✅ yes (fixtures exist) | aux readers + `encoding.h` |
| RDR-4  | Type system: `UInt32/64`, `large_list`, `large_string`, `large_list<u8>` | P1 | ✅ yes (bounded) | `data_type.h`, `parquet_types.h`, `query`, `array_index`, `data_arrays`, `parquet` |
| RDR-22 | In-memory / mmap archive source | P3 | ✅ yes | `open`, `archive`, `file`, `zip`, `directory` |
| RDR-25 | Detail-level (metadata-only) mode | P4 | ✅ yes | `spectra`, `spectrum`, `index` |
| RDR-21 | Row-group / peak-data LRU cache | P3 | ✅ yes (perf; no correctness fixture) | `data_arrays`, `parquet` |
| RDR-23 | Page/offset-index random access | P3 | ◐ partial (complex, perf) | `parquet`, `data_arrays` |
| WRT-1  | Writer **chunked/delta/numpress** + null-marking emit | P2 | ◐ thin slice only (full = multi-month) | `parquet_writer`, `encoding.h`, `writer` |
| WRT-2  | Writer run-level `metadata{}` blocks emit (RDR-24 dual) | P3 | ✅ yes (bounded) | `json_writer`, `writer` |
| RDR-19 | OpenMS `MSSpectrum`/`MSExperiment` integration | P2 | ❌ blocked (needs OpenMS build) | new adapter |
| RDR-20 | Parquet modular (AES) decryption | P3 | ❌ blocked (no encrypted fixtures/keys) | `parquet`, `arrow` |
| RDR-14 | Remote / cloud (HTTP-range / S3) reading | P4 | ❌ future (network infra) | `open`, `archive` |
| DOC-1  | Reconcile `reader-backlog.md` DONE markers | — | ✅ trivial | docs |

## Adversarial review outcome (Codex + Vibe, 2026-06-14)
Both reviewers independently returned the same MUST-FIXes, which **revised this
plan from "parallel-first" to "sequence the foundation, then parallelize"**:
1. **RDR-4 (unsigned index) must come FIRST.** Every entity-index read synthesizes
   `Int64` (`array_index.cpp:35`) and every fetch builds `Query::Predicate<Int64>`
   (`spectra.cpp`, `chromatograms.cpp`, `wavelength_spectra.cpp`). RDR-28/RDR-25
   would be re-patched once `UInt64` lands → do the narrow unsigned-index
   foundation before the reader items.
2. **Split RDR-28.** Chunked *chromatograms* exist in `small.chunked` +
   `small.numpress` (verified `top-node=chunk`) → validatable now (28a). Chunked
   *wavelength* has **no bundled fixture** (has_uv wavelength is `point`) and the
   chunk main-axis decoder hard-casts to `DoubleArray` while wavelength is
   `Float32` → **defer 28b** until a fixture exists.
3. **RDR-25 is not just "skip decode."** `Spectrum` decodes in its ctor and
   `Spectra::fetch` uses decoded-array emptiness to pick data-vs-peaks; detail
   mode needs a source decision from metadata `number_of_data_points/peaks`.
4. **RDR-22 buffer-backed *zip* is large** (`zip.cpp` reopens the archive per
   member); only buffer-backed *directory* is small. Split accordingly.
5. **Do not run 28+22+25 together.** Sequence; parallelize only the later
   genuinely-disjoint pair.

The phasing below reflects these corrections.

## Phasing

### Phase 0 — Housekeeping (DONE inline)
- **DOC-1** ✅ reconciled `reader-backlog.md` DONE markers (RDR-3/5/6/7/8/9/26);
  **THIRD_PARTY.md** added for vendored ms-numpress (Apache-2.0).

### Phase 1 — RDR-4a foundation (SEQUENCED FIRST, isolated)
Narrow unsigned-index typing — the critical path everything else rebases on.
- Add `UInt32`/`UInt64` to `PSI::DataType` + `data_type_traits` + the parquet
  type-tag maps (`parquet_types.h`).
- Route the entity-index column through `UInt64` (the files store it unsigned):
  `array_index.cpp` index `data_type`, the `Query::Predicate<…>` built in
  `spectra.cpp`/`chromatograms.cpp`/`wavelength_spectra.cpp`, the `Query` value
  variant + range/stats eval (`query.h`), and `record_count()`'s stats cast
  (`data_arrays.cpp:226`).
- Validate: **all fixtures still read** (full suite + e2e + cross-impl matrix);
  add a synthetic large-index test exercising predicates/stats **above
  `INT64_MAX`** (not just decode). Keep `large_list`/`large_string`/
  `large_list<u8>` modeling as a documented follow-on (RDR-4b) — the chunked
  decoder already casts those directly, so it is not blocking.

### Phase 2 — RDR-28a chunked chromatograms (after Phase 1)
- Drop the `"not yet supported"` throw in `chromatograms.cpp:34`; route the
  chunked chromatogram data table through `Encoding::decode_chunked` (the
  time/intensity axes), reusing the existing chunked spectra path.
- Validate against `small.chunked` + `small.numpress` chromatograms (numpress
  chunk encoding too) vs pyarrow / toleranced ground truth; **keep the has_uv
  point multi-intensity regression (RDR-29) green**; add e2e assertions.
- **RDR-28b chunked wavelength: DEFERRED** — no bundled fixture has it
  (has_uv wavelength is `point`) and the chunk main-axis decoder hard-casts
  `DoubleArray` while wavelength is `Float32`. Leave the existing throw; reopen
  when a chunked-wavelength fixture exists (e.g. generated via Rust `convert`).

### Phase 3 — Access modes (PARALLEL — genuinely disjoint, after Phase 2)
- **RDR-25** detail-level (metadata-only) mode — `Spectra`/`Index` level. Decide
  data-vs-peaks source from metadata `number_of_data_points`/`number_of_peaks`
  (NOT decoded-array emptiness), and skip array decode. Touches `spectrum.h/.cpp`,
  `spectra.h/.cpp`, `index.cpp`. Validate: metadata-only read returns
  ids/ms_level/time with empty arrays; full mode unchanged.
- **RDR-22a** buffer-backed **directory** source only — `open_buffer`/an
  in-memory file over `std::span<const std::byte>`. Touches `open.cpp`,
  `archive.h`, `file.cpp`, `directory.cpp`. **RDR-22b buffer-backed zip is
  DEFERRED** (needs a memory-backed libzip archive + lifetime design;
  `zip.cpp:193` reopens per member).
These two are file-disjoint → parallel worktree agents.

### Phase 4 — Writer (scoped)
- **WRT-2** (bounded only if scoped): emit a minimal/default run-level
  `metadata{}` block, OR accept raw JSON metadata — round-trip through the C++
  reader's `Index::metadata()` **and** the Rust reader. Serializing the full
  typed `RunMetadata` is a larger API change (the writer's input model is only
  `SpectrumData`); treat that as out of scope.
- **WRT-1** chunked/numpress emit — confirmed out of scope (a separate
  multi-month project: nested Arrow schema + chunk segmentation + null-marking +
  delta-model fit + numpress encode).

### Phase 5 — Blocked / external (document prerequisites; not implemented here)
- **RDR-19** OpenMS `MSSpectrum`/`MSExperiment` integration — uses RDR-10 +
  RDR-24; the actual point of the reader. STATUS (checked 2026-06-14): an OpenMS
  source tree (`~/Claude/OpenMS`) and a partial CMake build (`~/openms_build`)
  exist locally, but `libOpenMS` is NOT built (only deps: libOpenSwathAlgo,
  SQLiteCpp, sqlite3, yaml-cpp) — so there is nothing to link against yet.
  Prerequisite: finish the OpenMS build (long compile). Architecture decision
  still open: the adapter likely belongs in the OpenMS tree as an mzPeak file
  handler rather than adding a heavy OpenMS build-dep to this library. Deferred.
- **RDR-20** Parquet AES decryption — needs encrypted fixtures + a key path.
- **RDR-21** LRU cache, **RDR-23** page-index — perf infra, no behavioral fixture.
- **RDR-14** remote/cloud (HTTP-range/S3) — P4 "future".

## Risks & mitigations
- **RDR-4** blast radius (signed→unsigned index across Query/stats/casts):
  isolate in its own worktree, run full suite + e2e + cross-impl after, and keep
  the signed path compiling until the unsigned path is proven on every fixture.
- **RDR-28a** must not regress the has_uv point multi-intensity path (RDR-29) —
  chunked aux is a different code path; assert both in e2e.
- **RDR-25** must derive existence from metadata counts, not decoded emptiness,
  or metadata-only mode mis-routes data-vs-peaks.
- Reviewers flagged `record_count()`/`Query` stats casts as the easy-to-miss part
  of RDR-4 — add the >`INT64_MAX` predicate/stats test, not only a decode test.
