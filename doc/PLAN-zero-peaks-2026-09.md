# Plan: the "large archives return zero peaks, silently" defect (2026-09-09)

Companion to `doc/HANDOFF-large-archive-zero-peaks.md`.  Written before any
source change; the review brief quotes it verbatim.  Everything marked
**measured** was run in this session (locally on macOS/Arrow 25, or on `kim`
against the staged archives with the trunk library and Arrow 21); everything
else is read from the code.

## 0. Summary of what was found

**The handoff's diagnosis is wrong in its central claim, and the failure is
not size-dependent at all.**

- The C++ reader (trunk, built on `kim`, Arrow 21) returns the *correct* peak
  arrays for `diatracer_big.point.mzpeak`: 12/12 sampled spectra across the
  whole index range decode to exactly their declared `number_of_peaks` (500,
  322, 87, ...), with plausible m/z ranges, precursors and selected ions
  attached, no exception.  **Measured** (`probe` binary, see §5).
- FASTag still reports `200 MS2 spectra, 0 tags` on that archive and
  `4585 tags` on `diatracer_agxt` with the *same* `libmzpeak.so` build.
  **Measured**.
- The discriminator is the **selected-ion m/z**: in `diatracer_big` every one
  of the 3,086,644 `spectra_metadata_selected_ions.selected_ion_mz` values is
  `0.0` (non-null); in `diatracer_agxt`, `cptac_merged` and `tdf_agxt_raw`
  every value is non-zero.  **Measured** with pyarrow on `kim`.
- Cause of the zeros: the source mzML
  (`4223_TIMS2_006_2_Slot2-6_1_2877_diatracer.mzML`) writes `<selectedIon>`
  with `charge state` and `peak intensity` but **no `MS:1000744 selected ion
  m/z`**; mzpeak-convert (0.7.0 on `kim`; reproduced locally with 0.11.3 on
  another diaTracer file, `FKL4341-S08-...1305.mzML`) materialises the
  absent value as `0.0` instead of NULL (`SelectedIon { mz: f64 }` defaults to
  0.0 in the Rust model; `writer/visitor.rs:1285` writes it unconditionally).
  The working `diatracer_agxt` was converted from a *different* diaTracer file
  (`FKL4341-S23-...1320.d`) whose selectedIon carries the m/z.  **Measured**.
- Consumer effect: `FASTag/src/OnDiscMzPeakExperiment.cpp:110-118` prefers a
  *present* selected-ion m/z over the isolation-window target, so every
  precursor gets m/z 0; `FASTag.cpp:1377-1381` then calls
  `tagSpectrum(spec, 0.0, z)` and no tag can form.  On mzML input OpenMS's
  `MzMLHandler.cpp:2027-2031` sets the precursor m/z from the isolation window
  target (`MS:1000827`) and only overrides it when `MS:1000744` is present —
  hence the 62,347,705 tags from the mzML.
- The apparent size correlation in the handoff table is a coincidence of which
  diaTracer files happened to carry `MS:1000744`.  `bisect_85.35` inherits the
  zeros (it is a re-conversion of the failing archive) and additionally trips
  the re-conversion lane's profile/`number_of_data_points` mismatch.

Two further reader defects surfaced on the way (both **measured**, both with
cheap deterministic reproductions):

- **D2 — silent truncation on mixed profile/centroid archives.**  mzpeak-convert
  ≥ 0.11 stamps the `spectrum_count` KV *per table* (spectra in that table),
  while `Spectra` sizes itself as `max(data.record_count(), peaks.record_count())`
  (`src/spectra.cpp:58-60`).  `small.mzML` → 48 spectra (14 profile, 34
  centroid) → the reader reports `spectra.size() == 34` and spectra 34–47 are
  unreachable, no error.  On `hela_ddapasef` 371,184 vs 371,187.
- **D3 — the existing count check is representation-gated** (`src/spectrum.cpp:142-155`):
  with an empty/unknown representation a declared count is never compared.
  The "make it loud" change below closes that.

## 1. The code path, planner to returned points

All lines are trunk `768125f`.

