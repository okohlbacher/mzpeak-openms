# Handoff: the writer's page index cannot locate a spectrum

**Status:** open, writer-side. The obvious reader-side workaround was tried and
REVERTED -- it crashes at high thread count, see the last section. So nothing
mitigates this today, and the writer change is the way to make the *format*
plan well for any reader.

**Owner:** unassigned. Everything below is measured, not inferred, unless a
line says otherwise.

## One-paragraph summary

`point.spectrum_index` is the column every spectrum query filters on. It is
sorted, declared sorted, and compresses to **7,528 bytes per row group** under
`RLE_DICTIONARY`. That is far below Parquet's 1 MB default data page size, so
the column chunk holds essentially one page, and its page index therefore has
almost no resolution over the ~1,048,773 rows it covers. A reader asking for
one spectrum gets a planned range covering **56% of a row group**. The better
this column compresses, the less the page index can localise it — the format
is defeating its own index.

## The measurement

Archive: 9.07 GB, 762,016 MS2 spectra, 363 row groups of 1,048,773 rows,
379,815,617 points. Counters are in the reader, behind
`-DMZPEAK_READ_COUNTERS` (see `src/util/executor.cpp`).

```
plan_group_evals  276,635,040     363 per spectrum
plan_pruned       275,872,960     row-group statistics reject 99.72%
plan_full_scan              0     the whole-group fallback is NEVER taken
plan_pi_null                0     the page-index reader is always available
plan_page_index       762,080     every surviving group goes through it
plan_ranges           762,080     one range per spectrum
plan_range_rows 444,204,120,818   -> 582,880 rows per range
```

582,880 of 1,048,773 rows, for a spectrum holding a few hundred points — about
0.05% of the group. Row-group statistics are doing their job perfectly (one
group per spectrum); it is the page index inside the group that adds nothing.

Cross-check: 582,880 / 65,536 = 8.9 record batches, against 9.67 slices per
spectrum measured independently in the executor. The two numbers agree, so
this accounts for the executor's slicing entirely.

## Why it matters

Each surviving batch is sliced before it can be filtered, and a slice copies
the ownership of every column. Arrow shares **one datatype object per
primitive type**, so those refcounts are true sharing between every reader
thread regardless of which row group each is reading. On a 2-socket EPYC 9654
this is the leading suspect for why the mzPeak read path *degrades* from 64 to
192 threads (6.00 s -> 7.13 s in the parallel phase) while an mzML reader over
the identical spectra *improves* (4.70 s -> 3.54 s).

That suspicion is not yet proven — see "What is still unproven".

## The proposed fix

Cap pages by **row count**, not just by byte size, when writing the signal
tables. Parquet exposes this as `data_page_row_count_limit` on
`WriterProperties`. The relevant writer setup is
`standard_writer_properties()` in `src/util/parquet_writer.cpp`.

A limit of 65,536 rows would give ~16 pages per row group and align page
boundaries with Arrow's default record-batch size, so a page-index hit would
narrow to roughly one batch — which is exactly the granularity the executor
consumes.

### What to check before committing to 65,536

- **Page overhead.** More pages means more page headers and a larger page
  index. At 7,528 bytes per chunk for `spectrum_index`, 16 pages is a real
  relative overhead on *that* column, though negligible against the 2.5 MB
  `mz` and 1.8 MB `intensity` chunks in the same group. Measure the archive
  size change; the mzPeak-vs-mzML size advantage (6.4x smaller: 1.42 GB vs
  9.07 GB) is a headline number worth protecting.
- **Whether it must apply to every column.** Only the sorted lookup column
  needs fine pages. `WriterProperties` sets the row-count limit globally, so
  check whether a column-scoped override exists in the Arrow version pinned
  here before accepting a whole-file cost for one column's benefit.
- **Compression interaction.** `RLE_DICTIONARY` on a sorted, highly repetitive
  column is what makes the chunk so small. Splitting into 16 pages restarts
  the encoding per page; confirm the column does not balloon.

### Acceptance test

Write one archive each way from the same input, then on each:

