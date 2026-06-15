---
phase: 02-mzpeakfile-load-registration
plan: 02
type: summary
depends_on: ["02-01"]
requirements: [INT-03]
---

# 02-02 SUMMARY — METADATA layer of MzPeakFile::load (M2 CV dispatch)

Extended the working 02-01 peak load (zip+parquet read, point decode, null
reconstruction, sort+updateRanges) with the PSI-MS metadata mapping, mirroring
the `MzMLHandler::handleCVParam_` accession→setter subset that mzPeak emits
(integration-plan §4a M2). No rewrite — purely additive.

## What was mapped

### Per-spectrum scalars (spectra_metadata.parquet `spectrum` struct)
- `MS_1000525_spectrum_representation` (MS:1000127 / MS:1000128) →
  `MSSpectrum::setType(CENTROID/PROFILE)`. NOTE: `MS_1000559_spectrum_type`
  carries MS:1000579/1000580 (MS1/MSn spectrum), **not** the
  centroid/profile term — representation is the correct source.
- `MS_1000465_scan_polarity` (int8: +1/−1) →
  `InstrumentSettings::setPolarity(POSITIVE/NEGATIVE)`.
- `id` (e.g. `controllerType=0 controllerNumber=1 scan=3`) kept as the
  `mzpeak_native_id` meta value; the stable `index=N` native id from 02-01 is
  retained (existing lookups + assertions stay valid).
- per-spectrum `parameters` (flat CvParam list) → `applyCVParamToSpectrum_`:
  recognized → typed setter; unrecognized → accession-keyed `setMetaValue`
  (mirrors `handleUserParam_`); null-accession params kept name→value. (Empty
  in this fixture, but the path is wired and exercised structurally.)

### Precursors (RDR-10c precursor / selected_ion facets)
mzPeak stores `precursor` and `selected_ion` as **top-level struct columns
row-aligned separately from the `spectrum` column**, joined on `source_index`
(= spectrum index) and `precursor_index`. `readPrecursors_` builds that join
into a map `source_index → vector<PrecursorData>`, then merges into the
per-spectrum meta.
- isolation window as **OFFSETS**: `setMZ(MS:1000827 target)`,
  `setIsolationWindowLowerOffset(MS:1000828)`,
  `setIsolationWindowUpperOffset(MS:1000829)`.
- activation params → `applyActivationCVParam_`: MS:1000133 CID (+ PD/PSD/SID/
  BIRD/ECD/IMD/SORI/HCD/ETD/PQD/TRAP) → `getActivationMethods().insert(...)`;
  MS:1000045 collision energy / MS:1000509 activation energy →
  `setActivationEnergy(double)`.
- selected ion: MS:1000744 m/z → precursor m/z (overrides isolation target,
  which is then preserved as `"isolation window target m/z"` meta value, per
  the mzML selected-ion convention); MS:1000041 → `setCharge`; MS:1000042 →
  `setIntensity`. **One OpenMS `Precursor` per selected ion** — no silent drop.

### Run-level metadata (mzpeak_index.json `metadata{}` → ExperimentalSettings)
`applyRunMetadata_` (best-effort; malformed blocks never abort the peak load):
- `run.start_time` (ISO 8601) → `setDateTime`; `run.id` → `mzpeak_run_id` meta.
- `instrument_configuration_list[0]` → `setInstrument(Instrument)`: model term
  → `setName`; serial number MS:1000529 → meta value; `components` →
  IonSource/MassAnalyzer/IonDetector with `order` set and component CV
  accession kept as a meta value (these hosts are MetaInfoInterface, not
  CVTermList).
- `file_description.source_files` → `setSourceFiles`: name/location +
  `applySourceFileParam_` (MS:1000569 SHA-1 / MS:1000568 MD5 → `setChecksum`;
  MS:1000563/MS:1000584 → `setFileType`; MS:1000768 → native-id type+accession;
  others → `addCVTerm`, since SourceFile IS-A CVTermList).
- `sample_list[0]` → `setSample` (name + id→number).
- `software_list[0]` → `Instrument::setSoftware` (id→name, version, params →
  `addCVTerm`; Software is CVTermList). Software has no run-level home in
  OpenMS, so it is attached to the instrument per §5/§4.

base_peak / TIC: not added as fields (OpenMS computes via getBasePeak/
calculateTIC) — per plan.

## Accession subset covered
Spectrum: MS:1000127, MS:1000128, MS:1000129, MS:1000130 (+ accession-keyed
fallback). Activation: MS:1000133/134/135/136/242/250/262/282/422/598/599,
MS:1002472, MS:1000045, MS:1000509. Selected ion / isolation: MS:1000744,
MS:1000041, MS:1000042, MS:1000827/828/829. Source file: MS:1000569, MS:1000568,
MS:1000563, MS:1000584, MS:1000768. Instrument: MS:1000529 + named model term.
(Subset of the ~332-accession MzMLHandler table — only what mzPeak emits.)

## Ground truth asserted (small.mzpeak, pyarrow-verified)
- type: index 0 PROFILE, index 2 CENTROID; polarity POSITIVE on both; native id
  non-empty; `mzpeak_native_id` present.
- precursor (source_index 2): m/z ≈810.789428 (selected ion), isolation offsets
  1.0/1.0, CID present, collision energy 35.0, selected-ion intensity
  ≈1994039.125, isolation-target meta value present.
- run-level: instrument name non-empty (LTQ FT), ion sources non-empty,
  source file `small.RAW` with non-empty SHA-1 checksum, instrument serial
  number meta value present (unrecognized-CvParam-as-meta-value check),
  dateTime non-null (2005-07-20T19:44:22Z).

## Build / test result
- `make OpenMS` + `make MzPeakFile_test`: compile + link clean.
- `MzPeakFile_test`: **PASSED** (all 02-01 + 02-02 assertions; verified via
  OPENMS_TEST_VERBOSE=True — every line `passed`).
- clang-format diff-verify (not --dry-run): `MzPeakFile.cpp` and
  `MzPeakFile_test.cpp` CLEAN; no trailing whitespace; final newlines OK.

## Commit hashes
- OpenMS (`feature/mzpeak-file-handler`): `3326808` — metadata layer + test.

## Deferrals (per plan)
- Ion mobility (NULL in fixture), wavelength spectra, chromatogram metadata,
  auxiliary_arrays, scan-context params, M1 shared-helper refactor (Phase 3b).
- MS:1000045 mapped to `setActivationEnergy` (plan-directed) rather than the
  meta-value-only behaviour MzMLHandler uses for that accession — the only
  approximation vs the mzML reader; documented here.

## Self-Check: PASSED
