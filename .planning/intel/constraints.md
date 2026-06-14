# Constraints (synthesized from ingest)

Technical constraints, contracts, and invariants extracted from the SPEC-class
docs. These bound how the remaining work (esp. RDR-19) must be implemented.

---

## CON-openms-cpp20 — OpenMS targets C++20; mzPeak reader is C++23
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§0, §6)
- type: nfr
- content: OpenMS CMake sets `cxx_std_20`; the mzPeak reader builds under a meson
  `cpp_std=c++23` flag but uses mostly C++20 features (ranges, `using enum`,
  `std::bit_cast`, concepts). Any code reused inside OpenMS (the ported cores)
  must compile as C++20. The full lib does not need porting under option (C).

## CON-openms-deps-present — Arrow/Parquet/libzip/Boost are already OpenMS deps
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§0)
- type: nfr
- content: `vcpkg.json` lists `"arrow"`; `cmake/package_general.cmake` wires
  `OPENMS_ARROW_TARGET`/`OPENMS_PARQUET_TARGET`/`OPENMS_ARROW_DATASET_TARGET`;
  `ZipArchiveFile`/`ZipRandomAccessFile`/`ZipIfstream` + `Libzip_test` exist;
  Boost is core. No new heavy dependency may be added — reuse these. Existing
  columnar precedent: `FORMAT/ParquetFile.{h,cpp}` (store-only compression, like
  mzPeak), `TransitionParquetFile`, `MSExperimentArrowExport.cpp`,
  `ProteinIdentificationArrowIO`.

## CON-mzpeakfile-api — MzPeakFile public surface contract
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§2)
- type: api-contract
- content:
  ```
  // src/openms/include/OpenMS/FORMAT/MzPeakFile.h
  class OPENMS_DLLAPI MzPeakFile : public ProgressLogger {
  public:
    MzPeakFile();
    void load(const String& filename, MSExperiment& exp);
    void store(const String& filename, const MSExperiment& exp);
    PeakFileOptions& getOptions();
    const PeakFileOptions& getOptions() const;
    void transform(const String& filename, Interfaces::IMSDataConsumer* consumer,
                   bool skip_full_count, bool skip_first_pass);
  };
  ```
  Header: SPDX BSD-3 + `$Maintainer$`/`$Authors$` block, `OPENMS_DLLAPI`,
  `#include <OpenMS/config.h>`. Registration = 3 edits: `MZPEAK` in
  `FileTypes.h enum Type` + name/extension tables in `FileTypes.cpp` (`.mzpeak`);
  dispatch in `FileHandler::loadExperiment`/`storeExperiment`/`getType`;
  `.mzpeak` content detection (zip whose first member is `mzpeak_index.json`).
  Tests: `src/tests/class_tests/openms/source/MzPeakFile_test.cpp` (registered in
  `executables.cmake`); data under `src/tests/class_tests/openms/data/`.

## CON-data-model-mapping — mzPeak → MSExperiment per-spectrum mapping
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§4)
- type: schema
- content: RT is **seconds**, intensity **float**, m/z **double**. Mapping:
  - `Spectrum::mz()/intensity()` → `MSSpectrum::push_back(Peak1D{mz,int})`
  - `SpectrumMetadata::retention_time` → `setRT` (seconds)
  - `::ms_level` → `setMSLevel(UInt)`
  - `::id` → `SpectrumSettings::setNativeID`
  - `::polarity` → `InstrumentSettings::setPolarity` (+1/−1 → POSITIVE/NEGATIVE)
  - `::representation` (MS:1000128/127) → `setType(PROFILE/CENTROID)`
  - base_peak/TIC → recomputed (`getBasePeak()`/`calculateTIC()`)
  - auxiliary arrays → `MSSpectrum::getFloatDataArrays()` (named + CV-annotated)
  - ion mobility → `setDriftTime`+`setDriftTimeUnit` OR FloatDataArray child of
    `MS:1002893` + `setIMFormat` (two representations, pick per layout)
  Run-level (`RunMetadata` → `ExperimentalSettings`): run→setDateTime/ids;
  sample_list→setSample; source_files→setSourceFiles; instrument_configuration_list
  →setInstrument; software_list→Instrument::setSoftware / per-spectrum
  DataProcessing; data_processing_method_list→per-spectrum DataProcessing.
  Chromatograms → `MSChromatogram`; wavelength → no native analog (named
  FloatDataArray/EMR by convention).

## CON-mzml-cv-dispatch — MzMLHandler::handleCVParam_ is the metadata template
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§4a)
- type: api-contract
- content: `MzMLHandler::handleCVParam_` (MzMLHandler.cpp:1415) is a ~332-accession
  dispatch keyed by parent context (run/spectrum/scan/scanWindow/selectedIon/
  precursor/activation/binaryDataArray/instrumentConfiguration/
  referenceableParamGroup). e.g. MS:1000511→setMSLevel, MS:1000127/1000128→
  setType, MS:1000285→TIC, MS:1000504/505→base peak, MS:1000129/1000130→polarity,
  MS:1000827→precursor m/z. Unrecognized → `handleUserParam_`→`setMetaValue`.
  mzPeak per-spectrum/run metadata is flat `CvParam` tuples
  `{accession,name,value,unit}` — exactly the `(context,accession,value,unit)`
  shape the dispatch consumes (minus XML). Do NOT re-derive a parallel table; v1
  mirrors the emitted subset (M2), M1 consolidation proposed later.

