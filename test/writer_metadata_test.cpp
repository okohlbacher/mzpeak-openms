/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * WRT-2 — the writer emits run-level `metadata{}` blocks (the RDR-24
 * write-side dual).  Build a RunMetadata, write a tiny file with it, reopen
 * with MzPeak::open, and confirm Index::metadata() round-trips the run /
 * software / file_description values we wrote.  Also confirm the no-metadata
 * overload still produces a file whose Index::metadata().empty() is true.
 */
#define BOOST_TEST_MODULE WriterMetadata
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <filesystem>
#include <ranges>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "mzpeak/index.h"
#include "mzpeak/open.h"
#include "mzpeak/run_metadata.h"
#include "mzpeak/writer.h"

namespace fs = std::filesystem;
namespace json = boost::json;

namespace {

// RAII temp directory with a unique name, removed on scope exit.
struct TempDir {
  fs::path path;
  TempDir()
      : path(fs::temp_directory_path() /
             ("mzpeak_writer_metadata_test_" + std::to_string(next_id())))
  {
    std::error_code ec;
    fs::remove_all(path, ec); // start clean
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }

  static unsigned next_id()
  {
    static std::atomic<unsigned> counter{0};
    return counter.fetch_add(1);
  }
};

// A small but representative run-level metadata object, built by parsing a
// JSON object through the existing RunMetadata(boost::json::object) ctor.  It
// exercises the run block, software_list (with a CV param) and
// file_description (with a source file carrying a checksum CV param).
MzPeak::RunMetadata sample_metadata()
{
  json::object meta{
      {"run",
       {{"id", "myrun"},
        {"start_time", "2024-01-02T03:04:05Z"},
        {"default_instrument_id", 0},
        {"default_data_processing_id", "dp1"},
        {"default_source_file_id", "SRC1"}}},
      {"software_list",
       json::array{json::object{
           {"id", "mzpeak_writer"},
           {"version", "1.2.3"},
           {"parameters",
            json::array{json::object{{"accession", "MS:1000799"},
                                     {"name", "custom unreleased software tool"},
                                     {"value", "mzpeak"}}}}}}},
      {"file_description",
       {{"contents", json::array{json::object{{"accession", "MS:1000579"},
                                              {"name", "MS1 spectrum"}}}},
        {"source_files",
         json::array{json::object{
             {"id", "SRC1"},
             {"name", "input.raw"},
             {"location", "file:///data"},
             {"parameters",
              json::array{json::object{{"accession", "MS:1000569"},
                                       {"name", "SHA-1"},
                                       {"value", "deadbeefcafef00d"}}}}}}}}},
  };
  return MzPeak::RunMetadata(meta);
}

} // namespace

/******************************************************************************/
// parse(serialize(x)) == x for the typed fields: serialize a RunMetadata, parse
// it straight back, and confirm the typed accessors match.
BOOST_AUTO_TEST_CASE(serializer_round_trips_through_parser)
{
  using namespace MzPeak;

  RunMetadata original = sample_metadata();
  RunMetadata reparsed(original.to_json());

  BOOST_TEST(!reparsed.empty());

  BOOST_TEST(reparsed.run().has_value());
  BOOST_TEST((reparsed.run()->id == std::optional<std::string>("myrun")));
  BOOST_TEST((reparsed.run()->start_time ==
              std::optional<std::string>("2024-01-02T03:04:05Z")));
  BOOST_TEST(
      (reparsed.run()->default_instrument_id == std::optional<std::int64_t>(0)));
  BOOST_TEST((reparsed.run()->default_data_processing_id ==
              std::optional<std::string>("dp1")));
  BOOST_TEST((reparsed.run()->default_source_file_id ==
              std::optional<std::string>("SRC1")));

  BOOST_TEST(reparsed.software_list().size() == 1u);
  const auto& sw = reparsed.software_list().front();
  BOOST_TEST(sw.id == "mzpeak_writer");
  BOOST_TEST((sw.version == std::optional<std::string>("1.2.3")));
  BOOST_TEST(sw.parameters.size() == 1u);
  BOOST_TEST(
      (sw.parameters.front().accession == std::optional<std::string>("MS:1000799")));
  BOOST_TEST((sw.parameters.front().value == std::optional<std::string>("mzpeak")));

  BOOST_TEST(reparsed.file_description().has_value());
  const auto& fd = *reparsed.file_description();
  BOOST_TEST(fd.contents.size() == 1u);
  BOOST_TEST(fd.source_files.size() == 1u);
  const auto& src = fd.source_files.front();
  BOOST_TEST(src.id == "SRC1");
  BOOST_TEST((src.name == std::optional<std::string>("input.raw")));
  const auto sha = std::ranges::find(
      src.parameters, std::optional<std::string>("MS:1000569"), &CvParam::accession);
  BOOST_TEST((sha != src.parameters.end()), "expected SHA-1 param");
  BOOST_TEST((sha->value == std::optional<std::string>("deadbeefcafef00d")));
}