| stage | file:line |
|---|---|
| FASTag asks for spectrum *i* | `FasTag/src/OnDiscMzPeakExperiment.cpp:337-347` → `impl_->spectra[i]` |
| table choice by representation | `src/spectra.cpp:216-266` `Spectra::fetch_` (centroid → `peaks_`, profile → `data_`, else "wherever the data is" 241-244); dims filtered 251-264 |
| lazy decode entry | `src/spectrum.cpp:46-59` `Spectrum::decode_` → `signals_->select(dims_, index().eq(index_))` |
| projection + plan + execute | `src/data/signals.cpp:175-213` `Signals::select` |
| planner, ordered fast path | `src/util/planner.cpp:693-749` `Planner::Impl::plan`: `all_sorted_ascending` (135-151) → `whole_groups_ = true` (724) → `plan_row_group` (612-637): stats prune 622-626 (`with_column_stats` 535-549, UInt64 bit_cast 430-449), else a whole-group range **and `count_plan_full_scan()` at 633** |
| planner, true fallback | `src/util/planner.cpp:678-690` `full_scan` (also `count_plan_full_scan()` 687) |
| executor | `src/util/executor.cpp:349-540` `Executor::execute`: `sorted_column` 405-409; decoded group 416 (`src/util/parquet.cpp:234-277` → `decode_group_` 280-386, per-batch key spans 343-384); key-span batch narrowing 437-459; whole-batch binary-search filter 499-514 (`EqualityScan` 204-264, `filter` 267-310); `project` 313-318 → `Slice::append` (`src/util/slice.cpp:69-80`) |
| decode | `include/mzpeak/data/encoding.h:118-133` `decimal` → `remap` 158-169 → `decode` 174-208 → `decode_with_nulls` 213-247: **silent early return when the column is not in the slice (223-224)**; point path `Decoders::Scalar` (`include/mzpeak/util/decoders.h:187-207`), nulls via `NullToZero` or `NullMarking::Decoder` (`include/mzpeak/data/null_marking.h`) |
| consistency checks | `src/spectrum.cpp:132-136` (mz vs intensity length), `142-155` (declared count, representation-gated) |
| metadata behind `metadata()` | `src/util/metadata_model.cpp:867-1216` PASS 1; `attach_precursors` 665-727; `attach_selected_ions` 736-797 (**`selected_ion_mz` read at 761-762, no plausibility check**); scans 1235-1341 |

Counter semantics that misled the handoff: `count_plan_group_eval()` is
**never called** (grep: only its definition in `src/util/executor.cpp:85`), so
`plan_group_evals = 0` carries no information; `plan_full_scan` fires on both
planner branches (633 and 687).  The sorted-column path *was* taken on both
archives — `batches_visited == plan_ranges (+1)` proves the per-batch key
narrowing was active (without keys every ~17 batches of a group are visited).

## 2. Quantities that grow with size, and the types holding them

Read, not measured.  None overflow at the observed scale; listed so the
review can attack them.

- `Planner::Range{int32_t row_group; int64_t offset, length}` (`planner.h:94-98`);
  `StatsIndex::Impl::slots_` sized `groups × columns` as `size_t` (`planner.cpp:61-62`);
  `columns_` is `int32_t` (163); `plan()` loops `int32_t` groups (695, 727, 742).
- `Executor::execute`: `int64_t` row offsets (389-400, 467), `size_t` batch
  indices (437-438); `project_wanted(size_t i, size_t j)` → `int64_t` (378-386).
- `EqualityScan`: `int64_t length`, `want_rows.assign(size_t)` (226, 254);
  the general filter loop `std::views::iota(0, batch->num_rows())` has value
  type `int` (303) — fine while a record batch is < 2^31 rows (Arrow batches
  are 65,536 rows).  Same pattern in `decoders.h:198`, `algorithm.h:182`,
  `null_marking.h:125`.
- `RowGroupBatches::row_offset/key_first/key_last` are `int64_t`; `key_leaf`
  `int32_t` (`row_group_cache.h:366-380`).
- `Signals::record_count()` parses the `spectrum_count` KV into `size_t`
  (`parquet.cpp:45-62`); statistics fallback `bit_cast<uint64_t>` (`signals.cpp:130`).
- `IndexMap` keys `uint64_t`; dense fast path `key < data_.size()` (`index_map.h:263`).
- File layer: `ZipFile_::read/seek/tell` go through `zip_int64_t` (`zip.cpp:41-64`);
  `ArrowFile_::Read(int64_t)` → `size_t` (`arrow.cpp:63-105`); `DirFile_`
  uses `std::streamsize` (`directory.cpp:261-282`).  No 32-bit path.
- Footer/Thrift list lengths, page/offset indexes: handled inside Arrow;
  the planner only touches the page index when `!whole_groups_` (639-674).
- `struct_from_table` → `Table::CombineChunks` (`metadata_model.cpp:279-281`):
  the one place a 32-bit limit exists (Arrow `string`/`list` offsets overflow
  at 2 GiB of data per column) and it **fails silently** (`return nullptr` →
  "not a spectrum metadata table" / a facet silently dropped).  Not reached at
  3 M spectra (the `string` columns are ~10 B/row).

## 3. Hypotheses, discriminators, and what falsified them

