# mzPeak C++ — End-to-End Forward/Reverse Validation

How we validate the reader and the writer against each other and against the
Rust reference implementation. The guiding principle (from the writer research):
**validate semantically (decode and compare values within tolerance), not by
byte-diffing Parquet** — parquet-cpp and parquet-rs differ in `created_by`,
ZSTD streams and page layout, so byte-identity is neither achievable nor the
goal.

## The validation matrix

| # | Name | Pipeline | Oracle | Validates | Status |
|---|------|----------|--------|-----------|--------|
| **T1** | Forward (intra) | C++ write → C++ read | input data | writer+reader agree | **DONE** — `writer_test`, `archive_writer_test` |
| **T2** | Forward (cross) | C++ write → **Rust** read | Rust reference | C++ writer **conformance** | **HARNESS UP**; currently blocked on the metadata table (see below) |
| **T3** | Reverse (intra) | C++ read ref → C++ write → C++ read | first read | reader↔writer idempotence on real data | **DONE** — `roundtrip_test` |
| **T4** | Reverse (cross) | Rust write (bundled files) → C++ read | pyarrow ground truth | C++ reader **conformance** | partial — done ad hoc via pyarrow; blocked on reader gaps (chunked/peaks/null) |
| **T5** | Full pipeline | mzML → Rust mzpeak → C++ read → C++ write → Rust read → mzML | original mzML | whole stack | future (needs writer P1+ and reader G1/G3/G4) |

T1+T3 run in the normal `meson test` suite. T2/T4 need the Rust toolchain and
are run via the script below (and, longer term, an opt-in CI job).

## T2 — the cross-implementation oracle (and what it found)

The Rust reference can *read* an mzPeak via its `MzPeakReader` (example
`read_spectrum`):

```bash
cd ../hupo-mzpeak
cargo build --release --example read_spectrum         # ~minutes first time
target/release/examples/read_spectrum <file.mzpeak> <index>   # prints raw m/z+intensity
```

Note: the `convert` example is mzML→mzpeak **only** (it cannot read mzpeak), so
`read_spectrum`/`read` are the reader oracles.

**Finding (2026-06-12):** feeding a C++-written `.mzpeak` to the Rust reader
fails with `Spectrum metadata entry not found`. The Rust `get_spectrum(index)`
resolves a `SpectrumDescription` from **`spectra_metadata.parquet`**, which the
Phase-0/1a C++ writer does not emit. The C++ reader reads spectra straight from
`spectra_data.parquet` and so doesn't need it — but a conformant file MUST carry
the metadata table. **Therefore the spectra_metadata writer (Phase 1b) is the
acceptance gate for T2**, not optional polish. Until then T2 is expected to
fail with that exact error, which is a useful regression signal.

## T4 — the reverse cross check (C++ reads Rust output)

The bundled `test/files/*.mzpeak` are all Rust-written, so reading them with the
C++ reader and comparing to pyarrow ground truth is the reverse cross check.
Today it passes only for point-layout profile spectra in a non-empty data table;
chunked/numpress/peaks/wavelength/null-marked data are reader gaps
(see `reader-completion-gaps.md` / `reader-backlog.md`). As those gaps close
(RDR-3/4/5/6), extend T4 to the chunked, numpress and has_uv fixtures.

## Running the cross-impl checks

`scripts/e2e_cross_impl.sh` automates T2: build the C++ archive writer, write a
small `.mzpeak`, build the Rust `read_spectrum`, and read it back, reporting
PASS/FAIL (currently FAIL with the metadata-table error above — by design until
Phase 1b).

## Design notes / invariants the harness asserts
- **Semantic compare** with float tolerance (1e-9 m/z, 1e-6 intensity), never
  byte-diff.
- **Per-spectrum m/z sorting**: the writer sorts each spectrum by m/z; reverse
  tests sort the reference the same way before comparing.
- **Structural conformance** (beyond values), to catch shared writer/reader
  bugs that round-trip clean: ZIP members STORED, `store_schema` present,
  page index + statistics written, `spectrum_array_index` + `spectrum_count`
  file-KV present, canonical column paths. (`archive_writer_test` checks STORE;
  extend with a structural-metadata assertion as Phase 1 lands.)
- **One byte-exact carve-out**: Numpress buffers vs `numpress-rs`, once chunked
  encoding exists (writer P3 / reader RDR-7).

## Current coverage snapshot
- T1 forward intra: PASS (point directory + zip archive).
- T3 reverse intra: PASS (Example_Processed.img, 9 spectra, read→write→read).
- T2 forward cross: harness ready; blocked on Phase 1b metadata table.
- T4 reverse cross: ad hoc pyarrow; blocked on reader gaps.
