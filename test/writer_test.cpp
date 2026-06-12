/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE Writer
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

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
  ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }

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
      BOOST_TEST(it[j] == in[i].intensity[j],
                 boost::test_tools::tolerance(1e-6f));
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
