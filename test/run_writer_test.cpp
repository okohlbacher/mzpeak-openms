/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE RunWriter
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <parquet/api/reader.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include "mzpeak/chromatograms.h"
#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/util/json_writer.h"
#include "mzpeak/wavelength_spectra.h"
#include "mzpeak/writer.h"

namespace {

namespace fs = std::filesystem;

/// A scratch path removed on scope exit, so a failing assertion cannot leave
/// an artifact that makes the next run pass for the wrong reason.
struct Scratch {
  fs::path path;
  explicit Scratch(const char* name)
      : path(fs::temp_directory_path() / name)
  {
    fs::remove_all(path);
  }
  ~Scratch() { fs::remove_all(path); }
};

/// A chromatogram with a linear ramp: times 0, 30, 60, 90, 120 SECONDS.
MzPeak::ChromatogramData make_tic()
{
  MzPeak::ChromatogramData c;
  c.id = "TIC";
  c.chromatogram_type = "MS:1000235"; // total ion current chromatogram
  c.polarity = 1;
  for (int i = 0; i < 5; ++i) {
    c.time.push_back(i * 30.0);
    c.intensity.push_back(1000.0f * static_cast<float>(i + 1));
  }
  return c;
}

/// A UV spectrum whose intensities are ALL NEGATIVE, as a baseline-subtracted
/// absorbance trace is.
MzPeak::WavelengthSpectrumData make_uv()
{
  MzPeak::WavelengthSpectrumData w;
  w.id = "uv=0";
  w.time = 12.0; // seconds
  w.intensity_unit = "UO:0000269";
  for (int i = 0; i < 4; ++i) {
    w.wavelength.push_back(210.0f + static_cast<float>(i) * 10.0f);
    w.intensity.push_back(-1.0f - static_cast<float>(i));
  }
  return w;
}

} // namespace

