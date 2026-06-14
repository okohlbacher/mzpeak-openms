# Phase 1: Reader Lossless-Map Prerequisites - Context

**Gathered:** 2026-06-14
**Status:** Ready for planning
**Source:** Ingest Express Path (docs/openms-integration-plan.md §7 + docs/reader-backlog.md)

<domain>
## Phase Boundary

Extend the **mzPeak C++ reader** (in the standalone `openms-mzpeak` library, on
branch `writer_test`, using the existing toolchain — NOT in OpenMS yet) to surface
the per-spectrum and per-array metadata that a lossless mzPeak→`MSExperiment` map
needs downstream (Phase 2). `spectra_metadata.parquet` already carries more than the
reader exposes; this phase closes that gap. Three requirements: RDR-10b, RDR-10c,
RDR-9b. No OpenMS code, no `libOpenMS` dependency — this is pure mzpeak-lib reader work.

</domain>

<decisions>
## Implementation Decisions

### RDR-10b — per-spectrum descriptive metadata (CRITICAL, unblocked)
- Extend `Util::read_spectra_metadata` (`src/util/metadata_model.cpp`) and the
  `SpectrumMetadata` struct (`include/mzpeak/spectrum_metadata.h`) to expose:
  per-spectrum **`parameters`** as a flat CV-param list, **`spectrum_type`**
  (`MS:1000559`), **lowest/highest observed m/z**, and **`data_processing_ref`**.
- Reuse the existing `CvParam` model from `RunMetadata` (`include/mzpeak/run_metadata.h`)
  for the per-spectrum parameter list — accession + name + value + unit — because the
  downstream consumer (Phase 2) feeds these straight into an `MzMLHandler::handleCVParam_`-
  style accession→OpenMS-setter dispatch. Do **not** invent a second param shape.
- Verify column names against the real `spectra_metadata.parquet` schema (pyarrow) before
  coding — mirror how the existing `read_spectra_metadata` resolves `MS_1000511_ms_level`
  etc.

### RDR-9b — auxiliary-arrays accessor (unblocked)
- Add a typed accessor exposing each spectrum's **`auxiliary_arrays`** (name/CV term +
  the float/secondary-intensity/wavelength values) so Phase 2 can map them to OpenMS
  `MSSpectrum::FloatDataArrays` (named + CV-annotated). Mirror how the point/coalesced
  decode already reaches secondary columns (encoding.h).

### RDR-10c — precursor / isolation / activation / ion mobility (BLOCKED on a fixture)
- Mirror the Rust reference's `SpectrumSource` precursor fields: precursor m/z, charge,
  selected ion(s), **isolation window** (store the absolute bounds the file carries — the
  offset-vs-absolute conversion is Phase 2's job), **activation** method/energy, and
  **ion mobility** (per-spectrum and/or per-peak).
- **Known blocker:** no bundled fixture has MSn precursor + ion-mobility columns (the
  bundled files are MS1/centroid/UV). Options the planner must choose between and surface:
  (a) generate an MSn+IM fixture via the Rust `convert` from a suitable mzML, or
  (b) DEFER RDR-10c to a follow-up and ship Phase 1 with RDR-10b + RDR-9b only (a v1
  converter then maps peaks + core scalars + run metadata + aux arrays, dropping
  precursor/IM — explicitly documented). Prefer (b) for this phase unless a fixture is
  cheaply obtainable; do not write unvalidated decode paths against no ground truth.

### Claude's Discretion
- Exact accessor signatures/names, struct layout, and whether `parameters` is a
  `std::vector<CvParam>` member on `SpectrumMetadata` vs a parallel map — planner decides,
  matching the repo's existing idioms (snake_case methods, trailing-underscore members).
- Test granularity, as long as every new field is asserted against pyarrow ground truth.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Integration design (why these fields are needed)
- `docs/openms-integration-plan.md` §4a (mzML-handler CV-dispatch template) and §7
  (reader prerequisites) — defines RDR-10b/10c/9b and what each must expose.

### Reader history + the code to extend
- `docs/reader-backlog.md` — RDR-10 (done: scalar metadata) and RDR-9 (done: wavelength)
  history; this phase is the "10b/10c/9b" extension.
- `src/util/metadata_model.cpp` (`read_spectra_metadata`, `read_mz_delta_models`) and
  `include/mzpeak/spectrum_metadata.h` (`SpectrumMetadata`) — the code to extend.
- `include/mzpeak/run_metadata.h` (`CvParam`) — the param type to reuse.
- `include/mzpeak/util/encoding.h` — secondary/aux column decode patterns.

### Validation method (mandatory, established)
- `docs/e2e-testing.md` — semantic ground-truth-vs-pyarrow validation (1e-9 m/z /
  1e-6 intensity); add per-field assertions on bundled fixtures.

</canonical_refs>

<specifics>
## Specific Ideas

- Ground truth: `small.mzpeak`'s `spectra_metadata.parquet` carries `parameters`,
  `spectrum_type`, observed-mz, `data_processing_ref` (verify via pyarrow first).
- Standing per-phase gates (from PROJECT.md): clang-format + diff-verify + editorconfig
  style pass on every edited file; cross-AI adversarial review of the plan; full
  `meson test` + `scripts/e2e.sh` green before the phase closes.

</specifics>

<deferred>
## Deferred Ideas

- RDR-10c precursor/IM decode is deferred within this phase IF no MSn+IM fixture is
  available (see decision above) — it then becomes a fixture-gated follow-up, and Phase 2
  ships a v1 converter without precursor/IM.
- Any OpenMS-side wiring — that is Phase 2.

</deferred>

---

*Phase: 01-reader-lossless-map-prerequisites*
*Context gathered: 2026-06-14 via Ingest Express Path*