/******************************************************************************/
// The directory writer overload emits the run-level blocks, and the reader
// parses them back out of mzpeak_index.json.
BOOST_AUTO_TEST_CASE(directory_writer_round_trips_metadata)
{
  using namespace MzPeak;

  std::vector<SpectrumData> in{
      {{100.0, 200.0, 300.0}, {1.0f, 2.0f, 3.0f}},
      {{150.0, 250.0}, {4.0f, 5.0f}},
  };

  TempDir dir;
  write_spectra_directory(dir.path, in, sample_metadata());

  Index index = MzPeak::open(dir.path.string());
  const auto& meta = index.metadata();

  BOOST_TEST(!meta.empty(), "written metadata blocks should parse back");

  BOOST_TEST(meta.run().has_value());
  BOOST_TEST((meta.run()->id == std::optional<std::string>("myrun")));
  BOOST_TEST(
      (meta.run()->default_source_file_id == std::optional<std::string>("SRC1")));

  BOOST_TEST(meta.software_list().size() == 1u);
  BOOST_TEST(meta.software_list().front().id == "mzpeak_writer");
  BOOST_TEST(
      (meta.software_list().front().version == std::optional<std::string>("1.2.3")));

  BOOST_TEST(meta.file_description().has_value());
  BOOST_TEST(meta.file_description()->source_files.size() == 1u);
  BOOST_TEST(meta.file_description()->source_files.front().id == "SRC1");

  // The spectra still round-trip alongside the metadata.
  BOOST_TEST(index.spectra().size() == in.size());
}

/******************************************************************************/
// The archive writer overload likewise embeds the metadata in the STORED
// mzpeak_index.json.
BOOST_AUTO_TEST_CASE(archive_writer_round_trips_metadata)
{
  using namespace MzPeak;

  std::vector<SpectrumData> in{
      {{100.0, 200.0}, {1.0f, 2.0f}},
  };

  TempDir dir;
  fs::create_directories(dir.path); // TempDir only reserves the name
  fs::path archive(dir.path / "with_meta.mzpeak");
  write_spectra_archive(archive, in, sample_metadata());

  Index index = MzPeak::open(archive.string());
  const auto& meta = index.metadata();

  BOOST_TEST(!meta.empty());
  BOOST_TEST(meta.run().has_value());
  BOOST_TEST((meta.run()->id == std::optional<std::string>("myrun")));
  BOOST_TEST(meta.software_list().size() == 1u);
  BOOST_TEST(meta.software_list().front().id == "mzpeak_writer");
}

/******************************************************************************/
// The no-metadata overload is unchanged: it emits a metadata{} carrying only
// version, so Index::metadata().empty() is true.
BOOST_AUTO_TEST_CASE(no_metadata_overload_stays_empty)
{
  using namespace MzPeak;

  std::vector<SpectrumData> in{
      {{100.0, 200.0}, {1.0f, 2.0f}},
  };

  TempDir dir;
  write_spectra_directory(dir.path, in);

  Index index = MzPeak::open(dir.path.string());
  BOOST_TEST(index.metadata().empty(),
             "no-metadata overload must not emit run-level blocks");
  BOOST_TEST(index.spectra().size() == in.size());
}
