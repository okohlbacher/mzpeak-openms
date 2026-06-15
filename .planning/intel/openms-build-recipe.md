# OpenMS Build & Link Recipe (Phase 0 → durable, used by Phases 2–5)

Established 2026-06-15 while completing Phase 0. Everything RDR-19 needs to compile,
build, and run against a local libOpenMS.

## Locations
- OpenMS source: `/Users/kohlbach/Claude/OpenMS` (branch `feature/mzpeak-file-handler`).
- Build dir: `/Users/kohlbach/openms_build` (CMake **Unix Makefiles**, **Release**, **gnu++20**, `BUILD_TESTING=ON`, 16 cores).
- **Symlink required:** `/Users/kohlbach/OpenMS -> /Users/kohlbach/Claude/OpenMS`. The CMake cache was
  configured against the old `~/OpenMS` path; the symlink keeps all 29+ baked absolute paths valid
  WITHOUT a full reconfigure (which would refetch `_deps`). Do NOT delete it.
- libOpenMS: `/Users/kohlbach/openms_build/lib/libOpenMS.dylib` (~44M).

## Cache fixes already applied (don't re-trip)
- `GIT_EXECUTABLE` and `GITCOMMAND` in CMakeCache.txt were stale (`/usr/local/bin/git`, gone) →
  repointed to `/usr/bin/git` (Apple Git 2.50.1). Empty `Git_VERSION` had broken FetchContent(opentims).
- Backup at `~/openms_build/CMakeCache.txt.bak`.

## Build commands
- Rebuild the library after editing OpenMS sources:
  `make -C /Users/kohlbach/openms_build -j16 OpenMS`  (incremental: recompiles changed files + relinks)
- Build a single class test (target = test exe name):
  `make -C /Users/kohlbach/openms_build -j16 MzPeakFile_test`
- `make` auto-reruns `cmake_check_build_system`, so edits to `sources.cmake` / `executables.cmake`
  are picked up automatically.

## Standalone compile/link of a program against libOpenMS
Include flags come from the build's own flags.make (avoids guessing the long -I/-isystem list):
```bash
grep -m1 '^CXX_INCLUDES' ~/openms_build/src/openms/CMakeFiles/OpenMS.dir/flags.make \
  | sed 's/^CXX_INCLUDES = //' > /tmp/oinc.rsp     # @response file (do NOT pass unquoted — word-splitting breaks it)
c++ -std=gnu++20 prog.cpp @/tmp/oinc.rsp \
  -L ~/openms_build/lib -lOpenMS \
  -Wl,-rpath,$HOME/openms_build/lib -Wl,-rpath,/opt/homebrew/lib -Wl,-rpath,/Library/Frameworks \
  -o prog
```
- Runtime needs `libcurl.framework` at `/Library/Frameworks` (the 3rd rpath), else dyld fails to load
  libOpenMS. cmake-built test exes already carry the right rpaths; for manual runs add
  `DYLD_FRAMEWORK_PATH=/Library/Frameworks` if needed.

## Handler integration points (where a new format wires in)
- Header: `src/openms/include/OpenMS/FORMAT/MzPeakFile.h` (`class OPENMS_DLLAPI`, `#pragma once`,
  `$Maintainer$/$Authors$: Oliver Kohlbacher`).
- Source: `src/openms/source/FORMAT/MzPeakFile.cpp` (registered in `source/FORMAT/sources.cmake`).
- FileTypes: `FORMAT/FileTypes.h` enum `MZPEAK` + `FORMAT/FileTypes.cpp` `TypeNameBinding(..., "mzpeak", ...)`.
  NOTE: `type_with_annotation__` is linear-searched (entries self-identify), so enum/array order need
  not match — but adding a type bumps `SIZE_OF_TYPE`, so `FileTypes_test.cpp` hardcoded counts must be updated.
- Extension detection: automatic via `FileTypes::nameToType("mzpeak")`. Content detection (zip whose first
  member is `mzpeak_index.json`) hooks into `FileHandler::getTypeByContent` (Phase 2).
- Test: `src/tests/class_tests/openms/source/MzPeakFile_test.cpp` + register in
  `class_tests/openms/executables.cmake` `format_executables_list`. Test data dir:
  `src/tests/class_tests/openms/data/` via `OPENMS_GET_TEST_DATA_PATH(...)`.

## C++20 port (PORT-01)
`null_fill` + numpress wrapper compile clean under `-std=c++20` (no C++23-only features); their Boost.Test
kernel tests pass. These cores get copied into OpenMS for the native handler (the full mzpeak lib stays C++23).