1. `plan_range_rows / plan_ranges` should fall from ~582,880 to roughly one
   page's worth. This is the number the change exists to move.
2. `slices_made / spectra` should approach 1 (it is 9.67 today).
3. Tags must be **byte-identical** to the current output. Every optimisation
   in this area so far has held that line; do not relax it.
4. Archive size must not regress materially.

## What is still unproven

Be careful here — three plausible-sounding claims in this area have already
been measured false:

- That the shared row-group cache's single mutex was the bottleneck. It was
  not: over 99% of spectra never reach the cache (3,199 calls at 64 threads
  against 762,016 spectra), and removing the traffic changed nothing.
- That hoisting per-block reader construction would recover its full cost. The
  cost was real and serialized (12.70 s -> 4.49 s in isolation) but overlapped
  with work, so the run gained ~1 s, not 8.
- That making the metadata field lookup cheaper would not help. It halved the
  metadata build, 3.65 s -> 1.80 s. An earlier A/B said "no effect" and that
  A/B was built wrong.

So: **this handoff does not claim the writer change will fix the anti-scaling.**
It claims the page index provides no useful resolution, which is measured, and
that fixing it is the structural way to cut the slicing, which follows. Whether
that moves wall-clock time must be measured, not assumed.

## Reproducing

Reader counters:

```bash
meson configure build -Dcpp_args=-DMZPEAK_READ_COUNTERS
ninja -C build && meson install -C build --destdir ""
# then, from FASTag:
FASTAG_TIMING=1 FASTag -in run.mzpeak -out /dev/null -threads 64 ...
```

The `FASTAG_TIMING` line carries `plan_ranges`, `plan_range_rows`,
`slices_made`, `batches_visited` and the phase split. Build **without**
`-DMZPEAK_READ_COUNTERS` for any timing number: the planner counter fires
276 million times per run and distorts what it measures.

Footer geometry of an archive:

```python
import zipfile, pyarrow.parquet as pq
z = zipfile.ZipFile("run.mzpeak")
m = pq.ParquetFile(z.open("spectra_peaks.parquet")).metadata
rg = m.row_group(0)
print(rg.num_rows, rg.sorting_columns)
for i in range(m.num_columns):
    c = rg.column(i)
    print(c.path_in_schema, c.total_compressed_size, c.encodings,
          c.has_column_index, c.has_offset_index)
```

## A reader-side attempt that FAILED — read before retrying it

The obvious reader-side workaround is to stop trusting the planned range: when
the column is declared sorted and the query is an equality, filter the
**unsliced** batch (one binary search) and slice only the run that matched.
`Query::as_equality()` exposes the predicate, and `Executor::Impl::filter()`
already returns a contiguous run for that case, so the change is short.

It was implemented and **it crashes**. Do not simply re-apply it.

- 1 and 16 threads: byte-identical tags, 38/38 library tests pass.
- 192 threads: `Aborted` on one run, `Segmentation fault` on another, and a
  tag-file mismatch on a third.
- The same tree with the fast path reverted: three clean runs at 192 threads,
  19,498,431 tags each.

So the defect is concurrency-dependent and invisible at low thread counts and
to the test suite. Candidates not yet eliminated, in the order worth checking:

1. `filter()` takes its batch by **non-const** reference and the fast path
   hands it a copy of the cached batch. If anything on that path mutates the
   batch, the cached, SHARED batch is being mutated under other threads.
   `Executor::Impl::array()` is the thing to read first.
2. Run bounds. `run.second` is exclusive (the existing caller does
   `project_wanted(run.first, run.second - 1, ...)`, and `project_wanted` takes
   inclusive indices). The fast path assumed exclusive and clamped with the
   range's batch-relative `offset`/`length`. That reasoning holds at low thread
   counts, but has not been proven for straddling ranges.
3. Whether `have_run` can be true with `run` referring to a different array
   than the batch just passed.

Whoever retries this should run it at high thread count under a sanitiser
before trusting a byte-identical low-thread result — that result was obtained
here and was misleading.