| # | hypothesis | discriminating quantity / prediction | status |
|---|---|---|---|
| H1 | selected-ion m/z is 0.0 in the failing archive, so FASTag tags against precursor m/z 0 | `selected_ion_mz` all-zero in failing, non-zero in working; library returns full peak arrays | **CONFIRMED** (measured on `kim`, §0) |
| H2 | row-group count (> ~950) breaks planner/StatsIndex/executor | a parquet-rs archive with > 1000 groups fails | **FALSIFIED**: `hela_ddapasef.point.mzpeak`, 1,026 groups / 1.075 G rows, reads correctly (measured, local) |
| H3 | row count ≥ ~1 G | same archive | **FALSIFIED** (1.075 G rows > bisect's 0.999 G) |
| H4 | spectrum count > 2^20 (parquet-rs's row-group cap) puts the metadata tables into ≥ 2 row groups and the reader mishandles multi-chunk facets | synthetic 1,100,000-spectrum mzML → point archive with 2-row-group metadata tables | **FALSIFIED**: index 1,099,999 (second row group) decodes with its precursor (measured, local) |
| H5 | representation/count columns unparseable (dictionary type, naming) so the count check is disabled and a genuine zero decode goes unnoticed | `representation` empty in the probe | **FALSIFIED**: `MS:1000127` and `number_of_peaks` parse on all archives |
| H6 | writer-version skew (array index / column naming) → empty projection | array index / schema differences | **FALSIFIED**: both archives written by mzpeak-convert 0.7.0, identical layout |
| H7 | Arrow 21 (kim) vs 25 (local) decode difference for `DELTA_BINARY_PACKED` / `BYTE_STREAM_SPLIT` | probe on kim with Arrow 21 | **FALSIFIED** for this archive (probe on kim reads it) |
| H8 | precursor/selected-ion *join* lost at scale | `prec=0` or `ions=0` in the probe | **FALSIFIED** (join intact; the *value* is 0) |

Cheap falsification recipe for anything similar in future: build
`scratchpad/probe.cpp` (in this session's scratchpad; 100 lines, links the
static library) and run `probe <archive> --sample 12 --values --lean`; it
prints per spectrum the parsed metadata, precursor/ion values, decoded sizes,
and any exception.  It builds on `kim` against the env's Arrow 21 in ~4 s and
answers "is this a reader problem at all?" before any bisection is planned.

Note on the counters that sent the handoff down the size hypothesis:
`count_plan_group_eval()` has no call site anywhere in the library, so
`plan_group_evals = 0` is not evidence of anything; and `count_plan_full_scan()`
fires both on the sorted whole-group path (`src/util/planner.cpp:633`) and in
the true `full_scan` fallback (`:687`), so `plan_full_scan = 264` does not mean
the fallback was taken.  `plan_pi_null` / `plan_page_index` are the counters
that actually discriminate.  Both archives took the sorted fast path.

## 3a. Root cause: CONFIRMED by intervention

The falsifications above are circumstantial; this is not.  With the
selected-ion normalisation of §5.1 applied to the library on `kim` and FASTag
relinked against it, the handoff's own command was re-run unchanged:

| archive | trunk | with the one-line reader fix |
|---|---:|---:|
| `diatracer_big.point.mzpeak` | 200 MS2 spectra, **0 tags** | 200 MS2 spectra, **4,263 tags** |
| `diatracer_agxt.point.mzpeak` (control) | 200 MS2 spectra, 4,585 tags | 200 MS2 spectra, 4,585 tags |

Same archive, same binary otherwise, same seed and subsample; only the reader's
treatment of a zero selected-ion m/z differs.  The zero *is* the defect, and
normalising it restores the run while leaving a healthy archive untouched.
`kim`'s tree was restored to trunk afterwards and FASTag relinked, so the
environment is again as the handoff describes it.

## 4. "Make it loud" — extend the consistency check

`src/spectrum.cpp:142-155` compares the decoded length only against the
count *named by the representation*.  The change is scoped to the case where
the representation names nothing at all, and then mirrors `fetch_`'s own table
choice:

```
if (!expected) {
  // mirror fetch_'s table choice for an unknown representation
  if (ndp.value_or(0) == 0 && np.value_or(0) > 0) expected = np;
  else if (ndp) expected = ndp;
  else if (np) expected = np;
}
```

**Scoped deliberately.**  An earlier draft of this rule fell back whenever the
count *named by the representation* was null, whatever the representation
said.  The adversarial review's strongest finding is that this rejects
legitimate files: a spectrum declaring `profile` with `number_of_data_points`
null but `number_of_peaks = 1612` decodes 13,589 profile points correctly and
would then be measured against 1,612 and thrown away.  A populated
representation is the file telling the reader which array it is; the fallback
must therefore run **only** when there is no representation at all — exactly
the case where `fetch_` is itself guessing — and must mirror that guess.

Legitimate cases that must NOT throw: an empty spectrum declaring 0
(`writer_test: zero_data_points_round_trip`), MS1-only runs, profile spectra
carrying both counts (`small.chunked.mzpeak` spectrum 0: 13589 + 1612), the
chunked and numpress layouts, ims-compact (m/z reconstructed before the check,
`src/spectrum.cpp:116-124`), and the C++ writer's convention of writing the
"other" count as 0 rather than null (`src/writer.cpp:193-194`).  All are
covered by `test/zero_peaks_test.cpp`.

Honesty note: this check would **not** have caught the FASTag failure (the
peaks were correct).  It still removes a genuine silent mode (D3).

Known, pre-existing, NOT fixed here: `Index::spectra(..., SpectraSource::Peaks)`
reads the peaks table regardless of representation
(`src/index.cpp:88-91`) while the check still uses the representation's count,
so `mzp-inspect --peaks` on a profile archive reports "decoded 0 points but the
file declares 13589" — observed on a converted `small.mzML`.  The check is
right and the routing is wrong; fixing it means telling `Spectrum` which table
it was fetched from.

## 5. Fixes implemented, and the tests that prove them

All five are on `fix/zero-peaks-loud`, each with a regression test in
`test/zero_peaks_test.cpp` verified to FAIL on trunk and pass after (six of
the seven cases fail on trunk; the seventh is the control).

1. **A selected-ion m/z of 0 (or non-finite) reads as absent**
   (`src/util/metadata_model.cpp`, in `attach_selected_ions`).  m/z has no
   meaningful zero; the field is nullable in the format; the converter writes
   0.0 because its model holds a bare `f64`.  Non-finite is included so `+inf`
   cannot slip past a "present" test — the review's point.  Deliberately NOT
   generalised: intensity 0 is a real measurement, retention time 0 a real
   acquisition time, charge 0 already means "unknown", and the isolation
   window keeps its file-native value (its contract,
   `include/mzpeak/spectrum_metadata.h:45`, is to preserve what the file says).
   Proven end-to-end by §3a.
2. **`Spectra` is sized from the metadata** (`src/spectra.cpp`,
   `resize_from_metadata_`), with the signal tables' counts as a floor and a
   guard against `last + 1` wrapping.  Called after `load_metadata_` on BOTH
   its paths — the cached-map path returns early, and that is the ordinary
   path through `Index::spectra()`, so resizing only on the other one would
   have left every real reader broken (the review caught this).
3. **The loud check** (§4), scoped to an absent representation.
4. **A failed decode no longer accumulates.**  `std::call_once` re-runs its
   callable after an exception and the decoders append, so a spectrum that
   threw once returned duplicated peaks on the next access — an error turned
   into a plausible wrong answer.  The buffers are cleared at the start of
   every attempt.
5. **The planner's group hint no longer changes the answer**
   (`src/util/planner.cpp`).  A spectrum straddling a row-group boundary lost
   its first group whenever the hint had moved past it — reachable by reading
   the same spectrum twice, or any non-monotonic order (a subsampled run).
   Silent on a file with no per-spectrum count; a throw on one with counts.
   The comment above the hint claimed "the result cannot depend on the hint —
   only the cost"; now it does not.

Reported, not fixed here: **the converter** should write NULL rather than 0.0
for a missing `MS:1000744`; **FASTag** should prefer the isolation-window
target when the selected-ion m/z is absent *or non-positive* (a one-line
guard, drafted in the session scratchpad).  Either alone would have prevented
the 0-tag run; the reader fix repairs the archives that already exist.

Also found, NOT fixed (each needs its own change and test):
`SpectraSource::Peaks` routing versus the count check (§4);
`struct_from_table`'s `CombineChunks` failure being swallowed
(`src/util/metadata_model.cpp:279-299`) — a metadata table whose string
columns exceed Arrow's 2 GiB per-column offset limit loses the entire map
silently; and `std::views::iota(0, int64)` deducing `int` in the decoder
loops (`include/mzpeak/util/decoders.h:198`), which is unreachable for
ordinary batches but not for a single nested list of > 2^31 children.

**Docs:** the handoff's central claim is corrected by this plan; the counter
semantics (`plan_group_evals` is dead, `plan_full_scan` fires on both planner
branches) and the size table should be corrected there too.

Out of scope, deliberately: `timstof_dia` (null `number_of_peaks`, 0 MS2
reported) — a different shape, untouched.
