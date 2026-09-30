# Specification compliance

Audited against `HUPO-PSI/mzPeak-specification` at `85442bd`
(`docs/conformance.md`), as both a conformant reader and a conformant writer.
Re-checked against `d0c16b3` (2026-09-11), which formalised `term_marker`,
raised column statistics from optional to required, and moved the example
`scan_start_time` from float32 to double; again against `e5e9021`
(2026-09-22), which made a SHA-512 checksum per indexed file a MUST and
extended the page-index requirement to cover row group statistics; and again
against `840ba4a` (2026-09-29), which specified grid encoding, added the
optional Imaging profile, and tightened what a `cv_list` URI must point at.  The deltas
are folded into the rows below and into *Known deviations*.
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
   after (`test/files/list32.dir`).  Note that `list32.dir` was copied from
   `small.dir` while that was still the legacy nested layout; upstream has since
   regenerated `small.dir` into the split layout, so the fixture is nested and
   the comparison crosses layout as well as list width.
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
| W2 page index AND column statistics for index/coordinate columns | **OK** | `enable_write_page_index()` and `enable_statistics()` sit together in the single choke point for every Parquet file written (`parquet_writer.cpp`). `d0c16b3` raised statistics from "when present" to a writer MUST; this already satisfied it. |
| W3 declare every CV used, with a URI identifying a fixed release | **PARTIAL** — see *Known deviations* | `cv_list` was hardcoded MS+UO regardless of content. It is now derived from every CURIE prefix reachable in the finished index; MS and UO are always present (the array index and column mappings use them); each prefix is pinned from a registry of the vocabularies a mass-spec archive plausibly cites. An unpinnable prefix is still declared with a visible placeholder and a warning, never silently omitted. |
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
- **R2, `binary` == `large_binary`** — the reader implements the required
  `list`/`large_list` and `string`/`large_string` equivalences but NOT
  `binary`/`large_binary`; no such handling exists in `src/`. This is currently
  unexercised rather than broken: no bundled fixture and no member of the
  present Arrow schemas uses a binary column at all (verified by scanning the
  schema of every fixture Parquet file), because the one place raw bytes appear
  -- an auxiliary array's `data` -- is a list, not a binary column. It becomes
  real the moment the schema gains a binary field.
- **R5, isolation-window terms beyond the three preferred ones** — the typed
  `IsolationWindow` fields resolve `MS:1000827` (target m/z), `MS:1000828`
  (lower offset) and `MS:1000829` (upper offset), which are the three the
  specification names for that group. The *deprecated* limit terms
  `MS:1000793`/`MS:1000794` and the no-isolation flag `MS:1003159` are also
  children of `MS:1000792` (isolation window attribute), so a conformant archive
  may carry them instead. They reach the caller through
  `IsolationWindow::parameters` rather than a typed field: nothing is lost, but a
  caller computing window bounds from the typed fields alone sees an empty
  window rather than an error. `mzdata` added handling for exactly these terms
  in `58e509b` (2026-09-10), which is evidence that real files carry them.
- **`term_marker`, both forms — FIXED** (was a gap when `d0c16b3` landed). A
  marker column states that a term APPLIES to a row rather than carrying its
  value, and the terms live in sibling COLUMNS of `parameters` rather than
  inside the list, so every pass that walked the list missed them. Both forms
  now reach `parameters`: a boolean column contributes the mapping's own
  accession when true, and a string column contributes the CURIE it carries,
  which is a CHILD of the mapping's accession. The child's NAME is left unset,
  because resolving a CURIE to a name needs a loaded CV this library does not
  carry. Terms already surfaced as typed fields (`MS:1000525`, `MS:1000559`,
  `MS:1000465`) are skipped so a writer that flags a standardised column does
  not get it reported twice. Pinned by `test/term_marker_test.cpp` against
  `test/files/term_markers.dir`, which was built for this because nothing in
  either implementation's output carries such a column; the test fails if the
  feature is removed. Residual limitation: a mapping is matched to a facet by
  the LAST component of its path, so the same leaf name on two facets of one
  file would attach the term to both.
