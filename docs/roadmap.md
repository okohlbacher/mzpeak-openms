# mzPeak C++ — Consolidated Roadmap (reader + writer)

One ordered, dependency-aware plan unifying the reader gaps (RDR-1…25) and the
writer phases.  Those two source documents -- `reader-backlog.md` and
`writer-implementation-research.md` -- were working notes and are not part of
this repository, so the RDR-nn numbering used below is defined only by the
prose here.
Each phase is a self-contained, testable increment validated by the
forward/reverse + cross-impl harness ([e2e-testing.md](e2e-testing.md)).

## Done (merged to `trunk`)
- **RDR-1** small/empty Parquet open fix (Arrow buffer).
- **Writer P0/P1a/P1b** point directory + zip-STORE archive + spectra_metadata → **T2 cross-impl PASS** (Rust reads C++ output).
- **e2e harness** T1 (forward intra), T2 (forward cross), T3 (reverse intra).
- **Split-metadata ("v2") layout, reader and writer** — both generations read
  through one code path; the writer emits the split layout and the current Rust
  reference reads it. **T2 and T5 both PASS.** See
  [e2e-testing.md](e2e-testing.md#split-metadata-layout-the-v2-layout).
- **Reader correctness** (each validated against the Rust reference):
  null-marked m/z now reconstructed from its own adjacent run (was ~2.15 Da out
  on every profile spectrum); null-marked intensity reads as 0 rather than
  being delta-interpolated into negative values; the renamed
  `wavelength_spectrum` entity is recognised again.
- **Thread safety** — lazy peak decode serialised with `std::call_once` and
  shared across copies; ThreadSanitizer reports 0 races.

## Resolved: the imaging point-count difference was a harness bug

`Example_Processed.img.mzpeak` appeared to decode 2837 points against the
reference's 3007. It does not. The reference example writes an MGF block
*before* its `Raw Data:` section, and the comparison script skipped only one
line — so 170 MGF header lines were being counted as data points. Extracting
from `Raw Data:` onward gives 2837 on both sides, with m/z agreeing to 6.1e-05
(the imaging file stores float32 coordinates).

Only that fixture is affected: for `small.mzpeak` the reference emits
`Raw Data:` as its first line, so the earlier point/chunked comparisons against
it stand unchanged.

## Fixed since the review

- Multi-scan spectra report their earliest scan (92b8372).
- `Spectra` is non-copyable/non-movable, so its fetch callback cannot be left
  bound to another object (b599ba4).
- Decoded cardinality is checked: m/z against intensity, and both against the
  count the file declares for itself.
- Mixed collections size from BOTH tables, so centroid-only tail spectra are no
  longer omitted.
- Null reconstruction operates on the whole entity in the point layout too, not
  per Arrow record batch. Point and chunked decoding are now bit-identical, and
  the reconstructed-point error against the reference fell from 2.5e-06 to
  8.5e-07 Da.
- Unrecoverable null shapes are rejected instead of synthesised: a lone
  interior null is refused, per the spec's rule that unpaired nulls may appear
  only as the first or last value of an array.
- Coalesced point columns refuse a row where two sibling columns both carry a
  value; they may be in different units and preferring one would be a guess.
- `EnumerableProxy::Iterator` no longer declares defaulted move operations
  taking `const&&`, which is ill-formed and made the header fail to compile on
  GCC.
- `chunk_end` validation tightened to 1e-9 relative and extended with
  start <= end and chunk-ordering checks.

## Validated against real vendor data (2026-09-24)

A real Bruker diaPASEF acquisition finally became available (prototype
`test/data/diaPASEF.d/`), and converting it through the reference writer
retired the larger half of the standing backlog.  `test/files/diapasef.dir` is
two MS2 frames from it, trimmed and committed; `test/ion_mobility_test.cpp`
pins the results.  Every expected value was read out of the Parquet with
pyarrow BEFORE this reader was pointed at it.

- **Ion mobility — VALIDATED.** The per-peak array is present, parallel to m/z
  in every spectrum (8377 and 960 peaks), and matches ground truth to the last
  digit. `SelectedIonInfo::ion_mobility_value` and `_type` read correctly
  (1.2517460582524271, `MS:1002815`) -- they had been null in every bundled
  fixture, so this is their first real exercise.
- **Per-window mobility LIMITS — still unexercised, and now known to be rarer
  than assumed.** This acquisition records a mobility VALUE per selected ion
  and no limits at all, so `ion_mobility_lower_limit`/`_upper_limit` correctly
  read as absent. The README's description of a frame as one spectrum carrying
  several windows over disjoint mobility ranges does not describe this file:
  the converter emits one spectrum per window, each with a single precursor.
  A file that genuinely uses limits is still needed.
- **Bruker TDF "ims-compact" — STILL NOT VALIDATED.** The conversion declares
  no `ims_calibration` and stores explicit m/z, so the `(a + b*tof)^2`
  reconstruction never ran. Having a real `.d` is not sufficient; the converter
  has to be asked for the compact layout, or a vendor archive already in it has
  to be found. This is the one item the new data did NOT unblock.

## Backlog

- **Grid encoding is not supported.** The prototype added a `grid` buffer
  encoding (`e62e18c`) beside `point` and `chunk`. `group_name_to_layout` maps
  it to `Layout::Unknown` and the decoder raises `UnknownLayoutError`, so such
  an archive is refused cleanly rather than misread -- unstarted work, not a
  latent defect. Worth starting only against an archive that actually uses it;
  implementing from the prose alone would be guessing.
- **clang-format version skew makes the repository-wide style rule unsafe to
  apply.** Under clang-format 22.1.8, 48 of 151 tracked C++ files disagree with
  the repository's own `.clang-format`, so running `clang-format -i` on a file
  merely because it was edited produces a large diff of unrelated churn -- and
  would fight whichever version the tree was last formatted with. Current
  practice is to format files created new in full, and in existing files to
  check only the lines actually added. The fix is to pin the version the
  project formats with and then do ONE deliberate tree-wide pass; doing the
  pass without pinning first just moves the problem.
- **Promoted value columns outside the activation facet.** Activation is now
  read (it was the only facet with no typed fields at all), but the mechanism
  is deliberately scoped to it. A general version needs a complete list of the
  accessions this reader already surfaces typed, and a single omission from
  that list would report a term twice.
