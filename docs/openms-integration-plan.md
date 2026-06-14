# RDR-19 — Wiring mzPeak into OpenMS data structures (research-backed plan)

Status: **plan only.** Researched against the local OpenMS source (`~/Claude/OpenMS`),
the mzPeak reader/writer, and the Rust reference; adversarially reviewed (codex —
corrections folded into §1/§4a/§5; vibe run failed to complete). The headline finding
flips RDR-19's feasibility: the dependency obstacle the earlier roadmap feared is
**already solved**.

**Key refinement (metadata template):** treat this as **two layers with two different
precedents** — a *data/container* layer (random-access Parquet+zip → copy
`SqMassFile`/`ParquetFile`/`ZipRandomAccessFile`, NOT XML) and a *metadata-mapping*
layer (PSI-MS CV-param → OpenMS objects → **mirror/reuse `MzMLHandler`**, see §4a).
mzPeak carries the *same* PSI-MS accessions mzML does, so the mzML reader/writer is the
right template for metadata tracking; the binary container is where it differs.

## 0. Feasibility re-assessment (what changed)

| Earlier assumption | Reality (verified) |
|---|---|
| "Needs a heavy Arrow/Parquet/zip dep added to OpenMS" | **Arrow + Parquet + libzip + Boost are ALREADY OpenMS deps.** `vcpkg.json` lists `"arrow"`; `cmake/package_general.cmake` wires `OPENMS_ARROW_TARGET`/`OPENMS_PARQUET_TARGET`/`OPENMS_ARROW_DATASET_TARGET`; `ZipArchiveFile`/`ZipRandomAccessFile`/`ZipIfstream` + `Libzip_test` exist; Boost is core. |
| "No precedent for columnar formats" | `FORMAT/ParquetFile.{h,cpp}` (shared Parquet+zip-bundle utility, **store-only compression like mzPeak**), `TransitionParquetFile`, `MSExperimentArrowExport.cpp` (already serializes `MSExperiment`→Arrow/Parquet), `ProteinIdentificationArrowIO`. |
| "blocked: libOpenMS not built" | True locally (only deps built), but that is a *build-the-tree* chore, not an architectural blocker. |
| New constraint surfaced | **OpenMS is C++20** (`cmake/*`: `cxx_std_20`); our reader is C++23. A real (small) port — see §6. |

Net: RDR-19 is **feasible and scoped**, not blocked. The work is a converter + a thin
handler + (first) closing a few reader metadata gaps.

## 1. Architecture decision — where the code lives

Three options, with the recommendation:

- **(A) Native OpenMS handler reusing OpenMS's Arrow/Parquet/zip** — write `MzPeakFile`
  in `src/openms/{include,source}/OpenMS/FORMAT/`, OpenMS style, reading the Parquet
  tables directly via the existing `ParquetFile` helpers + `ZipRandomAccessFile`.
  Pro: single coding style, no C++23 port, no vendored lib, uses infra that already
  ships. Con: re-implements mzPeak's spec logic (chunked/numpress/null-fill) that we
  already wrote & tested (~4.5k LOC).
- **(B) Vendor the mzpeak reader as a third-party subtree** + a thin `MzPeakFile`
  wrapper that converts to `MSExperiment`. Pro: reuse tested decoders. Con: C++23→C++20
  port of the whole lib; two styles in-tree; OpenMS would carry our internal API.
- **(C) Hybrid (RECOMMENDED).** Native OpenMS `MzPeakFile` handler in OpenMS style for
  the table reading + `MSExperiment` conversion, reusing the spec-conformance algorithms.
  **Honest cost (codex):** only `null_fill.cpp` and the numpress wrapper are *truly*
  self-contained pure-numeric units that lift trivially. The chunked/delta decoder is
  **not** a small standalone function today — it is `Encoding<T>::decode_chunked` (a
  template in `include/mzpeak/util/encoding.h`) coupled to `schema/array_index.h`,
  `psi/data_type.h`, and `parquet_types.h`. Reusing it means either (i) porting that
  whole `Encoding`/schema/type sub-stack (non-trivial C++23→C++20), or (ii)
  **re-expressing the chunk reconstruction directly against `arrow::ChunkedArray` inside
  the OpenMS handler** (basic/delta/numpress are individually simple — cumulative-sum
  seeded by `chunk_start`, null-fill via the ported `null_fill`, numpress via the ported
  wrapper — but the *plumbing* that reads `*_chunk_start/_end/_values`/`chunk_encoding`
  must be rewritten, not lifted). Plan for (ii): port `null_fill` + numpress as cores,
  rewrite the ~200-line chunk-assembly loop natively. Pro: clean OpenMS style + reuse the
  *hard, tested* numeric kernels; matches how OpenMS already does Parquet I/O. Con:
  the chunk-assembly logic is re-implemented (and must be re-tested vs the same fixtures).

