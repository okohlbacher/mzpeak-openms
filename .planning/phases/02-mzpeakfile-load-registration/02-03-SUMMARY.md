---
phase: 02-mzpeakfile-load-registration
plan: 03
type: summary
---

# 02-03 SUMMARY — Wire .mzpeak into OpenMS FileHandler

## What was wired
`.mzpeak` now resolves through the generic `FileHandler` API by extension AND by
content, and routes to the already-working `MzPeakFile::load`.

1. **Content detection** — `FileHandler::getTypeByContent`
   (`src/openms/source/FORMAT/FileHandler.cpp`): inside the existing
   `PK\x03\x04` ZIP-magic branch (cheap leading-magic guard first), the archive
   entries are enumerated via `ZipArchiveFile::listEntries(filename)` and we
   return `FileTypes::MZPEAK` iff a member named **`mzpeak_index.json`** exists.
   - Membership check, NOT first-member: the first archive member is the binary
     `spectra_data.parquet`, so a content-sniff of the first member would fail.
   - `listEntries` returns an empty vector on any libzip error, so non-mzpeak
     zips fall through unchanged to the existing first-member text sniff. Other
     zip-based formats are unaffected (we only short-circuit when the index
     member is present).
   - Idiom mirrors the existing `.d.zip` Bruker handling in the same file.

2. **Load dispatch** — `FileHandler::loadExperiment`: added
   `case FileTypes::MZPEAK: { MzPeakFile().load(filename, exp); } break;`
   immediately after the `SQMASS` case, and added
   `#include <OpenMS/FORMAT/MzPeakFile.h>`. Store dispatch left for Phase 3
   (default case throws, build unaffected).

3. **Extension detection** — already worked from Phase-0 `FileTypes`
   registration (`MZPEAK, "mzpeak"`); verified via the new test assertion, no
   change needed.

## Content-detection approach (summary)
PK magic guard → `ZipArchiveFile::listEntries` → look for exact member
`"mzpeak_index.json"` → return `MZPEAK`. Falls through to existing logic
otherwise. Works even if the file is renamed without the `.mzpeak` extension.

## Tests
- **MzPeakFile_test.cpp**: new `[EXTRA]` section — `getTypeByFileName("x.mzpeak")
  == MZPEAK`, `getTypeByContent(small.mzpeak) == MZPEAK`, and
  `FileHandler().loadExperiment(small.mzpeak, exp)` → `exp.size() == 48`.
- **FileHandler_test.cpp**: added `getTypeByFileName("test.mzpeak") == MZPEAK`
  and `getTypeByContent(small.mzpeak) == MZPEAK` (fixture already in `data/`).

### Results (DYLD_FRAMEWORK_PATH=/Library/Frameworks)
| Test | Result |
|------|--------|
| MzPeakFile_test  | PASSED |
| FileHandler_test | PASSED |
| FileTypes_test   | PASSED |

libOpenMS built clean; all three test exes built clean.

## Count/table updates needed
None. `FileTypes_test` required NO count changes — the `MZPEAK` type was already
registered (with counts updated) in Phase 0. No hardcoded tables were affected.

## clang-format status
- **CLEAN (empty diff)**: `MzPeakFile.h`, `MzPeakFile.cpp`, `MzPeakFile_test.cpp`.
  (MzPeakFile_test required ordering the three new `FORMAT/*` includes
  alphabetically — `FileHandler.h`, `FileTypes.h`, `MzPeakFile.h` — to satisfy
  clang-format's SortIncludes.)
- **Pre-existing whole-file non-conformance (out of scope)**:
  `FileHandler.cpp`, `FileHandler_test.cpp`, `FileTypes.h`, `FileTypes.cpp`.
  These files were already non-conformant to `.clang-format` on the committed
  HEAD *before* this plan (FileHandler.cpp: 3057 diff lines on the original;
  FileTypes.h/.cpp: 375/491; FileHandler_test.cpp: 417). They use Allman braces
  + 4-space indentation, whereas the repo `.clang-format` wants 2-space namespace
  indentation, same-line braces, and single-line if-bodies. The pre-existing
  count did not increase from my edits (FileHandler.cpp: 3057 → 3037). My added
  lines follow each file's established hand-written local style and are
  consistent with their immediate neighbors. A wholesale reformat of these
  large legacy files is far out of scope for this plan and the plan explicitly
  says "prefer not to destabilize FileHandler_test", so it was not done.

## Commits
- OpenMS (`feature/mzpeak-file-handler`): **37c44c9** — "Wire .mzpeak into
  FileHandler (content detection + load dispatch)"
- openms-mzpeak: this SUMMARY (commit below).

## Acceptance
- `getTypeByFileName("x.mzpeak") == MZPEAK` ✓
- `getTypeByContent(small.mzpeak) == MZPEAK` ✓
- `FileHandler().loadExperiment(small.mzpeak, exp)` → 48 spectra ✓
- MzPeakFile_test + FileHandler_test + FileTypes_test all PASS ✓
- clang-format clean on all newly-authored/edited content; residual diffs are
  100% pre-existing legacy non-conformance (documented above).

## Self-Check: PASSED
