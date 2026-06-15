/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE Writer
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mzpeak/index.h"
#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/writer.h"

namespace fs = std::filesystem;

namespace {

// RAII temp directory with a unique name, removed on scope exit.
struct TempDir {
  fs::path path;
  TempDir()
      : path(fs::temp_directory_path() /
             ("mzpeak_writer_test_" + std::to_string(next_id())))
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

} // namespace

/******************************************************************************/
// The Phase-0 round trip: write a point-layout directory with the writer,
// read it back with the real reader, and confirm the m/z and intensity
// arrays of every spectrum survive exactly.
BOOST_AUTO_TEST_CASE(round_trips_through_the_reader)
{
  using namespace MzPeak;

  std::vector<SpectrumData> in{
      {{100.0, 200.5, 300.25}, {10.0f, 20.0f, 30.0f}},
      {{150.0, 250.75}, {40.0f, 50.0f}},
      {{175.5, 275.0, 375.5, 475.25}, {1.5f, 2.5f, 3.5f, 4.5f}},
  };

  TempDir dir;
  write_spectra_directory(dir.path, in);

  // Artifacts exist.
  BOOST_TEST(fs::exists(dir.path / "spectra_data.parquet"));
  BOOST_TEST(fs::exists(dir.path / "spectra_metadata.parquet"));
  BOOST_TEST(fs::exists(dir.path / "mzpeak_index.json"));

  // Read back through the real reader.
  Index index = MzPeak::open(dir.path.string());
  Spectra spectra = index.spectra();

  BOOST_TEST(spectra.size() == in.size());

  for (std::size_t i = 0; i < in.size(); ++i) {
    const auto& s = spectra[i];
    const auto& mz = s.mz();
    const auto& it = s.intensity();

    BOOST_TEST(mz.size() == in[i].mz.size());
    BOOST_TEST(it.size() == in[i].intensity.size());

    for (std::size_t j = 0; j < in[i].mz.size(); ++j) {
      BOOST_TEST(mz[j] == in[i].mz[j], boost::test_tools::tolerance(1e-9));
      BOOST_TEST(it[j] == in[i].intensity[j], boost::test_tools::tolerance(1e-6f));
    }
  }
}

/******************************************************************************/
// Empty input writes a valid (0-row) file the reader can open.
BOOST_AUTO_TEST_CASE(handles_empty_input)
{
  using namespace MzPeak;

  TempDir dir;
  write_spectra_directory(dir.path, {});

  Index index = MzPeak::open(dir.path.string());
  BOOST_TEST(index.spectra().size() == 0);
}

/******************************************************************************/
// Points within a spectrum are sorted by ascending m/z (carrying intensity
// along), so the array index's sorting_rank:0 claim is honoured.
BOOST_AUTO_TEST_CASE(sorts_points_by_mz)
{
  using namespace MzPeak;

  std::vector<SpectrumData> in{
      {{300.0, 100.0, 200.0}, {3.0f, 1.0f, 2.0f}},
  };

  TempDir dir;
  write_spectra_directory(dir.path, in);

  Index index = MzPeak::open(dir.path.string());
  auto spectra = index.spectra();
  BOOST_TEST(spectra.size() == 1);

  // Bind the spectrum first: operator[] returns by value, so mz()/intensity()
  // must reference a named spectrum, not a temporary.
  const auto& s = spectra[0];
  const auto& mz = s.mz();
  const auto& it = s.intensity();
  BOOST_TEST(mz.size() == 3);
  BOOST_TEST(mz[0] == 100.0, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(mz[1] == 200.0, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(mz[2] == 300.0, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(it[0] == 1.0f, boost::test_tools::tolerance(1e-6f)); // moved with mz
  BOOST_TEST(it[1] == 2.0f, boost::test_tools::tolerance(1e-6f));
  BOOST_TEST(it[2] == 3.0f, boost::test_tools::tolerance(1e-6f));
}

/******************************************************************************/
// Profile spectra go to spectra_data.parquet, centroid spectra to
// spectra_peaks.parquet; the reader serves both (round-trips via RDR-3).
BOOST_AUTO_TEST_CASE(splits_profile_and_centroid_spectra)
{
  using namespace MzPeak;

  std::vector<SpectrumData> in{
      {{100.0, 200.0, 300.0}, {1.0f, 2.0f, 3.0f}, /*centroid=*/false},
      {{150.0, 250.0}, {4.0f, 5.0f}, /*centroid=*/true},
      {{120.0, 220.0, 320.0, 420.0}, {6.0f, 7.0f, 8.0f, 9.0f}, /*centroid=*/false},
      {{175.0, 275.0}, {10.0f, 11.0f}, /*centroid=*/true},
  };

  TempDir dir;
  write_spectra_directory(dir.path, in);

  BOOST_TEST(fs::exists(dir.path / "spectra_data.parquet"));
  BOOST_TEST(fs::exists(dir.path / "spectra_peaks.parquet")); // centroids present
  BOOST_TEST(fs::exists(dir.path / "spectra_metadata.parquet"));

  Index index = MzPeak::open(dir.path.string());
  auto spectra = index.spectra();
  BOOST_TEST(spectra.size() == 4);

  for (std::size_t i = 0; i < in.size(); ++i) {
    const auto& s = spectra[i];
    const auto& mz = s.mz();
    BOOST_TEST(mz.size() == in[i].mz.size());
    for (std::size_t j = 0; j < in[i].mz.size(); ++j) {
      BOOST_TEST(mz[j] == in[i].mz[j], boost::test_tools::tolerance(1e-9));
    }
  }
}

/******************************************************************************/
// RDR-26: reading two spectrum tables (data + peaks) from a single C++-written
// ZIP archive works (each member gets its own archive handle, so concurrent
// zip_fseek no longer fails).
BOOST_AUTO_TEST_CASE(reads_data_and_peaks_from_one_zip_archive)
{
  using namespace MzPeak;

  std::vector<SpectrumData> in{
      {{100.0, 200.0, 300.0}, {1.0f, 2.0f, 3.0f}, /*centroid=*/false},
      {{150.0, 250.0}, {4.0f, 5.0f}, /*centroid=*/true},
      {{120.0, 220.0}, {6.0f, 7.0f}, /*centroid=*/false},
      {{175.0, 275.0, 375.0}, {8.0f, 9.0f, 10.0f}, /*centroid=*/true},
  };

  TempDir dir;
  fs::create_directories(dir.path); // TempDir only reserves the name
  fs::path archive(dir.path / "split.mzpeak");
  write_spectra_archive(archive, in);

  Index index = MzPeak::open(archive.string());
  auto spectra = index.spectra();
  BOOST_TEST(spectra.size() == 4);

  for (std::size_t i = 0; i < in.size(); ++i) {
    const auto& s = spectra[i];
    const auto& mz = s.mz();
    BOOST_TEST(mz.size() == in[i].mz.size());
    for (std::size_t j = 0; j < in[i].mz.size(); ++j) {
      BOOST_TEST(mz[j] == in[i].mz[j], boost::test_tools::tolerance(1e-9));
    }
  }
}

/******************************************************************************/
// RDR-24: the index format version is read, and an incompatible MAJOR version
// is rejected.
BOOST_AUTO_TEST_CASE(reads_and_validates_format_version)
{
  using namespace MzPeak;

  TempDir dir;
  write_spectra_directory(dir.path, {{{100.0, 200.0}, {1.0f, 2.0f}}});

  BOOST_TEST(MzPeak::open(dir.path.string()).version() == "0.9.0");

  // Tamper the index to a future major version -> open must reject it.
  fs::path index_path(dir.path / "mzpeak_index.json");
  std::string json;
  {
    std::ifstream in_file(index_path, std::ios::binary);
    std::ostringstream ss;
    ss << in_file.rdbuf();
    json = ss.str();
  }
  std::size_t at = json.find("0.9.0");
  BOOST_TEST((at != std::string::npos));
  json.replace(at, 5, "2.0.0");
  {
    std::ofstream out_file(index_path, std::ios::binary);
    out_file << json;
  }

  BOOST_CHECK_THROW(MzPeak::open(dir.path.string()), std::exception);
}

/******************************************************************************/
// TC-03: a spectrum whose mz and intensity arrays have different lengths is
// rejected at write time, before any Parquet I/O is performed.
BOOST_AUTO_TEST_CASE(rejects_mismatched_mz_intensity)
{
  using namespace MzPeak;
  TempDir dir;
  // 2 m/z values but only 1 intensity value — a clear mismatch.
  std::vector<SpectrumData> in{{{100.0, 200.0}, {10.0f}}};
  BOOST_CHECK_THROW(write_spectra_directory(dir.path, in), std::exception);
}

/******************************************************************************/
// TC-06: a directory with a single spectrum round-trips through the reader.
BOOST_AUTO_TEST_CASE(single_spectrum_round_trip)
{
  using namespace MzPeak;
  TempDir dir;
  std::vector<SpectrumData> in{{{100.0, 200.5, 300.25}, {10.0f, 20.0f, 30.0f}}};
  write_spectra_directory(dir.path, in);

  Index index = MzPeak::open(dir.path.string());
  auto spectra = index.spectra();
  BOOST_TEST(spectra.size() == 1u);

  const auto& s = spectra[0];
  const auto& mz = s.mz();
  const auto& it = s.intensity();
  BOOST_TEST(mz.size() == 3u);
  BOOST_TEST(mz[0] == 100.0, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(mz[1] == 200.5, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(mz[2] == 300.25, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(it[0] == 10.0f, boost::test_tools::tolerance(1e-6f));
  BOOST_TEST(it[1] == 20.0f, boost::test_tools::tolerance(1e-6f));
  BOOST_TEST(it[2] == 30.0f, boost::test_tools::tolerance(1e-6f));
}

/******************************************************************************/
// TC-07: a spectrum with zero data points round-trips; the second spectrum
// (index 1, 0 points) reads back with empty m/z and intensity arrays.
BOOST_AUTO_TEST_CASE(zero_data_points_round_trip)
{
  using namespace MzPeak;
  TempDir dir;
  // Mix: one spectrum with points followed by one with none.  The non-empty
  // spectrum ensures spectrum_count >= 2 so index 1 is reachable.
  std::vector<SpectrumData> in{
      {{100.0, 200.0}, {1.0f, 2.0f}},
      {{}, {}},
  };
  write_spectra_directory(dir.path, in);

  Index index = MzPeak::open(dir.path.string());
  auto spectra = index.spectra();
  BOOST_TEST(spectra.size() == 2u);

  const auto& s1 = spectra[1];
  BOOST_TEST(s1.mz().empty());
  BOOST_TEST(s1.intensity().empty());
}

/******************************************************************************/
// TC-10: the default SpectrumData has ms_level=1 and no retention time; those
// defaults survive a directory round-trip through the metadata table.
BOOST_AUTO_TEST_CASE(metadata_defaults_after_write)
{
  using namespace MzPeak;
  TempDir dir;
  std::vector<SpectrumData> in{{{100.0, 200.0}, {1.0f, 2.0f}}};
  write_spectra_directory(dir.path, in);

  Index index = MzPeak::open(dir.path.string());
  auto spectra = index.spectra();
  const auto& s = spectra[0];

  BOOST_TEST(s.ms_level().has_value());
  BOOST_TEST(s.ms_level().value() == 1);
  BOOST_TEST(!s.retention_time().has_value());
}

/******************************************************************************/
// TC-11: the point-layout writer does not produce precursor records; the
// read-back spectrum must report an empty precursors list.
BOOST_AUTO_TEST_CASE(precursors_absent_after_write)
{
  using namespace MzPeak;
  TempDir dir;
  std::vector<SpectrumData> in{{{100.0, 200.0}, {1.0f, 2.0f}}};
  write_spectra_directory(dir.path, in);

  Index index = MzPeak::open(dir.path.string());
  auto spectra = index.spectra();
  const auto& s = spectra[0];

  BOOST_TEST(s.metadata().precursors.empty());
}