The earlier reviewer worry ("adapter likely belongs in the OpenMS tree, not a dep on
mzpeak") is correct, and (C) realizes it without throwing away the spec work.

## 2. The handler class + registration (the OpenMS-facing surface)

Structure it like `MzMLFile` (a thin public class delegating to a `MzMLHandler`-style
worker that builds the `MSExperiment` + `ExperimentalSettings`), but with the *data layer*
borrowed from the binary/columnar handlers: read rows via `ParquetFile`/
`ZipRandomAccessFile` and random-access by row group like `SqMassFile`/`CachedMzML`,
instead of MzML's XML SAX stream. Public surface mirrors `ParquetFile`/`MzMLFile`
(verified in `FORMAT/ParquetFile.h`):

```
// src/openms/include/OpenMS/FORMAT/MzPeakFile.h
class OPENMS_DLLAPI MzPeakFile : public ProgressLogger   // (XMLFile base is XML-only; not used)
{
public:
  MzPeakFile();
  void load(const String& filename, MSExperiment& exp);          // whole-experiment
  void store(const String& filename, const MSExperiment& exp);   // writer
  PeakFileOptions& getOptions();                                 // RT/mz/ms-level filters, metadata-only
  const PeakFileOptions& getOptions() const;
  // optional streaming:
  void transform(const String& filename, Interfaces::IMSDataConsumer* consumer,
                 bool skip_full_count, bool skip_first_pass);
};
```
- Header: SPDX BSD-3 + `$Maintainer$`/`$Authors$` block (per `ParquetFile.h`), `OPENMS_DLLAPI`, `#include <OpenMS/config.h>`.
- **Registration** (3 edits): add `MZPEAK` to `FORMAT/FileTypes.h` `enum Type` + the name/extension tables in `FileTypes.cpp` (`.mzpeak`); add dispatch in `FileHandler::loadExperiment`/`storeExperiment`/`getType`. Add content detection (`.mzpeak` is a zip whose first member is `mzpeak_index.json`) to `getTypeByContent` if extension-only is insufficient.
- Tests live in `src/tests/class_tests/openms/source/MzPeakFile_test.cpp`, registered in `executables.cmake`; data under `src/tests/class_tests/openms/data/`. Build: add `MzPeakFile.cpp` to the FORMAT sources list (`src/openms/source/FORMAT/` is globbed/listed in the OpenMS lib CMake).

## 3. Streaming vs whole-experiment load

OpenMS supports both: whole-experiment `load(MSExperiment&)` and a streaming
`IMSDataConsumer` (`consumeSpectrum`/`consumeChromatogram`/`setExpectedSize`/
`setExperimentalSettings`, see `INTERFACES/IMSDataConsumer.h`, `DATAACCESS/MSDataWritingConsumer.h`).

Recommendation: implement **whole-experiment first** (mzPeak random-access by row group
is already efficient; our reader gives `Spectra`/`Chromatograms` enumerables). Add the
**streaming `transform`** second — it maps naturally: iterate `Index::spectra()` →
`consumer->consumeSpectrum(convert(s))`. The detail-level (`DetailLevel::MetadataOnly`)
maps to `PeakFileOptions::setMetadataOnly`/`setFillData(false)`.

## 4. The data-model mapping (mzPeak → MSExperiment)

Verified against OpenMS headers. RT is **seconds**, intensity **float**, m/z **double**,
peaks **must be sorted by m/z** by the converter (`MSSpectrum::sortByPosition()` then
`MSExperiment::updateRanges()` — not auto). Core per-spectrum mapping:

| mzPeak (reader) | OpenMS setter | notes |
|---|---|---|
| `Spectrum::mz()/intensity()` | `MSSpectrum::push_back(Peak1D{mz,int})` then `sortByPosition()` | sort + `updateRanges()` mandatory |
| `SpectrumMetadata::retention_time` | `MSSpectrum::setRT` | seconds |
| `::ms_level` | `setMSLevel(UInt)` | |
| `::id` | `SpectrumSettings::setNativeID` | |
| `::polarity` | `InstrumentSettings::setPolarity(IonSource::Polarity)` | +1/−1 → POSITIVE/NEGATIVE |
| `::representation` (MS:1000128/127) | `SpectrumSettings::setType(PROFILE/CENTROID)` | |
| `::base_peak_*` / `::total_ion_current` | recomputed (`getBasePeak()`/`calculateTIC()`) or `setMetaValue` | OpenMS has no field |
| auxiliary arrays (wavelength, etc.) | `MSSpectrum::getFloatDataArrays()` (named + CV-annotated) | the side-channel target |
| ion mobility (per-spectrum / per-peak) | `setDriftTime`+`setDriftTimeUnit` / a FloatDataArray child of `MS:1002893` + `setIMFormat` | two representations, pick per layout |

Run-level (`Index::metadata()` `RunMetadata` → `MSExperiment` IS-A `ExperimentalSettings`):
`run`→`setDateTime`/ids; `sample_list`→`setSample`; `file_description.source_files`→
`setSourceFiles` (+ `SourceFile` is a `CVTermList`); `instrument_configuration_list`→
`setInstrument(Instrument)` (+ components→IonSource/MassAnalyzer/IonDetector); `software_list`
→ `Instrument::setSoftware` / per-spectrum `DataProcessing::setSoftware` (no run-level home — see §5);
`data_processing_method_list`→ per-spectrum `DataProcessing` (fan out via shared_ptr).

Chromatograms → `MSChromatogram` (time/intensity → `ChromatogramPeak`, `setChromatogramType`).
Wavelength spectra → **no native analog** (see §5); convert to a named EMR/absorption
construct by convention.

## 4a. Metadata mapping — mirror the mzML CV dispatch (the template to copy)

This is the crux, and why **`MzMLFile`/`MzMLHandler` is the template, not `SqMassFile`**.
mzML and mzPeak both carry **the same PSI-MS CV accessions**; OpenMS has already encoded
the complete accession→OpenMS-object knowledge in `MzMLHandler::handleCVParam_`
(`src/openms/source/FORMAT/HANDLERS/MzMLHandler.cpp:1415`): a **~332-accession dispatch
keyed by parent context** — `run` / `spectrum` / `scan` / `scanWindow` / `selectedIon` /
`precursor` / `activation` / `binaryDataArray` / `instrumentConfiguration` /
`referenceableParamGroup`. e.g. `MS:1000511`→`setMSLevel`, `MS:1000127/1000128`→
`setType(CENTROID/PROFILE)`, `MS:1000285`→TIC, `MS:1000504/505`→base peak,
`MS:1000129/1000130`→polarity, `MS:1000827`/selected-ion→precursor m/z. Unrecognized
params fall to `handleUserParam_`→`MetaInfoInterface::setMetaValue` (the standard OpenMS
"keep it as a userParam" escape hatch). **Do not re-derive a parallel mapping table** —
it would drift out of sync with mzML.

The bridge: mzPeak's per-spectrum / run-level metadata is **flat `CvParam` tuples**
(`{accession, name, value, unit}` — already modelled by our reader's `RunMetadata` and,
once RDR-10b lands, per-spectrum `parameters`). That is precisely the
`(context, accession, value, unit)` shape `handleCVParam_` consumes (minus the XML).
Two reuse strategies:

- **(M1, best long-term) Refactor `handleCVParam_` into a format-agnostic CV applicator.**
  Extract the dispatch body into a helper `applyCVParam(context, accession, value, unit,
  <target OpenMS object>)` that both `MzMLHandler` and the new `MzPeakHandler` call.
  One source of truth → mzML and mzPeak map *identically*; fixes the §5 lossiness
  concern the OpenMS-canonical way. Cost: touches core mzML code → needs OpenMS-maintainer
  buy-in and careful regression-testing of mzML; bigger PR.
- **(M2, v1 PR) Mirror the subset.** `MzPeakFile` replicates the accession→setter mapping
  for the accessions mzPeak actually emits (a fraction of 332 — ms_level, representation,
  polarity, base-peak/TIC, precursor/isolation/activation, instrument/software/source-file
  terms). Self-contained, reviewable, no mzML risk; accept some duplication. **Recommended
  to ship first**, with M1 proposed to OpenMS as the follow-up consolidation.

Run-level: build `ExperimentalSettings` exactly as `MzMLHandler` does for the `<run>` /
`<fileDescription>` / `<instrumentConfigurationList>` / `<softwareList>` /
`<dataProcessingList>` / `<sampleList>` sections — same target setters listed in §4, fed
from `RunMetadata` blocks instead of XML. **Writer (store) symmetry:** mirror
`MzMLHandler`'s `writeUserParam_`/`writeCV_` direction — walk the `MSExperiment`'s typed
fields + meta values back into mzPeak `CvParam`s (our `RunMetadata::to_json()` already
exists for the run level; WRT-2). Reusing the mzML write path (or its CV-emitting helpers)
keeps read/write metadata symmetric, which is the whole point of choosing this template.

## 5. Impedance mismatches (route these explicitly — the lossiness risks)

1. **Flat CvParam vs OpenMS typed metadata + two CV stores.** OpenMS has typed fields,
   `CVTermList` (on `SourceFile`/`Software`/`Precursor`/`Product`), and `MetaInfoInterface`
   (on nearly everything — `SpectrumSettings`, `Instrument`, `Sample`, … — but it stores
   only name→value, no accession/unit). **This is NOT a hard lossiness wall** (codex
   correction): the mzML handler resolves it canonically and we copy that — recognized
   accession→typed setter (the §4a dispatch); CV-capable host (`SourceFile`/`Software`/
   `Precursor`/`Product`)→`addCVTerm` (accession+unit preserved); everything else→
   `setMetaValue` as a userParam. Only the *last bucket* loses the bare accession (encode
   it into the key, e.g. `"MS:1000XYZ"`, to keep it). So fidelity equals mzML's — which is
   the bar. Lossiness is bounded to genuinely-unmodelled params, same as mzML.
2. **Wavelength/EMR spectra have no native `MSSpectrum` analog** (x-axis is m/z). Round-trip
   only by convention (named FloatDataArray for wavelength + EMR scan mode), or via the
   chromatogram EMR/absorption types if time-axis.
3. **Ion mobility: two representations** (per-peak `MS:1002893` array + `IM_PEAK` vs
   per-spectrum `setDriftTime` + `IM_SPECTRUM`); precursor has its own drift time; units
   ms/1/K0/CCS/FAIMS-CV → `DriftTimeUnit`. Set `IMFormat` consistently.
4. **Software/DataProcessing are not run-level in OpenMS** — fan run-level lists onto each
   spectrum's `DataProcessing` or stash as experiment meta values.
5. **Isolation windows are offsets from target m/z**, not absolute bounds
   (`lowerOffset = target − lowEdge`). Sign errors silently corrupt widths. Also:
   `MSSpectrum` holds a **vector of `Precursor`s** and each precursor can carry multiple
   **selected ions** — map every selected ion, not just the first, or MSn/DIA precursor
   info is silently dropped (codex). SRM lives on the chromatogram side (`Precursor`
   *and* `Product` on `ChromatogramSettings`) — don't lose the product m/z.
6. **Base peak / TIC are computed, not stored** (`getBasePeak()`/`calculateTIC()`).
7. **`updateRanges()` is mandatory; sorting is conditional** (codex precision).
   `updateRanges()` must be called (per spectrum/chromatogram + experiment) or the range
   caches are wrong. Sorting by m/z is **not** required to *construct* a spectrum, but it
   is a precondition for every downstream binary-search/range API (`MZBegin`,
   `findNearest`, area iterators) — so the converter should still `sortByPosition()` +
   `MSExperiment::sortSpectra()` unless it can prove the source is already ascending.

## 6. C++23 → C++20 port (constraint, not blocker)

OpenMS targets `cxx_std_20`. Our reader uses mostly C++20 features (ranges, `using enum`,
`std::bit_cast`, concepts) under a `cpp_std=c++23` meson flag. Under option (C) only the
**ported cores** must be C++20 — audit them for any C++23-only usage (none obvious;
`std::bit_cast` and ranges are C++20). The full lib does NOT need porting under (C).

## 7. Reader prerequisites (close these FIRST — they bound a lossless map)

Our `SpectrumMetadata` exposes a subset; `spectra_metadata.parquet` carries MORE that the
converter needs (verified on small.mzpeak): `MS_1000559_spectrum_type`, lowest/highest
observed m/z, `data_processing_ref`, per-spectrum **`parameters`** (flat CV params),
**`auxiliary_arrays`** + count, and — in MSn/IM files — precursor/isolation/activation/IM
columns (the Rust reference surfaces these via mzdata `SpectrumSource`). Prereqs:
- **RDR-10b**: extend `read_spectra_metadata` + `SpectrumMetadata` to expose per-spectrum
  `parameters` (CvParam list), `spectrum_type`, observed-mz range, `data_processing_ref`.
- **RDR-10c**: precursor / selected-ion / isolation-window / activation / ion-mobility
  (needs an MSn+IM fixture; mirror the Rust reader's fields).
- **RDR-9b**: a typed accessor for `auxiliary_arrays` (→ OpenMS FloatDataArrays).
Without these, a v1 converter maps peaks + core scalars + run metadata (already useful for
many tools) but drops precursor/IM/aux/CV-params — acceptable for a first PR if documented.

## 8. Phasing

- **P0** Build OpenMS locally (chore) so we can compile/test against `libOpenMS`.
- **P1** Reader prereqs RDR-10b/10c/9b (in the mzpeak lib, our existing workflow).
- **P2** `MzPeakFile::load` (whole-experiment) + converter + FileTypes/FileHandler
  registration + `MzPeakFile_test` round-tripping a bundled fixture into `MSExperiment`
  (assert peaks, RT, ms_level, polarity, type, run metadata). Metadata via **§4a M2**
  (mirror the `handleCVParam_` subset). Hybrid option (C): port `null_fill`+numpress cores,
  re-implement the chunk-assembly loop natively (§1).
- **P3** `MzPeakFile::store` (MSExperiment→mzPeak) reusing our writer + WRT-2 metadata,
  emitting CV params à la `MzMLHandler::writeCV_`/`writeUserParam_` (§4a, write symmetry).
- **P3b** (optional, propose to OpenMS) **§4a M1**: refactor `handleCVParam_` into a shared
  `applyCVParam` helper so mzML and mzPeak map identically — consolidation PR after v1.
- **P4** Streaming `transform` consumer; `PeakFileOptions` (RT/mz/ms-level/metadata-only).
- **P5** Cross-validate: OpenMS reads C++/Rust-written mzPeak == the same `MSExperiment`
  as reading the equivalent mzML; round-trip mzML→mzPeak→mzML through OpenMS.

## 9. Risks
- **Lossy metadata** (§5) — set expectations; lead with peaks+core scalars+run metadata.
- **Reader parity debt** (§7) — precursor/IM not yet surfaced; sequence RDR-10c before
  claiming MSn fidelity.
- **C++20 port** of cores (§6) — small, audit for C++23-only.
- **Where it lands**: this is a PR to **OpenMS/OpenMS**, not OpenMS/mzpeak — different repo,
  conventions (CamelCase, OPENMS_DLLAPI, $Maintainer$ headers, Doxygen, the test harness).
- **Vendoring vs find_package** for any ported cores: prefer self-contained `.cpp` in the
  FORMAT dir (OpenMS already vendors ms-numpress-style code) over a new external dep.