- **SHA-512 checksums — FIXED, both sides.** `e5e9021`
  made a lowercase, separator-free SHA-512 digest per indexed file a MUST (the
  ZIP container is explicitly excluded, since packed and unpacked archives are
  equally valid). All four writer paths stamp one, and the reader verifies on
  request: `MzPeak::open(path, Validate::Checksums)` refuses a mismatch and
  `Index::verify_checksums()` reports one instead. Verification is opt in
  because it costs a full pass over every indexed member. The report separates
  `verified` from `unchecked`, so an archive predating the requirement -- which
  passes having proved nothing -- stays distinguishable from one actually
  checked; such an archive is not refused, since rejecting everything older than
  the rule would be a worse reader rather than a stricter one. Semantics were
  confirmed
  against the reference rather than inferred -- all ten digests in
  `small.unpacked.mzpeak` reproduce as a plain SHA-512 over each file's raw
  bytes, no salt, no header exclusions -- and `test/sha512_test.cpp` recomputes
  that same digest so the two implementations are checked against each other.
- **Provenance and column-chunk checksums — not an obligation yet.** The
  prototype gained per-column-chunk SHA-512 digests and a `write_provenance_table`
  (`85ff6af`), which is the start of the specification's draft *Provenance*
  section. Nothing calls that writer, the specification's index schema declares
  no place for the table, and the section itself still says only that the file
  "MUST be encrypted using a proprietary secret key". So there is nothing to
  implement against and no member for a reader to encounter; recorded here only
  so the mechanism's location is known when it is formalised.
- **`cv_list` URIs do not identify a fixed release.** `840ba4a` changed W3 from
  "version-pinned" to requiring a `uri` that "identifies a fixed release or
  snapshot and the matching `version`". This writer emits the *latest* artifact
  URI (`http://purl.obolibrary.org/obo/uo.owl`) beside a pinned `version`, so
  the two drift apart as the ontology is re-released: the version says 2023 while
  the URI resolves to whatever is current. The reference emits
  `http://purl.obolibrary.org/obo/ms/4.1.249/psi-ms.obo` and
  `http://purl.obolibrary.org/obo/uo/releases/2026-01-16/uo.obo`. Note the two
  patterns DIFFER -- `<id>/<version>/<file>` versus `<id>/releases/<date>/<file>`
  -- so this cannot be fixed by deriving one URL template; each vocabulary's
  release layout has to be recorded, and a guessed URL that 404s is worse than
  today's honest-but-unpinned one.
