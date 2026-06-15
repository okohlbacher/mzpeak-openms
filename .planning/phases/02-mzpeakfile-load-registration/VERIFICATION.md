# Phase 2 — MzPeakFile::load + Registration — VERIFICATION

**Verdict: PASS (4/4 success criteria)** — verified hands-on by the orchestrator (each plan's test run independently; full gate re-run).
**Date:** 2026-06-15 · **Requirements:** INT-01, INT-02, INT-03, INT-04
**Code:** OpenMS branch `feature/mzpeak-file-handler` (commits b8d379d, 3326808, 37c44c9).

## Success criteria

1. **load round-trips a fixture into MSExperiment (INT-01)** — PASS.
   `MzPeakFile().load(small.mzpeak, exp)` → 48 spectra (14 PROFILE/ms1 + 34 CENTROID/ms2). Profile spectrum 0:
   13589 pts, null-marked m/z reconstructed (no interior zeros), strictly ascending, first/last m/z 202.607/1999.840,
   intensity[1]=1938.12. Centroid spectrum: peaks match pyarrow within tol. RT in seconds; `sortByPosition` +
   `updateRanges` applied. (`MzPeakFile.cpp` ~1041 lines.)

2. **FileHandler resolves .mzpeak by extension AND content; getType==MZPEAK (INT-02)** — PASS.
   `getTypeByFileName("x.mzpeak")==MZPEAK` (Phase 0 registration); `getTypeByContent(small.mzpeak)==MZPEAK`
   via ZIP-magic guard + `mzpeak_index.json` membership check (not first-member — first member is
   spectra_data.parquet); `FileHandler().loadExperiment(small.mzpeak, exp)` → 48 spectra.

3. **CV dispatch: recognized→typed setters, unrecognized→setMetaValue; run-level→ExperimentalSettings (INT-03)** — PASS.
   M2 mirror of MzMLHandler::handleCVParam_ subset: type (MS:1000127/128 via MS_1000525 representation), polarity
   (MS:1000129/130), native id; precursors with isolation OFFSETS (target 810.789, ±1.0), activation (CID +
   collision energy 35), every selected ion mapped (mz 810.789428, intensity 1994039.125); unrecognized →
   accession-keyed meta value; run-level run/instrument(LTQ FT)/source-file(small.RAW + SHA-1)/sample/software →
   ExperimentalSettings.

4. **MzPeakFile_test passes in the OpenMS harness (INT-04)** — PASS.
   `MzPeakFile_test`, `FileHandler_test`, `FileTypes_test` all PASS (100%) with
   `DYLD_FRAMEWORK_PATH=/Library/Frameworks`. (Bare ctest aborts on a pre-existing libcurl dyld rpath quirk that
   affects ALL OpenMS tests — documented in intel/openms-build-recipe.md — not a Phase 2 defect.)

## Deferrals (documented, not gaps)
- Chunked + numpress layouts (small.chunked/small.numpress), chromatogram + auxiliary-array + ion-mobility metadata
  → Phase 4 / fixture-gated. Point-layout small.mzpeak is the Phase 2 load fixture.
- M1 shared applyCVParam refactor → Phase 3b (optional).
- Style: new OpenMS files (MzPeakFile.*, test) are clang-format-clean; edits to legacy FileHandler.cpp/FileTypes.*
  match their pre-existing hand-written style (those files predate the repo .clang-format; wholesale reformat is
  out of scope for an upstreamable PR).

## Cross-phase note for Phase 5
small.mzpeak's stored `time` (e.g. ~0.0049 s for scan 1) differs from the original small.mzML scan-1 RT (900.092 s) —
it is a different conversion/provenance. The reader follows the no-unit-conversion constraint. Account for this when
building the Phase 5 mzML→mzpeak→mzML cross-validation (use a consistently-generated fixture, not the legacy small.mzML).