## CON-impedance-mismatches — Lossiness risks to route explicitly
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/openms-integration-plan.md (§5)
- type: schema
- content:
  1. Flat CvParam vs OpenMS typed metadata + two CV stores (CVTermList vs
     MetaInfoInterface): recognized accession→typed setter; CV-capable host
     (SourceFile/Software/Precursor/Product)→addCVTerm (accession+unit preserved);
     else→setMetaValue (encode accession into the key to keep it). Fidelity equals
     mzML's — not a hard wall.
  2. Wavelength/EMR spectra have no native MSSpectrum analog (x-axis is m/z) —
     round-trip only by convention.
  3. Ion mobility has two representations (per-peak MS:1002893 + IM_PEAK vs
     per-spectrum setDriftTime + IM_SPECTRUM); precursor has its own drift time;
     set IMFormat consistently.
  4. Software/DataProcessing are not run-level in OpenMS — fan onto each
     spectrum's DataProcessing or stash as experiment meta values.
  5. Isolation windows are **offsets** from target m/z (lowerOffset = target −
     lowEdge), not absolute bounds — sign errors silently corrupt widths. Map
     **every** selected ion (MSSpectrum holds a vector of Precursors, each with
     multiple selected ions), not just the first. SRM: Precursor AND Product on
     ChromatogramSettings — don't lose product m/z.
  6. Base peak / TIC are computed, not stored.
  7. `updateRanges()` is **mandatory** (per spectrum/chromatogram + experiment);
     sorting by m/z is conditional but required before any binary-search/range API
     (MZBegin, findNearest, area iterators) — `sortByPosition()` +
     `MSExperiment::sortSpectra()` unless source is provably ascending.

## CON-rdr4-blast-radius — Signed→unsigned index touches Query/stats/casts
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/roadmap-remaining.md (Risks, Phase 1)
- type: nfr
- content: RDR-4 routes the entity index through `UInt64` across `DataType` +
  `data_type_traits` + parquet type-tag maps; `array_index.cpp` index data_type;
  the `Query::Predicate<…>` in spectra/chromatograms/wavelength_spectra; the Query
  value variant + range/stats eval (`query.h`); `record_count()`'s stats cast
  (`data_arrays.cpp:226`). Keep the signed path compiling until the unsigned path
  is proven on every fixture. The >INT64_MAX **predicate/stats** test (not only
  decode) is the easy-to-miss part.

## CON-semantic-compare — Validate semantically, never byte-diff Parquet
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/e2e-testing.md (intro, design notes)
- type: protocol
- content: parquet-cpp and parquet-rs differ in `created_by`, ZSTD streams, and
  page layout, so byte-identity is neither achievable nor the goal. Compare
  decoded values within float tolerance: **1e-9 m/z, 1e-6 intensity**. Per-spectrum
  m/z sorting: the writer sorts each spectrum by m/z; reverse tests sort the
  reference the same way before comparing. One byte-exact carve-out: Numpress
  buffers vs numpress-rs (once chunked encoding exists, writer P3 / reader RDR-7).

## CON-structural-conformance — Structural invariants beyond values
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/e2e-testing.md (design notes)
- type: protocol
- content: Beyond value compares, assert structural conformance to catch shared
  writer/reader bugs that round-trip clean: ZIP members STORED, `store_schema`
  present, page index + statistics written, `spectrum_array_index` +
  `spectrum_count` file-KV present, canonical column paths. A conformant file MUST
  carry `spectra_metadata.parquet` even though the C++ reader reads spectra
  straight from the data table (the Rust reader's `get_spectrum` resolves a
  SpectrumDescription from it).

## CON-e2e-matrix — T1–T5 forward/reverse validation matrix
- source: /Users/kohlbach/Claude/mzPeak OpenMS/openms-mzpeak/docs/e2e-testing.md (matrix, coverage snapshot)
- type: protocol
- content:
  - T1 forward intra (C++ write → C++ read): PASS — `writer_test`, `archive_writer_test`.
  - T2 forward cross (C++ write → Rust read): PASS — Rust MzPeakReader reads C++
    output, values match (Phase 1b added the metadata table); `scripts/e2e_cross_impl.sh`.
  - T3 reverse intra (C++ read→write→read): PASS — `roundtrip_test`.
  - T4 reverse cross (Rust write → C++ read vs pyarrow): PASS — `e2e` suite drives
    all layouts; per-decoder unit tests pin values vs pyarrow.
  - T5 full pipeline (mzML→Rust convert→C++ read→write→Rust read→mzML): PASS —
    small.mzML → 48 spectra, 13589 points in spectrum 0; wired into `scripts/e2e.sh`.
  Drivers: `scripts/e2e.sh` (whole matrix, FAST=1 skips the slow e2e suite),
  `scripts/e2e_cross_impl.sh` (T2). `test/e2e_test.cpp` (300s timeout) drives the
  entire reader API over every bundled fixture; it surfaced RDR-28/RDR-29.