- **Grid encoding (`MS:1003826`) — IMPLEMENTED, reading.** A chunk
  encoding, chosen PER CHUNK: `chunk_values` is null or empty and the
  coordinates are integer indices into a model carried in a sibling
  `<array>_grid` struct. All four models decode -- `MS:1003824` linear,
  `MS:1003825` square root, and the Bruker models the reference carries under
  the placeholder accessions `MS:9999002` (timsTOF m/z) and `MS:9999001` (TIMS
  mobility). Main-axis indices are delta-coded, secondary ones absolute.

  *Mixed dimensions.* The specification tells a writer that a model whose
  error exceeds its threshold "SHOULD fall back to use a different encoding",
  so one dimension may hold grid rows beside uncompressed, delta or Numpress
  ones. Every row is decoded in order: grid rows through the model, plain rows
  through exactly the transform the ordinary chunk decoder applies. On a
  secondary axis a row is grid when its grid struct is present; a row carrying
  both a plain list and a grid is refused. (An earlier version refused any
  non-grid main-axis row, and on a secondary axis silently DROPPED the grid
  rows when a plain column was also present.)

  *Bounds.* Every grid row's first and last value is checked against the
  `chunk_start`/`chunk_end` it recorded, with a tolerance of one grid step. The
  specification calls grid encoding "likely to be a lossy transformation" and
  does not say the bounds are the model's evaluation, so a writer may record
  the original coordinate, which snapping to the grid moves by at most half a
  step; wrong models are off by many. Non-finite coordinates are refused, and
  a missing bound column is skipped rather than read.

  *Arithmetic.* There is no single reference to be bit-identical to: the grid
  codec `grid.rs` fuses the flight-time multiply-add, while mzdata 0.66.7's
  scalar `convert_f64` does not, and the two differ in the last bit on about a
  third of timsTOF coordinates. This follows `grid.rs`, because that is what
  writes grid archives and so what their recorded bounds were computed with.
  Implicit contraction is disabled per function for both Clang and GCC -- GCC
  contracts by default even in ISO mode -- and verified on each: with the
  guard, GCC 16.2 `-O2` on arm64 reproduces the reference exactly; without it,
  it emits ten fused instructions and diverges. The tests assert EXACT
  equality, so a contracting build fails them.

  *Pre-fix archives.* Two reference writer bugs, since fixed upstream, are
  read rather than refused: a one-point chunk whose `chunk_end` fell through to
  0.0, and a `MS:9999001` pair written as `[c6, c7, slope, offset]`. The first
  is excused only on one-point chunks; the second is resolved by magnitude
  under stated physical assumptions and every result is checked against a
  physical fence, so a misreading is refused rather than returned.

  Validated on a real Bruker diaPASEF conversion (C2 != 0, C4 != 0): ion
  mobility and intensity bit-identical on all 291,453 points; m/z within one
  ulp on 151 of them, which is the fused/unfused split above. Unimplemented:
  WRITING grid archives, and the materialised grid form.
- **Imaging profile — ignored, as a Core reader must.** `840ba4a` added it,
  declared by `metadata.imaging.is_imaging`. Conformance requires a Core reader
  to read Core content and ignore profile content it does not implement.
  Verified rather than assumed: declaring the profile on a fixture, with a
  `grid_geometry` block beside it, changes nothing about what this reader
  returns.
- **S8, caller CURIE ancestry** — the writer does not verify that a
  caller-supplied unit or type CURIE descends from the required CV parent. The
  syntactic shape is a CURIE; the ancestry check needs a loaded controlled
  vocabulary, which this library does not carry. The writer's own terms are
  correct by construction.

## Re-checked against the specification at `85442bd`

Spot-checks made directly against `spec/docs/conformance.md`, recorded so they
are not re-derived:

- **"readers MUST NOT depend on member names other than `mzpeak_index.json`"** —
  holds. Every hard-coded member name in this tree is in `src/writer.cpp`, where
  a writer must choose names; the reader resolves entities by
  `entity_type`/`data_kind` from the index. Worth stating because upstream's own
  `Index::spectra()` and `Spectrum` look up `"spectra_data.parquet"` and
  `"spectra_metadata.parquet"` literally, which this requirement forbids; that
  is one reason this tree did not adopt them.
- **"mzPeak files containing only metadata are still legal archives"** — holds:
  an archive with the signal member removed from the index reports zero spectra
  and does not throw.
- **"any of the Parquet files MAY be empty but present ... readers must
  gracefully handle"** — holds: with every member rewritten to zero rows the
  reader returns empty arrays without error.
- **Time in minutes (`UO:0000031`)** — holds; the seconds conversion is pinned
  by independent literals in `spectrum_metadata_test`, `query_api_test`,
  `wavelength_spectra_test` and `chromatograms_test`.

## How this was verified

Every FIXED row has a regression test that fails on the pre-fix code:
`test/files/list32.dir` (R3 lists), `test/files/uint32_index.dir` (R3 index +
strings), the NCIT-term run in `run_writer_test` (W3/A3), and the
non-ascending-index case in `parquet_writer_test` (S2). The full suite is green
in the release, debugoptimized and thread-sanitizer builds, and the peak-path
digests are unchanged (`run2k 9fae6e76aea6a57e`; the `run13k` fixture has since
been regenerated and re-baselined to `5e86e69f2c9b3101` -- see
`docs/read-path-performance.md`).
