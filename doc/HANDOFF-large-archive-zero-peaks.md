# Handoff: large archives return no peaks, silently

**Status:** open, **reader-side**, not the writer. Long-standing — the shipped
FASTag v1.2.1 behaves identically, so this is not a regression from the
2026-09 read-path work.

**Severity: this is the worst failure shape in the library.** No exception, no
warning, exit code 0. The reader reports the correct spectrum count and returns
zero points for every one of them, so a consumer writes an empty result and
calls it a clean run. On one 23 GB archive that is **0 tags instead of
62,347,705**.

**Owner:** unassigned. Everything below is measured unless a line says it is a
guess.

## One-paragraph summary

Reading `diatracer_big.point.mzpeak` (1,507,781,795 peak rows) through the C++
reader yields 0 points for every spectrum. The archive is intact: the Rust
`mzpeak-convert` reads the same file and re-emits ~1e9 peak rows from it. The
planner and the executor do *identical* work on a working archive and on this
one — same range count, same batches visited, same slices made — and only the
peaks come back empty. So the defect is downstream of batch selection, in
decoding or in the predicate that picks rows out of a decoded group, and it is
size-dependent.

## Reproduce

Everything is staged on `kim` (node-local `/scratch`, so treat it as
perishable); the source mzML is on Ceph and is not.

```bash
# fails: 0 tags, exit 0, ~14 s
FASTag -in /scratch/kohlbach/bigbench/mzp/diatracer_big.point.mzpeak \
       -out /dev/null -threads 1 -subsample_spectra 200 \
       -tag_length 3 -gaps 0 -max_tags 50 -max_peaks 100 -peaks_per_window 0 \
       -fragment_tolerance 0.02 -fragment_tolerance_unit Da -fixed_modifications ""
#   -> "200 MS2 spectra, 0 tags"

# control, same command, works: "200 MS2 spectra, 4585 tags"
FASTag -in /scratch/kohlbach/bigbench/mzp/diatracer_agxt.point.mzpeak ...
```

**One thread and 200 spectra reproduce it**, so it is neither concurrency nor
scale of run. Rebuild the archive from
`/ceph/ibmi/abi/oliver/benchmark/PXD047793/raw/4223_TIMS2_006_2_Slot2-6_1_2877_diatracer.mzML`
(23.01 GB) with `mzpeak-convert --layout point` if `/scratch` has been cleaned;
it takes ~5 minutes.

## What the counters say — this is the useful part

Built with `-DMZPEAK_READ_COUNTERS`, 200 spectra, one thread, failing archive
against working archive:

| counter | `diatracer_agxt` (works) | `diatracer_big` (fails) |
|---|---:|---:|
| `plan_ranges` | 264 | 264 |
| `plan_range_rows` | 276,148,876 | 276,824,064 |
| `plan_full_scan` | 264 | 264 |
| `plan_group_evals` | 0 | 0 |
| `plan_page_index` | 0 | 0 |
| `batches_visited` | 265 | 265 |
| `slices_made` | 265 | 265 |
| **tags** | **4,585** | **0** |

**The two runs are indistinguishable until the peaks come out.** The planner
resolves the same number of ranges over the same number of rows, the executor
visits and slices the same batches. Whatever is wrong happens after the right
rows have been located.

Note also `plan_full_scan = 264` on both: these diaTracer archives do **not**
take the sorted-column fast path, they take the whole-group fallback and filter
inside it by key. So the suspect surface is that fallback's key filter and the
decode beneath it, not the fast path added in `read-scaling-2026-09-08`.

## The archive is not the problem

Verified on the failing file:

- 1,507,781,795 rows in 1,438 row groups, schema
  `point.spectrum_index` (INT64, logical UInt64), `point.mz`, `point.intensity`
- `SortingColumn(column_index=0, ascending)` declared on every row group
- per-group statistics correct and contiguous: rg0 `[0, 2211]`,
  rg1 `[2211, 4518]`, rg719 `[1541212, 1543317]`,
  rg1437 `[3084625, 3086643]`
- `spectra_metadata.number_of_peaks` non-null for all 3,086,644 spectra and
  **summing to exactly 1,507,781,795** — the peak-table row count
- `ms_level` = 2 for all; `spectrum_representation` `MS:1000127`,
  `spectrum_type` `MS:1000580` — identical in shape to the working archive

And decisively: **`mzpeak-convert` (the Rust reader) reads this archive's peaks
and re-emits them.** A `--rt 0-85.35` mzPeak→mzPeak pass produced a new archive
holding 999,589,212 peak rows taken out of this one. The data is retrievable;
the C++ reader is not retrieving it.

