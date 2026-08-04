# Specification compliance

Audited against `HUPO-PSI/mzPeak-specification` at `85442bd`
(`docs/conformance.md`), as both a conformant reader and a conformant writer.
Two independent passes: this implementation's own, and an adversarial audit by
an external model. Findings the audit raised are cited where they changed the
verdict.

Legend: **OK** compliant · **FIXED** was non-compliant, now compliant with a
regression test · **PARTIAL** compliant for what the writer emits / the common
case, with a documented deviation · **N/A** not applicable to this writer's
feature set.

## Reader

| Req | Verdict | Notes |
|---|---|---|
| R1 resolve members through `mzpeak_index.json` | **OK** | only `mzpeak_index.json` is a fixed name; every member is found via the index `files[]` by entity/kind, never by filename. |
| R2 support point and chunk layouts | **OK** | both decode; the chunked path's list-width gap is folded into R3. |
| R3 `list`≡`large_list`, `string`≡`large_string` | **FIXED** | see below — four silent sites. |
| R4 resolve signal columns via the array index | **PARTIAL** | m/z, intensity and mobility resolve by CV accession from the array index. The ims-compact `tof` column is still matched by name substring — see *Known deviations*. |
| R5 resolve CV parameters by accession | **OK** | all semantic dispatch compares accessions; names are advisory; accession stored verbatim. |
| R6 ignore unrecognized content without error | **PARTIAL** | unknown members, columns, metadata keys and CV accessions are ignored without error and copied verbatim. Unknown `entity_type`/`data_kind`/`array_type` collapse to an `Other`/`NonStandard` enum (the literal string is not retained), and a future `1.x` `metadata.version` is rejected rather than read best-effort. Both are future-facing — see *Known deviations*. |

### R3 — the four silent widths (FIXED)

The reference writer emits `large_list` and mixes string widths, so a
`LargeListArray`-only cast passed every reference fixture while silently losing
data from a conformant third-party archive (PyArrow defaults to 32-bit `list`
and picks string widths per field). All four are now width-agnostic, with
fixtures:

1. **Metadata list columns** — `parameters`, `scan_windows`, `auxiliary_arrays`
   cast only `LargeListArray`. A plain `list` read as empty. Demonstrated: a
   32-bit-list copy of `small.dir` lost all 48 scan windows before the fix, 48
   after (`test/files/list32.dir`).
2. **Chunked signal columns** — `chunk_values`/`chunk_secondary`/`chunk_transform`
   cast only `LargeListArray` and threw on a plain `list`. A 32-bit-list copy of
   the chunked fixture now decodes to the identical 243,054 peaks.
3. **CvParam struct strings** — `accession` was `string`-only, `name`
   `large_string`-only, `unit` `string`-only. The other width emptied the
   field, and an empty accession makes the term unmatchable (R5).
4. **Facet index column** — required to be exactly `UINT64`, so a `uint32`
   index (spec-permitted) silently emptied the *entire* metadata map. Now reads
   uint32/uint64/int32/int64 (`test/files/uint32_index.dir`).

## Writer

| Req | Verdict | Notes |
|---|---|---|
| W1 produce a conformant archive | **PARTIAL** | subject to the S-invariants below; a NaN coordinate is now rejected rather than written non-ascending; caller-supplied unit CURIEs are not checked for CV ancestry (S8). |
| W2 page index for index/coordinate columns | **OK** | `enable_write_page_index()` is the single choke point for every Parquet file written. |
| W3 declare every CV used, version-pinned | **FIXED** | `cv_list` was hardcoded MS+UO regardless of content. It is now derived from every CURIE prefix reachable in the finished index; MS and UO are always present (the array index and column mappings use them); each prefix is pinned from a registry of the vocabularies a mass-spec archive plausibly cites. An unpinnable prefix is still declared with a visible placeholder and a warning, never silently omitted. |
| W4 array index sufficient to reconstruct without names | **OK** (scope-limited) | full `path`/`data_type`/`array_type`/`unit`/`buffer_format`/`sorting_rank` per entry, in the spec-mandated Parquet KV location. Sufficiency holds for the writer's feature set: transforms, mobility and auxiliary arrays are not yet emitted. |

## Archive

| Req | Verdict | Notes |
|---|---|---|
| A1 valid index carrying SemVer `metadata.version` | **OK** | `0.9.0` stamped unconditionally. |
| A2 reference only present files matching entity/kind | **OK** | index entries and emitted files are appended together; no dangling reference. |
| A3 declare every CV prefix used | **FIXED** | via W3. |
| A4 one signal layout per file, by array-index prefix | **OK** | the writer emits point only; the reader accepts either. |

## Semantic invariants

| Inv | Verdict | Notes |
|---|---|---|
| S1 parallel columns equal length | **OK** | enforced in `validate()` and again in the sink writers. |
| S2 sorting-rank-0 array ascending | **FIXED** | the writer now verifies the index is ascending before declaring the sorting column, and a NaN coordinate is rejected. The verification function existed but was never called — a real gap. |
| S3 foreign keys resolve | **OK** (writer) | the writer emits only entity indices it generates; not validated on read, which readers are not required to do. |
| S4 chunks ascending by `chunk_start`, non-overlapping | **OK** | checked on read; not written (point-only writer). |
| S5 one layout family per entity | **OK** | the reader accepts either; the writer never mixes. |
| S6 time columns in minutes | **OK** | every time is stored minutes (÷60 on write) with `UO:0000031` declared; the reader converts on read. |
| S7 signal file carries a page index | **OK** | see W2. |
| S8 CURIEs descend from their CV parents | **PARTIAL** | the writer's own CURIEs are correct by construction; a caller-supplied unit or type CURIE is not checked for CV ancestry (that needs a CV, not just syntax) — see *Known deviations*. |

## Known deviations, with reasons

These are deliberate, documented, and low-impact on real data. None affects the
reference corpus or a well-formed archive from the reference writer.

- **R4, ims-compact `tof`** — the `tof` column is matched by the substring
  `tof` in its name rather than purely through the array index. `tof` has no PSI
  accession, so the array index types it `NonStandard`; resolving it needs
  *some* discriminator, and the column name is the one the reference emits.
  A conformant ims archive that named the column differently would not be
  projected. Narrowing this to the array index's declared `array_name` is a
  follow-up; it does not bite the current corpus.
- **R6, literal preservation and the version gate** — an unrecognized
  `entity_type`/`data_kind`/`array_type` is ignored (as required) but its
  literal string is collapsed to an enum rather than retained, and a `1.x`
  `metadata.version` is rejected rather than read best-effort. Both are
  future-facing: they bite only when the spec adds a layout or ships `1.0`.
- **S8, caller CURIE ancestry** — the writer does not verify that a
  caller-supplied unit or type CURIE descends from the required CV parent. The
  syntactic shape is a CURIE; the ancestry check needs a loaded controlled
  vocabulary, which this library does not carry. The writer's own terms are
  correct by construction.

## How this was verified

Every FIXED row has a regression test that fails on the pre-fix code:
`test/files/list32.dir` (R3 lists), `test/files/uint32_index.dir` (R3 index +
strings), the NCIT-term run in `run_writer_test` (W3/A3), and the
non-ascending-index case in `parquet_writer_test` (S2). The full suite is green
in the release, debugoptimized and thread-sanitizer builds, and the peak-path
digests are unchanged (`run13k 5604ebcf86dd4567`, `run2k 9fae6e76aea6a57e`).
