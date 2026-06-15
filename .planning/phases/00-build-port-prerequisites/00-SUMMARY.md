# Phase 0 — Build & Port Prerequisites — SUMMARY

**Status:** COMPLETE (2026-06-15)
**Requirements:** BUILD-01, PORT-01
**Executed operationally** (no formal PLAN.md — Phase 0 is a build/port chore, not code authorship in this repo). Code artifacts live in the OpenMS tree on branch `feature/mzpeak-file-handler`.

## Self-Check: PASSED

## What was delivered

### BUILD-01 — libOpenMS builds + is linkable ✅
- Built `libOpenMS.dylib` (~44M) at `~/openms_build/lib/` via `make -j16 OpenMS` (Unix Makefiles, Release, gnu++20).
- Unblocked a stale configuration first: CMake cache pointed at the old source path `~/OpenMS` (source had moved to `~/Claude/OpenMS`) and had a dead `GIT_EXECUTABLE=/usr/local/bin/git`. Fixed via a `~/OpenMS -> ~/Claude/OpenMS` symlink (preserves the partial build) + repointing `GIT_EXECUTABLE`/`GITCOMMAND` to `/usr/bin/git`.
- Proved linkable: a trivial program (`MSExperiment`/`MSSpectrum`) compiles, links against `-lOpenMS`, and runs → `OpenMS link OK: spectra=1 rt=1.5`. Recipe captured in `.planning/intel/openms-build-recipe.md`.

### PORT-01 — reused cores compile under C++20 ✅
- `src/util/null_fill.cpp` + `src/util/numpress.cpp` (+ vendored `MSNumpress.cpp`) compile clean under `-std=c++20`; no C++23-only features.
- Their Boost.Test kernel tests pass under C++20 (`null_fill_test` 3/3, `numpress_test` 3/3, no errors).

### Criterion 3 — MzPeakFile_test stub compile-links against libOpenMS ✅
On OpenMS branch `feature/mzpeak-file-handler` (commits `3c02aad`, `869eb60`):
- `FORMAT/MzPeakFile.h` + `.cpp`: `class OPENMS_DLLAPI MzPeakFile` with `load`/`store`/`transform` stubs throwing `Exception::NotImplemented` (Phase 2 implements `load`). Registered in `source/FORMAT/sources.cmake`.
- `FileTypes`: added `MZPEAK` enum + `TypeNameBinding(..., "mzpeak", "mzPeak peak file", {PROVIDES_EXPERIMENT, READABLE, WRITEABLE})`; bumped `FileTypes_test` counts (still green).
- `MzPeakFile_test.cpp` (registered in `class_tests/openms/executables.cmake`): ctor/dtor sections + `TEST_EXCEPTION(Exception::NotImplemented, load(...))`. libOpenMS rebuilt with `MzPeakFile.cpp`; `make MzPeakFile_test` compiles+links+runs → **PASSED**.

## Key facts for downstream phases
- Build/link recipe + integration points: `.planning/intel/openms-build-recipe.md`.
- The native OpenMS `MzPeakFile` handler scaffold now exists; Phase 2 fills `load`, Phase 3 fills `store`, Phase 4 fills `transform`.
- OpenMS-tree work is committed to `feature/mzpeak-file-handler` (separate repo from this `.planning/` which lives in `openms-mzpeak`).

## Deviations
- `FileTypes_test.cpp` hardcoded type counts required updating (READABLE 44→45, total 67→68) — mandatory, not optional, when adding a FileTypes enum value.