## Where the boundary is — measured, and it fits nothing obvious

| archive | peak rows | row groups | peaks member | C++ reader |
|---|---:|---:|---:|---|
| `diatracer_agxt` | 356,889,228 | 341 | 1.32 GB | works |
| `tdf_agxt_raw` | 500,832,884 | 478 | 4.87 GB | works |
| `cptac_merged` | 575,291,684 | 549 | 3.80 GB | works |
| `bisect_85.35` † | 999,589,212 | ~953 | ~3.6 GB | fails |
| `diatracer_big` | 1,507,781,795 | 1,438 | 5.28 GB | fails |

† a mzPeak→mzPeak re-conversion of the failing archive, so **treat it as weak
evidence**: it fails with a *different* symptom (see below), and the
re-conversion path may have its own defect.

Two hypotheses this table already weakens:

- **Not a 2^30-row limit.** `bisect_85.35` fails at 999,589,212 rows, below
  2^30 = 1,073,741,824.
- **Not a byte threshold.** A 3.6 GB peaks member fails while a 4.87 GB one
  works.

A signed-32-bit overflow on a row index does not fit either — 1.51e9 is below
2^31. The honest position is that the discriminating quantity is **not yet
identified**, and a clean bisection is the next step.

## The one extra clue

`bisect_85.35` does not fail silently. It throws:

```
terminate called after throwing an instance of 'MzPeak::ParquetError'
  what():  spectrum 1029372: decoded 0 points but the file declares 500
```

Same outcome — zero points — but *detected*, because the re-converted archive
carries a populated `number_of_data_points` where the original has it null.
**The reader already has a consistency check for exactly this failure, and on
the original archive that check does not run.** Whatever else is fixed, making
that check fire whenever any per-spectrum count is available would convert this
from a silent wrong answer into a loud one.

## Suggested next steps, in order

1. **Make the failure loud first**, before diagnosing it. The check above exists;
   extend it to `number_of_peaks` (which the failing archive *does* carry, and
   which sums exactly to the row count) so a zero-point decode against a
   non-zero declared count throws instead of returning empty. This is a small,
   independently shippable change and it removes the dangerous property.
2. **Bisect cleanly on row count.** Do it by truncating the *mzML* and
   converting each truncation, not by re-converting the mzPeak — the
   mzPeak→mzPeak path introduces the second symptom above and muddies the
   result. Target the interval 575 M … 1,000 M rows; five conversions bracket
   it to ~50 M.
3. **Instrument the whole-group fallback.** `plan_full_scan` is the path taken
   here. Log, for one spectrum, the key value being searched, the decoded
   group's key range, and the row count the filter returns. If the filter
   returns zero against a group whose statistics bracket the key, the bug is in
   the key comparison; if the group decodes with zero rows, it is in the decode.
4. **Check the second, different defect separately.** `timstof_dia`
   (5,417,849,389 rows, 5,167 groups, an extra `point.Ion_Mobility` column)
   fails a different way: `number_of_peaks` is null for all 156,388 spectra,
   `number_of_data_points` is populated instead, `spectrum_type` is
   `MS:1000294`, and the reader reports **0 MS2 spectra** rather than 0 peaks.
   That looks like a writer-side metadata-shape problem, not this one. Do not
   fix them together.

## What is already ruled out

- **Concurrency** — reproduces at `-threads 1`.
- **Scale of the run** — reproduces with 200 subsampled spectra.
- **A regression from the recent read-path work** — shipped v1.2.1 is identical.
- **Archive corruption** — statistics, sorting and metadata all verified, and
  the Rust reader reads the peaks out.
- **Metadata shape** — identical to the working `diatracer_agxt`.
- **The sorted-column fast path** — not taken here (`plan_group_evals = 0`,
  `plan_full_scan = 264`).

## Environment

`kim`, `/scratch/kohlbach/build/{mzpeak-src,fastag-src}`, micromamba env
`/scratch/kohlbach/mamba/envs/fastag`. Sources there are checksum-identical to
`trunk` at `40df266`. The library builds **static** (`libmzpeak.a`), so a
counter build needs FASTag relinked as well:

```bash
cd /scratch/kohlbach/build/mzpeak-src
meson configure build -Dcpp_args=-DMZPEAK_READ_COUNTERS
ninja -C build && meson install -C build --destdir ""
cd ../fastag-src && ninja -C build FASTag
FASTAG_TIMING=1 ./build/FASTag ...      # counters land on the FASTAG_TIMING line
```

Both trees were left **without** counters (`-Dcpp_args=`) — a counter build
distorts every timing, and the planner counter fires hundreds of millions of
times per run.