/******************************************************************************/
// A chromatogram survives a write/read cycle: values, metadata and units.
BOOST_AUTO_TEST_CASE(chromatogram_round_trip)
{
  Scratch scratch("mzp-test-chrom-rt");

  MzPeak::RunContents run;
  run.chromatograms.push_back(make_tic());
  MzPeak::write_run_directory(scratch.path, run);

  auto chromatograms = MzPeak::open(scratch.path).chromatograms();
  BOOST_TEST_REQUIRE(chromatograms.size() == 1u);
  auto chromatogram = chromatograms[0];

  // Time goes out in seconds, is stored in minutes, and comes back in seconds.
  // A missing conversion on either side shows up as a factor of 60.
  const auto& time = chromatogram.time();
  BOOST_TEST_REQUIRE(time.size() == 5u);
  BOOST_TEST(time.front() == 0.0, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(time.back() == 120.0, boost::test_tools::tolerance(1e-9));

  const auto& intensity = chromatogram.intensity();
  BOOST_TEST_REQUIRE(intensity.size() == 5u);
  BOOST_TEST(intensity.front() == 1000.0f);
  BOOST_TEST(intensity.back() == 5000.0f);

  BOOST_TEST(chromatogram.intensity_unit() == "MS:1000131");

  const MzPeak::ChromatogramMetadata& md = chromatogram.metadata();
  BOOST_TEST(md.id == "TIC");
  BOOST_TEST(md.chromatogram_type == "MS:1000235");
  BOOST_TEST_REQUIRE(md.polarity.has_value());
  BOOST_TEST(*md.polarity == 1);
  BOOST_TEST_REQUIRE(md.number_of_data_points.has_value());
  BOOST_TEST(*md.number_of_data_points == 5u);
  BOOST_TEST(!md.has_unreadable_product);

  BOOST_TEST(chromatograms.by_id("TIC").metadata().id == "TIC");
}

/******************************************************************************/
// A UV spectrum survives the same cycle, and its summary fields are computed
// from the data rather than seeded at zero.  The reference writer records
// lambda max 0 and base peak 0 for an all-negative spectrum; ours records the
// real maximum, which here is the LEAST negative value.
BOOST_AUTO_TEST_CASE(wavelength_round_trip_summarises_negative_spectra_honestly)
{
  Scratch scratch("mzp-test-uv-rt");

  MzPeak::RunContents run;
  run.wavelength_spectra.push_back(make_uv());
  MzPeak::write_run_directory(scratch.path, run);

  auto spectra = MzPeak::open(scratch.path).wavelength_spectra();
  BOOST_TEST_REQUIRE(spectra.size() == 1u);
  auto spectrum = spectra[0];

  BOOST_TEST_REQUIRE(spectrum.wavelength().size() == 4u);
  BOOST_TEST(spectrum.wavelength().front() == 210.0);
  BOOST_TEST(spectrum.wavelength().back() == 240.0);
  BOOST_TEST(spectrum.wavelength_unit() == "UO:0000018"); // nanometre
  BOOST_TEST(spectrum.intensity_unit() == "UO:0000269");  // absorbance unit

  const MzPeak::WavelengthSpectrumMetadata& md = spectrum.metadata();
  BOOST_TEST(md.id == "uv=0");
  BOOST_TEST_REQUIRE(md.time.has_value());
  BOOST_TEST(*md.time == 12.0, boost::test_tools::tolerance(1e-6));
  BOOST_TEST_REQUIRE(md.number_of_data_points.has_value());
  BOOST_TEST(*md.number_of_data_points == 4u);

  BOOST_TEST_REQUIRE(md.lowest_observed_wavelength.has_value());
  BOOST_TEST(*md.lowest_observed_wavelength == 210.0);
  BOOST_TEST_REQUIRE(md.highest_observed_wavelength.has_value());
  BOOST_TEST(*md.highest_observed_wavelength == 240.0);

  // Intensities are -1, -2, -3, -4 at 210, 220, 230, 240 nm.  The maximum is
  // -1 at 210 nm; a zero-seeded maximum would report 0 at nothing.
  BOOST_TEST_REQUIRE(md.base_peak_intensity.has_value());
  BOOST_TEST(*md.base_peak_intensity == -1.0f);
  BOOST_TEST_REQUIRE(md.lambda_max.has_value());
  BOOST_TEST(*md.lambda_max == 210.0);
  BOOST_TEST_REQUIRE(md.total_ion_current.has_value());
  BOOST_TEST(*md.total_ion_current == -10.0f);
}

/******************************************************************************/
// Spectra, chromatograms and UV spectra in ONE archive, which is what a real
// LC-MS run with a diode-array detector produces.
BOOST_AUTO_TEST_CASE(mixed_run_archive_round_trip)
{
  Scratch scratch("mzp-test-mixed.mzpeak");

  MzPeak::RunContents run;

  MzPeak::SpectrumData s;
  s.mz = {100.0, 200.0, 300.0};
  s.intensity = {10.0f, 20.0f, 30.0f};
  s.ms_level = 1;
  s.retention_time = 5.0;
  s.id = "scan=1";
  run.spectra.push_back(s);

  run.chromatograms.push_back(make_tic());
  run.wavelength_spectra.push_back(make_uv());

  MzPeak::write_run_archive(scratch.path, run);
  BOOST_TEST_REQUIRE(fs::exists(scratch.path));

  auto index = MzPeak::open(scratch.path);
  BOOST_TEST(index.spectra().size() == 1u);
  BOOST_TEST(index.chromatograms().size() == 1u);
  BOOST_TEST(index.wavelength_spectra().size() == 1u);

  // Each entity type kept its own values; the three tables did not bleed.
  BOOST_TEST(index.spectra()[0].mz().size() == 3u);
  BOOST_TEST(index.chromatograms()[0].time().size() == 5u);
  BOOST_TEST(index.wavelength_spectra()[0].wavelength().size() == 4u);
}

/******************************************************************************/
// An SRM/MRM chromatogram is REFUSED rather than written without its Q3
// selection.  The product facet has no writer and an unimplemented reader path,
// so the file would look valid and silently lack the transition.
BOOST_AUTO_TEST_CASE(srm_chromatogram_is_refused)
{
  Scratch scratch("mzp-test-srm");

  MzPeak::RunContents run;
  MzPeak::ChromatogramData srm;
  srm.chromatogram_type = "MS:1001473"; // selected reaction monitoring
  srm.time = {1.0, 2.0};
  srm.intensity = {1.0f, 2.0f};
  run.chromatograms.push_back(srm);

  BOOST_CHECK_THROW(MzPeak::write_run_directory(scratch.path, run),
                    MzPeak::ParquetError);

  // Both spellings are refused: mzdata emits MS:1000473 for SRM while its own
  // reader expects MS:1001473, so a caller may legitimately hold either.
  run.chromatograms[0].chromatogram_type = "MS:1000473";
  BOOST_CHECK_THROW(MzPeak::write_run_directory(scratch.path, run),
                    MzPeak::ParquetError);

  // Nothing was left behind by the refusal.
  BOOST_TEST(!fs::exists(scratch.path));
}

/******************************************************************************/
// Two chromatograms in different intensity units cannot share one intensity
// column.  Writing them anyway would label absorbance as detector counts.
BOOST_AUTO_TEST_CASE(mixed_intensity_units_are_refused)
{
  Scratch scratch("mzp-test-units");

  MzPeak::RunContents run;
  run.chromatograms.push_back(make_tic()); // MS:1000131
  MzPeak::ChromatogramData dad(make_tic());
  dad.id = "DAD";
  dad.chromatogram_type = "MS:1000812";
  dad.intensity_unit = "UO:0000269"; // absorbance
  run.chromatograms.push_back(dad);

  BOOST_CHECK_THROW(MzPeak::write_run_directory(scratch.path, run),
                    MzPeak::ParquetError);
}

/******************************************************************************/
// Parallel arrays of different lengths are caught before anything is written.
BOOST_AUTO_TEST_CASE(mismatched_lengths_are_refused)
{
  Scratch scratch("mzp-test-lengths");

  MzPeak::RunContents run;
  MzPeak::ChromatogramData c;
  c.time = {1.0, 2.0};
  c.intensity = {1.0f};
  run.chromatograms.push_back(c);
  BOOST_CHECK_THROW(MzPeak::write_run_directory(scratch.path, run),
                    MzPeak::ParquetError);

  MzPeak::RunContents uv_run;
  MzPeak::WavelengthSpectrumData w;
  w.wavelength = {210.0f};
  w.intensity = {1.0f, 2.0f};
  uv_run.wavelength_spectra.push_back(w);
  BOOST_CHECK_THROW(MzPeak::write_run_directory(scratch.path, uv_run),
                    MzPeak::ParquetError);

  BOOST_TEST(!fs::exists(scratch.path));
}

/******************************************************************************/
// Unsorted input is stored ascending, and the observed range describes what was
// STORED.  The reference writer sorts a copy for output but summarises the
// original order, so it can report a low above its high.
BOOST_AUTO_TEST_CASE(unsorted_input_is_sorted_and_summarised_consistently)
{
  Scratch scratch("mzp-test-unsorted");

  MzPeak::RunContents run;
  MzPeak::WavelengthSpectrumData w;
  w.id = "uv=0";
  w.wavelength = {400.0f, 210.0f, 300.0f};
  w.intensity = {3.0f, 1.0f, 2.0f};
  run.wavelength_spectra.push_back(w);
  MzPeak::write_run_directory(scratch.path, run);

  auto spectra = MzPeak::open(scratch.path).wavelength_spectra();
  auto spectrum = spectra[0];

  const auto& wavelength = spectrum.wavelength();
  BOOST_TEST(std::is_sorted(wavelength.begin(), wavelength.end()));
  BOOST_TEST(wavelength.front() == 210.0);
  BOOST_TEST(wavelength.back() == 400.0);

  const MzPeak::WavelengthSpectrumMetadata& md = spectrum.metadata();
  BOOST_TEST_REQUIRE(md.lowest_observed_wavelength.has_value());
  BOOST_TEST_REQUIRE(md.highest_observed_wavelength.has_value());
  BOOST_TEST(*md.lowest_observed_wavelength == 210.0);
  BOOST_TEST(*md.highest_observed_wavelength == 400.0);
  BOOST_TEST(*md.lowest_observed_wavelength < *md.highest_observed_wavelength);

  // Intensity travelled with its own wavelength, not with its input position.
  BOOST_TEST(spectrum.intensity().front() == 1.0f); // the 210 nm sample
  BOOST_TEST(spectrum.intensity().back() == 3.0f);  // the 400 nm sample
}

/******************************************************************************/
// The stored time unit is pinned independently of the round trip.
//
// Round-tripping through our own reader cannot catch a coordinated break: if
// the writer stopped dividing by 60 and the reader stopped multiplying, every
// test above would still pass while the files claimed minutes and held
// seconds.  The array index is what a foreign reader trusts, so assert on it.
BOOST_AUTO_TEST_CASE(array_index_declares_the_units_the_data_is_stored_in)
{
  const std::string chromatograms(
      MzPeak::Util::point_chromatograms_array_index_json("MS:1000131"));
  // Time is stored in MINUTES; a reader converts using this declaration.
  BOOST_TEST(chromatograms.find("\"unit\":\"UO:0000031\"") != std::string::npos);
  BOOST_TEST(chromatograms.find("\"path\":\"point.time\"") != std::string::npos);
  BOOST_TEST(chromatograms.find("\"unit\":\"MS:1000131\"") != std::string::npos);

  // The intensity unit is whatever the caller declared, not a constant.
  const std::string absorbance(
      MzPeak::Util::point_chromatograms_array_index_json("UO:0000269"));
  BOOST_TEST(absorbance.find("\"unit\":\"UO:0000269\"") != std::string::npos);

  const std::string wavelength(
      MzPeak::Util::point_wavelength_array_index_json("MS:1000131"));
  // Nanometres, and the axis is declared sorted so a reader may binary-search.
  BOOST_TEST(wavelength.find("\"unit\":\"UO:0000018\"") != std::string::npos);
  BOOST_TEST(wavelength.find("\"sorting_rank\":0") != std::string::npos);
}

/******************************************************************************/
// The entity count is derived from the signal table when the metadata file
// declares none.
//
// This is the branch that used to dereference an empty std::optional: the
// count KV is not required, and `Index` leaves it unset whenever the metadata
// member is absent.  Removing the metadata member from a written run is the
// only way to reach it from the public API -- no bundled fixture omits the
// count -- so the run is rewritten here without it.
BOOST_AUTO_TEST_CASE(entity_count_is_derived_when_the_file_declares_none)
{
  Scratch scratch("mzp-test-no-count");

  MzPeak::RunContents run;
  run.chromatograms.push_back(make_tic());
  MzPeak::write_run_directory(scratch.path, run);

  // Drop the metadata member and every index entry that referred to it, so the
  // reader sees chromatogram signal data with no declared count at all.
  const fs::path index_path = scratch.path / "mzpeak_index.json";
  std::string json;
  {
    std::ifstream in(index_path, std::ios::binary);
    json.assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
  }
  auto root = boost::json::parse(json).as_object();
  boost::json::array kept;
  for (const auto& entry : root["files"].as_array()) {
    const std::string name(entry.as_object().at("name").as_string());
    if (name.find("chromatograms_metadata") != std::string::npos) {
      fs::remove(scratch.path / name);
      continue;
    }
    kept.push_back(entry);
  }
  root["files"] = std::move(kept);
  {
    std::ofstream out(index_path, std::ios::binary);
    out << boost::json::serialize(root);
  }

  // Five points went in; the count comes back from the index column's
  // statistics rather than from a KV that is no longer there.
  auto chromatograms = MzPeak::open(scratch.path).chromatograms();
  BOOST_TEST(chromatograms.size() == 1u);
  BOOST_TEST(chromatograms[0].time().size() == 5u);

  // No metadata table, so the descriptive record is empty rather than absent.
  BOOST_TEST(chromatograms[0].metadata().id.empty());
}

/******************************************************************************/
// Every emitted table declares its entity index as the sorted leaf.
//
// The leaf position is DERIVED from the schema, and a shape this writer does
// not recognise declares NOTHING rather than guessing at leaf 0 -- a reader may
// believe the declaration and binary search on it, and searching an unsorted
// column finds one run and silently misses the rest.  This pins the derivation
// against a future change to any table's shape.
BOOST_AUTO_TEST_CASE(every_table_declares_its_entity_index_sorted)
{
  Scratch scratch("mzp-test-sorting");

  MzPeak::RunContents run;
  MzPeak::SpectrumData s;
  s.mz = {100.0, 200.0};
  s.intensity = {1.0f, 2.0f};
  s.id = "scan=1";
  run.spectra.push_back(s);
  run.chromatograms.push_back(make_tic());
  run.wavelength_spectra.push_back(make_uv());
  MzPeak::write_run_directory(scratch.path, run);

  std::size_t checked = 0;
  for (const auto& entry : fs::directory_iterator(scratch.path)) {
    if (entry.path().extension() != ".parquet") continue;

    auto reader = parquet::ParquetFileReader::OpenFile(entry.path().string());
    auto metadata = reader->metadata();
    BOOST_TEST_REQUIRE(metadata->num_row_groups() > 0);

    const auto sorting = metadata->RowGroup(0)->sorting_columns();
    BOOST_TEST_REQUIRE(sorting.size() == 1u,
                       "no sorting column declared in " << entry.path().filename());
    BOOST_TEST(sorting[0].column_idx == 0);
    BOOST_TEST(!sorting[0].descending);
    BOOST_TEST(!sorting[0].nulls_first);
    ++checked;
  }

  // Spectra, chromatograms and UV each contribute data plus metadata plus
  // facets; a derivation that silently stopped matching would show up here as a
  // smaller count long before it showed up as a wrong answer.
  BOOST_TEST(checked >= 9u);
}

/******************************************************************************/
// A metadata column whose NAME the reader cannot guess is still found, because
// the index declares which CV term it carries.
//
// Field resolution tries the name as given, then mechanically strips the CV
// prefix and unit suffix -- which is how the reference writer's plain names are
// found today.  A writer is equally free to call the column something else and
// declare the binding in `column_mapping`; that is what the mapping is for, and
// nothing but the mapping can find such a column.  This renames a column on
// disk and rewrites the index to match, then reads it back.
BOOST_AUTO_TEST_CASE(a_column_is_found_through_column_mapping_alone)
{
  Scratch scratch("mzp-test-colmap");

  MzPeak::RunContents run;
  run.chromatograms.push_back(make_tic());
  MzPeak::write_run_directory(scratch.path, run);

  // Sanity: the type is readable before anything is renamed.
  BOOST_TEST_REQUIRE(
      MzPeak::open(scratch.path).chromatograms()[0].metadata().chromatogram_type ==
      "MS:1000235");

  // Rename `chromatogram_type` to a name no transformation could derive.
  const fs::path table = scratch.path / "chromatograms_metadata.parquet";
  auto original = arrow::io::ReadableFile::Open(table.string()).ValueOrDie();
  auto reader =
      parquet::arrow::OpenFile(original, arrow::default_memory_pool()).ValueOrDie();
  std::shared_ptr<arrow::Table> loaded;
  BOOST_TEST_REQUIRE(reader->ReadTable(&loaded).ok());

  auto fields = loaded->schema()->fields();
  int renamed = -1;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (fields[i]->name() == "chromatogram_type") {
      fields[i] = fields[i]->WithName("opt_chromatogram_class");
      renamed = static_cast<int>(i);
    }
  }
  BOOST_TEST_REQUIRE(renamed >= 0);
  auto renamed_table = arrow::Table::Make(arrow::schema(fields), loaded->columns(),
                                          loaded->num_rows());
  original->Close();

  auto sink = arrow::io::FileOutputStream::Open(table.string()).ValueOrDie();
  BOOST_TEST_REQUIRE(
      parquet::arrow::WriteTable(*renamed_table, arrow::default_memory_pool(), sink)
          .ok());
  BOOST_TEST_REQUIRE(sink->Close().ok());

  // Point the index's mapping at the new name.
  const fs::path index_path = scratch.path / "mzpeak_index.json";
  std::string json;
  {
    std::ifstream in(index_path, std::ios::binary);
    json.assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
  }
  auto root = boost::json::parse(json).as_object();
  for (auto& entry : root["files"].as_array()) {
    auto& file = entry.as_object();
    if (std::string(file.at("name").as_string()) != "chromatograms_metadata.parquet")
      continue;
    for (auto& mapping : file.at("column_mapping").as_array()) {
      auto& m = mapping.as_object();
      if (std::string(m.at("accession").as_string()) == "MS:1000626") {
        m["path"] = "opt_chromatogram_class";
      }
    }
  }
  {
    std::ofstream out(index_path, std::ios::binary);
    out << boost::json::serialize(root);
  }

  // Only the mapping can find it now.
  auto chromatograms = MzPeak::open(scratch.path).chromatograms();
  BOOST_TEST_REQUIRE(chromatograms.size() == 1u);
  BOOST_TEST(chromatograms[0].metadata().chromatogram_type == "MS:1000235");
}
